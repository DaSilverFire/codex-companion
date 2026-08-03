#include "updater-helper/UpdateInstallTransaction.h"

#include <QDir>
#include <QStandardPaths>
#include <QtTest>
#include <QUuid>

#include <chrono>
#include <memory>

namespace {

companion::CompanionError failure(
    QString code)
{
    return {
        std::move(code),
        QStringLiteral("injected failure"),
        false,
        {},
    };
}

companion::UpdateInstallRequest
validRequest()
{
    companion::UpdateInstallRequest
        request;
    request.requestId =
        QUuid::createUuid()
            .toString(
                QUuid::WithoutBraces);
    request.installerPath =
        QDir(
            QStandardPaths::
                writableLocation(
                    QStandardPaths::
                        GenericCacheLocation))
            .filePath(
                QStringLiteral(
                    "CodexCompanionUpdaterTests/"
                    "installer.exe"));
    request.expectedSha256 =
        QString(64, QLatin1Char('a'));
    request.expectedSize = 4'200;
    request.expectedVersion =
        QStringLiteral("0.3.5");
    request.expectedBuild = 2;
    request.installRoot =
        companion::UpdateInstallRequest::
            expectedInstallRoot();
    request.rollbackRoot =
        request.installRoot
        + QStringLiteral(".rollback.")
        + request.requestId;
    request.uninstallRegistryKey =
        companion::UpdateInstallRequest::
            expectedUninstallRegistryKey();
    request.startMenuShortcut =
        companion::UpdateInstallRequest::
            expectedStartMenuShortcut();
    request.acknowledgementEvent =
        companion::UpdateInstallRequest::
            acknowledgementEventFor(
                request.requestId);
    request.parentProcessId = 77;
    return request;
}

struct Harness final {
    QStringList events;
    companion::
        UpdateInstallTransactionDependencies
            dependencies;
    companion::
        UpdateInstallStateSnapshot snapshot;
    QString failAt;
    int installerExitCode = 0;
    quint32 launchedNewProcessId = 901;
    int oldLaunchCount = 0;
    int newLaunchCount = 0;
    std::chrono::milliseconds
        observedParentTimeout{0};
    std::chrono::milliseconds
        observedAckTimeout{0};

    Harness()
    {
        snapshot.uninstallRegistry.existed =
            true;
        snapshot.startMenuShortcut.existed =
            true;

        dependencies
            .prepareAcknowledgement =
            [this](QStringView) {
                events.append(
                    QStringLiteral(
                        "prepare-ack"));
                if (failAt
                    == QStringLiteral(
                        "prepare-ack")) {
                    return companion::Result<
                        companion::
                            UpdateAcknowledgementHandle>::
                        failure(
                            failure(
                                failAt));
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
        dependencies.waitForParentExit =
            [this](
                quint32,
                std::chrono::milliseconds
                    timeout) {
                events.append(
                    QStringLiteral(
                        "wait-parent"));
                observedParentTimeout =
                    timeout;
                return result(
                    QStringLiteral(
                        "wait-parent"));
            };
        dependencies.signalReady =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "signal-ready"));
                return result(
                    QStringLiteral(
                        "signal-ready"));
            };
        dependencies.revalidateInstaller =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "revalidate"));
                return result(
                    QStringLiteral(
                        "revalidate"));
            };
        dependencies.snapshotState =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "snapshot"));
                if (failAt
                    == QStringLiteral(
                        "snapshot")) {
                    return companion::Result<
                        companion::
                            UpdateInstallStateSnapshot>::
                        failure(
                            failure(
                                failAt));
                }
                return companion::Result<
                    companion::
                        UpdateInstallStateSnapshot>::
                    success(snapshot);
            };
        dependencies
            .moveInstallToRollback =
            [this](const auto&) {
                events.append(
                    QStringLiteral("move"));
                return result(
                    QStringLiteral("move"));
            };
        dependencies.runInstaller =
            [this](
                const auto&,
                QStringView logPath) {
                events.append(
                    QStringLiteral(
                        "installer"));
                if (!logPath.endsWith(
                        QStringLiteral(
                            "installer.log"))) {
                    return companion::Result<
                        int>::failure(
                            failure(
                                QStringLiteral(
                                    "bad-log-path")));
                }
                if (failAt
                    == QStringLiteral(
                        "installer")) {
                    return companion::Result<
                        int>::failure(
                            failure(
                                failAt));
                }
                return companion::Result<
                    int>::success(
                        installerExitCode);
            };
        dependencies.verifyInstalledTree =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "verify-installed"));
                return result(
                    QStringLiteral(
                        "verify-installed"));
            };
        dependencies.afterReplacement =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "after-replacement"));
                return result(
                    QStringLiteral(
                        "after-replacement"));
            };
        dependencies.launchApplication =
            [this](
                QStringView,
                const QStringList&
                    arguments,
                QStringView) {
                if (arguments.isEmpty()) {
                    events.append(
                        QStringLiteral(
                            "launch-old"));
                    ++oldLaunchCount;
                    if (failAt
                        == QStringLiteral(
                            "launch-old")) {
                        return companion::
                            Result<quint32>::
                            failure(
                                failure(
                                    failAt));
                    }
                    return companion::
                        Result<quint32>::
                        success(902);
                }
                events.append(
                    QStringLiteral(
                        "launch-new"));
                ++newLaunchCount;
                if (failAt
                    == QStringLiteral(
                        "launch-new")) {
                    return companion::
                        Result<quint32>::
                        failure(
                            failure(
                                failAt));
                }
                return companion::
                    Result<quint32>::
                    success(
                        launchedNewProcessId);
            };
        dependencies
            .waitForAcknowledgement =
            [this](
                const auto&,
                std::chrono::milliseconds
                    timeout) {
                events.append(
                    QStringLiteral(
                        "wait-ack"));
                observedAckTimeout =
                    timeout;
                return result(
                    QStringLiteral(
                        "wait-ack"));
            };
        dependencies.terminateProcess =
            [this](quint32 processId) {
                events.append(
                    QStringLiteral(
                        "terminate-%1")
                        .arg(processId));
                return result(
                    QStringLiteral(
                        "terminate"));
            };
        dependencies.restoreRollback =
            [this](
                const auto&,
                const auto& restored) {
                events.append(
                    QStringLiteral(
                        "restore"));
                if (restored != snapshot) {
                    return companion::
                        Result<void>::
                        failure(
                            failure(
                                QStringLiteral(
                                    "wrong-snapshot")));
                }
                return result(
                    QStringLiteral(
                        "restore"));
            };
        dependencies.commitRollback =
            [this](const auto&) {
                events.append(
                    QStringLiteral(
                        "commit"));
                return result(
                    QStringLiteral(
                        "commit"));
            };
    }

    companion::Result<void> result(
        const QString& operation)
    {
        return failAt == operation
            ? companion::Result<void>::
                  failure(
                      failure(operation))
            : companion::Result<void>::
                  success();
    }
};

companion::UpdateInstallTransaction
transaction(Harness& harness)
{
    companion::
        UpdateInstallTransactionOptions
            options;
    options.transactionRoot =
        QStringLiteral(
            "C:/Temp/CodexCompanionUpdater/"
            "request");
    return {
        std::move(options),
        harness.dependencies,
    };
}

} // namespace

class UpdateInstallTransactionTests final
    : public QObject {
    Q_OBJECT

private slots:
    void successCommitsAfterAcknowledgement()
    {
        Harness harness;
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY2(
            result.hasValue(),
            qPrintable(
                result.hasValue()
                    ? QString()
                    : result.error()
                          .message));
        QCOMPARE(
            harness.events,
            QStringList({
                QStringLiteral(
                    "prepare-ack"),
                QStringLiteral(
                    "signal-ready"),
                QStringLiteral(
                    "wait-parent"),
                QStringLiteral(
                    "revalidate"),
                QStringLiteral(
                    "snapshot"),
                QStringLiteral("move"),
                QStringLiteral(
                    "installer"),
                QStringLiteral(
                    "verify-installed"),
                QStringLiteral(
                    "after-replacement"),
                QStringLiteral(
                    "launch-new"),
                QStringLiteral(
                    "wait-ack"),
                QStringLiteral(
                    "commit"),
            }));
        QCOMPARE(
            harness.oldLaunchCount,
            0);
        QCOMPARE(
            harness.newLaunchCount,
            1);
        QCOMPARE(
            harness.observedParentTimeout,
            std::chrono::seconds(30));
        QCOMPARE(
            harness.observedAckTimeout,
            std::chrono::seconds(20));
    }

    void validationFailureDoesNotTouchInstallTree()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral("revalidate");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "revalidate"));
        QCOMPARE(
            harness.events,
            QStringList({
                QStringLiteral(
                    "prepare-ack"),
                QStringLiteral(
                    "signal-ready"),
                QStringLiteral(
                    "wait-parent"),
                QStringLiteral(
                    "revalidate"),
            }));
        QCOMPARE(
            harness.oldLaunchCount,
            0);
    }

    void parentTimeoutDoesNotValidateOrTouchTree()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral(
                "wait-parent");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            harness.events,
            QStringList({
                QStringLiteral(
                    "prepare-ack"),
                QStringLiteral(
                    "signal-ready"),
                QStringLiteral(
                    "wait-parent"),
            }));
    }

    void installerExitFailureRestoresAndRelaunchesOldOnce()
    {
        Harness harness;
        harness.installerExitCode = 5;
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "update.installer_exit_failed"));
        QVERIFY(
            harness.events.contains(
                QStringLiteral(
                    "restore")));
        QCOMPARE(
            harness.events.constLast(),
            QStringLiteral(
                "launch-old"));
        QCOMPARE(
            harness.oldLaunchCount,
            1);
        QCOMPARE(
            harness.newLaunchCount,
            0);
    }

    void missingInstalledTreeRollsBack()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral(
                "verify-installed");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "verify-installed"));
        QCOMPARE(
            harness.oldLaunchCount,
            1);
        QCOMPARE(
            harness.newLaunchCount,
            0);
    }

    void injectedPostReplacementFailureRollsBack()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral(
                "after-replacement");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "after-replacement"));
        QCOMPARE(
            harness.oldLaunchCount,
            1);
        QCOMPARE(
            harness.newLaunchCount,
            0);
    }

    void acknowledgementFailureStopsNewAndRestoresOld()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral("wait-ack");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "wait-ack"));
        QVERIFY(
            harness.events.contains(
                QStringLiteral(
                    "terminate-901")));
        QCOMPARE(
            harness.oldLaunchCount,
            1);
        QCOMPARE(
            harness.newLaunchCount,
            1);
        QVERIFY(
            !harness.events.contains(
                QStringLiteral(
                    "commit")));
    }

    void rollbackFailureIsReportedWithOriginalCause()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral("restore");
        harness.installerExitCode = 5;
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral(
                "update.rollback_failed"));
        QCOMPARE(
            result.error()
                .context.value(
                    QStringLiteral(
                        "causeCode"))
                .toString(),
            QStringLiteral(
                "update.installer_exit_failed"));
        QCOMPARE(
            result.error()
                .context.value(
                    QStringLiteral(
                        "rollbackCode"))
                .toString(),
            QStringLiteral("restore"));
        QCOMPARE(
            harness.oldLaunchCount,
            1);
    }

    void commitFailureDoesNotRollbackAcknowledgedApp()
    {
        Harness harness;
        harness.failAt =
            QStringLiteral("commit");
        auto updater =
            transaction(harness);

        const auto result =
            updater.run(validRequest());
        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("commit"));
        QVERIFY(
            !harness.events.contains(
                QStringLiteral(
                    "restore")));
        QCOMPARE(
            harness.oldLaunchCount,
            0);
        QCOMPARE(
            harness.newLaunchCount,
            1);
    }
};

QTEST_GUILESS_MAIN(
    UpdateInstallTransactionTests)

#include "UpdateInstallTransactionTests.moc"
