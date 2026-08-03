#include "updater-helper/UpdateInstallRequest.h"

#include <limits>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <QUuid>

namespace {

QString localRoot()
{
    const QString base =
        QStandardPaths::writableLocation(
            QStandardPaths::
                GenericCacheLocation);
    return QDir::cleanPath(
        QDir(base).filePath(
            QStringLiteral(
                "CodexCompanionRequestTests")));
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
        QDir(localRoot()).filePath(
            QStringLiteral(
                "updates/update.exe"));
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
    request.parentProcessId = 4242;
    return request;
}

QByteArray withField(
    const companion::UpdateInstallRequest&
        request,
    const QString& field,
    const QJsonValue& value)
{
    QJsonObject object =
        QJsonDocument::fromJson(
            request.encode())
            .object();
    object.insert(field, value);
    return QJsonDocument(
               std::move(object))
        .toJson(
            QJsonDocument::Compact);
}

QString decodeCode(
    QByteArrayView bytes)
{
    const auto decoded =
        companion::UpdateInstallRequest::
            decode(bytes);
    return decoded.hasValue()
        ? QStringLiteral("<success>")
        : decoded.error().code;
}

} // namespace

class UpdateInstallRequestTests final
    : public QObject {
    Q_OBJECT

private slots:
    void roundTripsCanonicalRequest()
    {
        const auto request =
            validRequest();
        QVERIFY2(
            request.validate().hasValue(),
            "valid request rejected");

        const auto decoded =
            companion::UpdateInstallRequest::
                decode(request.encode());
        QVERIFY2(
            decoded.hasValue(),
            qPrintable(
                decoded.hasValue()
                    ? QString()
                    : decoded.error()
                          .message));
        QCOMPARE(
            decoded.value(),
            request);
    }

    void writesAndLoadsAtomically()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString requestPath =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "handoff/request.json"));
        const auto request =
            validRequest();

        QVERIFY2(
            request.writeAtomically(
                       requestPath)
                .hasValue(),
            "atomic request write failed");
        const auto loaded =
            companion::UpdateInstallRequest::
                load(requestPath);
        QVERIFY2(
            loaded.hasValue(),
            qPrintable(
                loaded.hasValue()
                    ? QString()
                    : loaded.error()
                          .message));
        QCOMPARE(
            loaded.value(),
            request);
        QVERIFY(!QFile::exists(
            requestPath
            + QStringLiteral(".tmp")));
    }

    void rejectsMalformedDuplicateMissingAndUnknownFields()
    {
        const auto request =
            validRequest();
        QCOMPARE(
            decodeCode(
                QByteArrayLiteral("{")),
            QStringLiteral(
                "update.install_request_invalid"));

        const QByteArray encoded =
            request.encode();
        QByteArray duplicate =
            QByteArrayLiteral(
                "{\"requestId\":\"")
            + request.requestId.toUtf8()
            + QByteArrayLiteral("\",")
            + encoded.sliced(1);
        QCOMPARE(
            decodeCode(duplicate),
            QStringLiteral(
                "update.install_request_invalid"));

        QJsonObject missing =
            QJsonDocument::fromJson(
                encoded)
                .object();
        missing.remove(
            QStringLiteral(
                "expectedBuild"));
        QCOMPARE(
            decodeCode(
                QJsonDocument(missing)
                    .toJson(
                        QJsonDocument::
                            Compact)),
            QStringLiteral(
                "update.install_request_invalid"));

        QJsonObject unknown =
            QJsonDocument::fromJson(
                encoded)
                .object();
        unknown.insert(
            QStringLiteral("future"),
            true);
        QCOMPARE(
            decodeCode(
                QJsonDocument(unknown)
                    .toJson(
                        QJsonDocument::
                            Compact)),
            QStringLiteral(
                "update.install_request_invalid"));
    }

    void rejectsWrongTypesAndNonIntegralNumbers()
    {
        const auto request =
            validRequest();
        QCOMPARE(
            decodeCode(
                withField(
                    request,
                    QStringLiteral(
                        "installerPath"),
                    42)),
            QStringLiteral(
                "update.install_request_invalid"));
        QCOMPARE(
            decodeCode(
                withField(
                    request,
                    QStringLiteral(
                        "expectedSize"),
                    1.5)),
            QStringLiteral(
                "update.install_request_invalid"));
        QCOMPARE(
            decodeCode(
                withField(
                    request,
                    QStringLiteral(
                        "parentProcessId"),
                    static_cast<double>(
                        std::numeric_limits<
                            quint32>::max())
                        + 1.0)),
            QStringLiteral(
                "update.install_request_invalid"));
    }

    void rejectsInvalidIdentityDigestVersionAndProcess()
    {
        auto request = validRequest();
        request.requestId =
            QStringLiteral("not-a-uuid");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.expectedSha256 =
            QString(63, QLatin1Char('a'));
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.expectedSha256 =
            QString(64, QLatin1Char('A'));
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.expectedVersion =
            QStringLiteral("release");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.expectedBuild = 0;
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.parentProcessId = 0;
        QVERIFY(
            !request.validate()
                 .hasValue());
    }

    void rejectsUnsafeArtifactAndInstallPaths()
    {
        auto request = validRequest();
        request.installerPath =
            QStringLiteral(
                "relative/update.exe");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.installerPath =
            request.installerPath
            + QStringLiteral("/../update.exe");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.installerPath.chop(4);
        request.installerPath +=
            QStringLiteral(".zip");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.rollbackRoot =
            QDir(localRoot()).filePath(
                QStringLiteral(
                    "OtherParent/"
                    "Codex Companion.rollback."))
            + request.requestId;
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.rollbackRoot =
            request.installRoot;
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.rollbackRoot =
            request.installRoot
            + QStringLiteral(
                ".backup");
        QVERIFY(
            !request.validate()
                 .hasValue());
    }

    void rejectsShellIntegrationAndEventDrift()
    {
        auto request = validRequest();
        request.uninstallRegistryKey +=
            QStringLiteral("-other");
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.startMenuShortcut =
            QDir(localRoot()).filePath(
                QStringLiteral(
                    "Codex Companion.lnk"));
        QVERIFY(
            !request.validate()
                 .hasValue());

        request = validRequest();
        request.acknowledgementEvent +=
            QStringLiteral("-other");
        QVERIFY(
            !request.validate()
                 .hasValue());
    }

    void rejectsInstallTreeOverlappingUserData()
    {
        auto request = validRequest();
        request.installRoot =
            QStandardPaths::
                writableLocation(
                    QStandardPaths::
                        AppLocalDataLocation);
        request.rollbackRoot =
            request.installRoot
            + QStringLiteral(".rollback.")
            + request.requestId;
        QVERIFY(
            !request.validate()
                 .hasValue());
    }

    void rejectsOversizedRequestFile()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString requestPath =
            QDir(temporary.path())
                .filePath(
                    QStringLiteral(
                        "request.json"));
        QFile file(requestPath);
        QVERIFY(file.open(
            QIODevice::WriteOnly));
        QCOMPARE(
            file.write(
                QByteArray(
                    64 * 1024 + 1,
                    'x')),
            qint64(64 * 1024 + 1));
        file.close();

        const auto loaded =
            companion::UpdateInstallRequest::
                load(requestPath);
        QVERIFY(!loaded.hasValue());
        QCOMPARE(
            loaded.error().code,
            QStringLiteral(
                "update.install_request_invalid"));
    }
};

QTEST_GUILESS_MAIN(
    UpdateInstallRequestTests)

#include "UpdateInstallRequestTests.moc"
