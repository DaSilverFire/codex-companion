#include "updater-helper/UpdateInstallerHandoff.h"

#include <QDir>
#include <QStandardPaths>
#include <QtTest>
#include <QUuid>

#include <memory>

namespace {

companion::UpdateManifest manifest()
{
    companion::UpdateManifest value;
    value.schemaVersion = 1;
    value.version =
        QStringLiteral("0.3.5");
    value.build = 2;
    value.minimumSystemVersion =
        QStringLiteral("10.0.22000");
    value.publishedAt =
        QStringLiteral(
            "2026-07-25T12:00:00Z");
    value.downloadUrl =
        QStringLiteral(
            "https://updates.example.test/"
            "installer.exe");
    value.sha256 =
        QString(64, QLatin1Char('a'));
    value.size = 4'200;
    value.signature =
        QStringLiteral("signature");
    return value;
}

companion::VerifiedArtifact artifact()
{
    companion::VerifiedArtifact value;
    value.path =
        QDir(
            QStandardPaths::
                writableLocation(
                    QStandardPaths::
                        GenericCacheLocation))
            .filePath(
                QStringLiteral(
                    "CodexCompanionHandoffTests/"
                    "installer.exe"));
    value.size = 4'200;
    value.sha256 =
        QByteArray(32, '\xAA');
    return value;
}

struct Harness final {
    QStringList events;
    QStringList sources;
    QStringList destinations;
    companion::UpdateInstallRequest
        writtenRequest;
    QString requestPath;
    QString launchedPath;
    QStringList launchedArguments;
    QString launchedWorkingDirectory;
    QString failAt;
    quint32 helperProcessId = 812;
    int terminateCalls = 0;
    int cleanupCalls = 0;
    std::chrono::milliseconds
        observedReadyTimeout{0};
    companion::
        UpdateInstallerHandoffDependencies
            dependencies;

    Harness()
    {
        dependencies
            .createTransactionDirectory =
            [this](QStringView path) {
                events.append(
                    QStringLiteral(
                        "create-directory"));
                destinations.append(
                    path.toString());
                return result(
                    QStringLiteral(
                        "create-directory"));
            };
        dependencies.copyFile =
            [this](
                QStringView source,
                QStringView destination) {
                events.append(
                    QStringLiteral(
                        "copy"));
                sources.append(
                    source.toString());
                destinations.append(
                    destination
                        .toString());
                return result(
                    QStringLiteral("copy"));
            };
        dependencies.writeRequest =
            [this](
                const companion::
                    UpdateInstallRequest&
                    request,
                QStringView path) {
                events.append(
                    QStringLiteral(
                        "write-request"));
                writtenRequest = request;
                requestPath =
                    path.toString();
                return result(
                    QStringLiteral(
                        "write-request"));
            };
        dependencies.prepareEvent =
            [this](QStringView name) {
                events.append(
                    QStringLiteral(
                        "event:%1")
                        .arg(name));
                if (failAt
                    == QStringLiteral(
                        "event")) {
                    return companion::Result<
                        companion::
                            UpdateAcknowledgementHandle>::
                        failure(error());
                }
                return companion::Result<
                    companion::
                        UpdateAcknowledgementHandle>::
                    success(
                        std::static_pointer_cast<
                            void>(
                            std::make_shared<
                                int>(1)));
            };
        dependencies.launchHelper =
            [this](
                QStringView path,
                const QStringList&
                    arguments,
                QStringView working) {
                events.append(
                    QStringLiteral(
                        "launch"));
                launchedPath =
                    path.toString();
                launchedArguments =
                    arguments;
                launchedWorkingDirectory =
                    working.toString();
                if (failAt
                    == QStringLiteral(
                        "launch")) {
                    return companion::
                        Result<quint32>::
                        failure(error());
                }
                return companion::
                    Result<quint32>::
                    success(
                        helperProcessId);
            };
        dependencies.waitForEvent =
            [this](
                const auto&,
                std::chrono::milliseconds
                    timeout) {
                events.append(
                    QStringLiteral(
                        "wait-ready"));
                observedReadyTimeout =
                    timeout;
                return result(
                    QStringLiteral(
                        "wait-ready"));
            };
        dependencies.terminateProcess =
            [this](quint32 processId) {
                events.append(
                    QStringLiteral(
                        "terminate:%1")
                        .arg(processId));
                ++terminateCalls;
                return companion::
                    Result<void>::success();
            };
        dependencies
            .cleanupTransactionDirectory =
            [this](QStringView) {
                events.append(
                    QStringLiteral(
                        "cleanup"));
                ++cleanupCalls;
            };
    }

    companion::CompanionError error()
        const
    {
        return {
            failAt,
            QStringLiteral(
                "injected failure"),
            false,
            {},
        };
    }

    companion::Result<void> result(
        const QString& operation)
    {
        return failAt == operation
            ? companion::Result<void>::
                  failure(error())
            : companion::Result<void>::
                  success();
    }
};

companion::UpdateInstallerHandoff
handoff(Harness& harness)
{
    companion::
        UpdateInstallerHandoffOptions
            options;
    options.applicationDirectory =
        QStringLiteral(
            "C:/Program Files/"
            "Codex Companion/bin");
    options.temporaryRoot =
        QStringLiteral(
            "C:/Temp/"
            "CodexCompanionUpdater");
    options.runtimeFileNames = {
        QStringLiteral(
            "CodexCompanionUpdater.exe"),
        QStringLiteral("Qt6Core.dll"),
        QStringLiteral("MSVCP140.dll"),
    };
    options.idFactory = [] {
        return QUuid(
            QStringLiteral(
                "{11111111-2222-3333-4444-"
                "555555555555}"));
    };
    options.currentProcessId = [] {
        return quint32(4242);
    };
    return {
        std::move(options),
        harness.dependencies,
    };
}

} // namespace

class UpdateInstallerHandoffTests final
    : public QObject {
    Q_OBJECT

private slots:
    void copiesRuntimeWritesRequestAndWaitsForReady()
    {
        Harness harness;
        auto launcher =
            handoff(harness);

        const auto launched =
            launcher.launch(
                manifest(),
                artifact());
        QVERIFY2(
            launched.hasValue(),
            qPrintable(
                launched.hasValue()
                    ? QString()
                    : launched.error()
                          .message));
        QCOMPARE(
            launched.value()
                .helperProcessId,
            quint32(812));
        QCOMPARE(
            harness.sources.size(),
            3);
        QCOMPARE(
            QFileInfo(
                harness.sources.at(0))
                .fileName(),
            QStringLiteral(
                "CodexCompanionUpdater.exe"));
        QCOMPARE(
            harness.writtenRequest
                .expectedVersion,
            QStringLiteral("0.3.5"));
        QCOMPARE(
            harness.writtenRequest
                .expectedBuild,
            qint64(2));
        QCOMPARE(
            harness.writtenRequest
                .expectedSha256,
            QString(64, QLatin1Char('a')));
        QCOMPARE(
            harness.writtenRequest
                .parentProcessId,
            quint32(4242));
        QCOMPARE(
            harness.launchedArguments,
            QStringList({
                QStringLiteral(
                    "--request"),
                harness.requestPath,
            }));
        QCOMPARE(
            harness.launchedWorkingDirectory,
            launched.value()
                .transactionRoot);
        QCOMPARE(
            harness.observedReadyTimeout,
            std::chrono::seconds(10));
        QCOMPARE(
            harness.cleanupCalls,
            0);
        QCOMPARE(
            harness.terminateCalls,
            0);
    }

    void copyFailureCleansWithoutLaunching()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral("copy");
        auto launcher =
            handoff(harness);

        const auto launched =
            launcher.launch(
                manifest(),
                artifact());
        QVERIFY(!launched.hasValue());
        QCOMPARE(
            harness.cleanupCalls,
            1);
        QVERIFY(
            !harness.events.contains(
                QStringLiteral(
                    "launch")));
    }

    void readyTimeoutTerminatesAndCleans()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral(
                "wait-ready");
        auto launcher =
            handoff(harness);

        const auto launched =
            launcher.launch(
                manifest(),
                artifact());
        QVERIFY(!launched.hasValue());
        QCOMPARE(
            harness.terminateCalls,
            1);
        QCOMPARE(
            harness.cleanupCalls,
            1);
        QVERIFY(
            harness.events.contains(
                QStringLiteral(
                    "terminate:812")));
    }

    void invalidArtifactFactsFailBeforeDirectoryCreation()
    {
        Harness harness;
        auto launcher =
            handoff(harness);
        auto badArtifact =
            artifact();
        badArtifact.sha256.clear();

        const auto launched =
            launcher.launch(
                manifest(),
                badArtifact);
        QVERIFY(!launched.hasValue());
        QVERIFY(
            harness.events.isEmpty()
            || harness.events
                   == QStringList({
                       QStringLiteral(
                           "create-directory"),
                       QStringLiteral(
                           "copy"),
                       QStringLiteral(
                           "copy"),
                       QStringLiteral(
                           "copy"),
                       QStringLiteral(
                           "cleanup"),
                   }));
    }
};

QTEST_GUILESS_MAIN(
    UpdateInstallerHandoffTests)

#include "UpdateInstallerHandoffTests.moc"
