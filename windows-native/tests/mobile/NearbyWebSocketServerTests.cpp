#include "mobile/nearby/NearbyWebSocketServer.h"
#include "codex/models/BridgeJsonCodec.h"
#include "mobile/nearby/NearbyFrameCodec.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/SecretProtector.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPromise>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>
#include <QtTest>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketProtocol>

namespace {

using namespace companion;

constexpr qint64 kNowMilliseconds =
    1'780'000'000'123;

class TestProtector final
    : public SecretProtector {
public:
    Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView) const override
    {
        return Result<QByteArray>::success(
            plaintext.toByteArray());
    }

    Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView) const override
    {
        return Result<QByteArray>::success(
            protectedData.toByteArray());
    }
};

QSslConfiguration testTlsConfiguration()
{
    const QString fixtureRoot =
        QStringLiteral(
            COMPANION_QTWEBSOCKET_FIXTURE_ROOT);
    QFile certificateFile(
        fixtureRoot
        + QStringLiteral(
            "/localhost.cert"));
    QFile keyFile(
        fixtureRoot
        + QStringLiteral(
            "/localhost.key"));
    if (!certificateFile.open(
            QIODevice::ReadOnly)
        || !keyFile.open(
            QIODevice::ReadOnly)) {
        return {};
    }

    const QSslCertificate certificate(
        &certificateFile,
        QSsl::Pem);
    const QSslKey key(
        &keyFile,
        QSsl::Rsa,
        QSsl::Pem);
    QSslConfiguration configuration =
        QSslConfiguration::
            defaultConfiguration();
    configuration.setPeerVerifyMode(
        QSslSocket::VerifyNone);
    configuration.setLocalCertificate(
        certificate);
    configuration.setPrivateKey(key);
    return configuration;
}

QDateTime fixedNow()
{
    return QDateTime::
        fromMSecsSinceEpoch(
            kNowMilliseconds,
            QTimeZone::UTC);
}

QByteArray repeatedSecret(char value)
{
    return QByteArray(32, value);
}

QFuture<BridgeResponse> readyResponse(
    BridgeResponse response)
{
    QPromise<BridgeResponse> promise;
    promise.start();
    QFuture<BridgeResponse> future =
        promise.future();
    promise.addResult(
        std::move(response));
    promise.finish();
    return future;
}

QByteArray encodeInvitation(
    const BridgeInvitation& invitation)
{
    QJsonObject object{
        {
            QStringLiteral("version"),
            invitation.version,
        },
        {
            QStringLiteral("deviceID"),
            invitation.deviceId,
        },
        {
            QStringLiteral("displayName"),
            invitation.displayName,
        },
        {
            QStringLiteral(
                "issuedAtMilliseconds"),
            static_cast<double>(
                invitation
                    .issuedAtMilliseconds),
        },
        {
            QStringLiteral("nonce"),
            QString::fromLatin1(
                invitation.nonce
                    .toBase64()),
        },
    };
    if (invitation.authenticator
            .has_value()) {
        object.insert(
            QStringLiteral(
                "authenticator"),
            QString::fromLatin1(
                invitation
                    .authenticator
                    ->toBase64()));
    }
    if (invitation.pairingCode
            .has_value()) {
        object.insert(
            QStringLiteral(
                "pairingCode"),
            *invitation.pairingCode);
    }
    return QJsonDocument(object)
        .toJson(
            QJsonDocument::Compact);
}

QString certificateFingerprint(
    const QSslConfiguration& configuration)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            configuration
                .localCertificate()
                .toDer(),
            QCryptographicHash::Sha256)
            .toHex());
}

QUrl serverUrl(
    quint16 port,
    QString scheme,
    QString path)
{
    QUrl url;
    url.setScheme(std::move(scheme));
    url.setHost(
        QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(std::move(path));
    return url;
}

void configureTestTlsClient(
    QWebSocket& socket)
{
    QSslConfiguration configuration =
        socket.sslConfiguration();
    configuration.setPeerVerifyMode(
        QSslSocket::VerifyNone);
    socket.setSslConfiguration(
        configuration);
    QObject::connect(
        &socket,
        &QWebSocket::sslErrors,
        &socket,
        qOverload<>(
            &QWebSocket::
                ignoreSslErrors));
}

} // namespace

class NearbyWebSocketServerTests final
    : public QObject {
    Q_OBJECT

private slots:
    void advertisesOnlyPrivateAndDomainNetworksUsingActualTlsIdentity()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());
        QVERIFY(!tls.privateKey().isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        PairingCoordinator pairing(
            pairingStore);

        QVector<NearbyServiceAdvertisement>
            published;
        int withdrawals = 0;
        NearbyWebSocketServerOptions options;
        options.sslConfiguration = tls;
        options.installationId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        options.computerName =
            QString(80, QLatin1Char('A'));
        options.hostDisplayName =
            QStringLiteral(
                "Codex Companion Windows");
        options.listenAddress =
            QHostAddress::LocalHost;
        options.transferRootPath =
            directory.filePath(
                QStringLiteral(
                    "incoming"));
        options.publishAdvertisement =
            [&published](
                const NearbyServiceAdvertisement&
                    advertisement) {
                published.append(
                    advertisement);
                return Result<void>::success();
            };
        options.withdrawAdvertisement =
            [&withdrawals]() {
                ++withdrawals;
            };

        NearbyWebSocketServer server(
            pairing,
            [](QString, BridgeRequest request) {
                BridgeResponse response;
                response.id = request.id;
                response.operation =
                    request.operation;
                response.succeeded = true;
                QPromise<BridgeResponse>
                    promise;
                promise.start();
                const auto future =
                    promise.future();
                promise.addResult(
                    std::move(response));
                promise.finish();
                return future;
            },
            std::move(options));

        const auto started = server.start();
        QVERIFY2(
            started.hasValue(),
            qPrintable(
                started.hasValue()
                    ? QString()
                    : started.error().code));
        QVERIFY(server.isListening());
        QVERIFY(server.serverPort() > 0);
        QCOMPARE(published.size(), 0);
        QCOMPARE(withdrawals, 0);

        QVERIFY(
            server.setNetworkProfile(
                      NearbyNetworkProfile::
                          Public)
                .hasValue());
        QCOMPARE(published.size(), 0);
        QCOMPARE(withdrawals, 0);

        QVERIFY(
            server.setNetworkProfile(
                      NearbyNetworkProfile::
                          Private)
                .hasValue());
        QCOMPARE(published.size(), 1);
        QCOMPARE(withdrawals, 0);

        const auto& first = published.first();
        QCOMPARE(
            first.serviceType,
            QStringLiteral(
                "_codex-companion._tcp.local"));
        QCOMPARE(
            first.port,
            server.serverPort());
        QVERIFY(
            first.instanceName
                    .toUtf8()
                    .size()
                <= 63);
        QCOMPARE(
            first.instanceName,
            QString(63, QLatin1Char('A')));
        QCOMPARE(
            first.txt,
            (QMap<QString, QString>{
                {
                    QStringLiteral("frame"),
                    QStringLiteral("1"),
                },
                {
                    QStringLiteral("id"),
                    QStringLiteral(
                        "11111111-2222-3333-4444-555555555555"),
                },
                {
                    QStringLiteral("path"),
                    QStringLiteral(
                        "/companion/v1"),
                },
                {
                    QStringLiteral("pv"),
                    QStringLiteral("1"),
                },
                {
                    QStringLiteral("tlsfp"),
                    certificateFingerprint(
                        tls),
                },
                {
                    QStringLiteral(
                        "transport"),
                    QStringLiteral("wss"),
                },
            }));

        QVERIFY(
            server.setNetworkProfile(
                      NearbyNetworkProfile::
                          Domain)
                .hasValue());
        QCOMPARE(withdrawals, 1);
        QCOMPARE(published.size(), 2);

        QVERIFY(
            server.setNetworkProfile(
                      NearbyNetworkProfile::
                          Unavailable)
                .hasValue());
        QCOMPARE(withdrawals, 2);
        QCOMPARE(published.size(), 2);

        auto stopped = server.stop();
        stopped.waitForFinished();
        QVERIFY(!server.isListening());
        QCOMPARE(withdrawals, 2);
    }

    void requiresTlsAndRejectsInvalidPathsAndInvitations()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        PairingCoordinator pairing(
            pairingStore);

        NearbyWebSocketServerOptions options;
        options.sslConfiguration = tls;
        options.installationId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        options.computerName =
            QStringLiteral(
                "Companion-Test");
        options.hostDisplayName =
            QStringLiteral(
                "Codex Companion Windows");
        options.listenAddress =
            QHostAddress::LocalHost;
        options.transferRootPath =
            directory.filePath(
                QStringLiteral(
                    "incoming"));

        NearbyWebSocketServer server(
            pairing,
            [](QString, BridgeRequest request) {
                BridgeResponse response;
                response.id = request.id;
                response.operation =
                    request.operation;
                response.succeeded = true;
                QPromise<BridgeResponse>
                    promise;
                promise.start();
                const auto future =
                    promise.future();
                promise.addResult(
                    std::move(response));
                promise.finish();
                return future;
            },
            std::move(options));
        QVERIFY(server.start().hasValue());

        QWebSocket plaintext;
        QSignalSpy plaintextConnected(
            &plaintext,
            &QWebSocket::connected);
        QSignalSpy plaintextError(
            &plaintext,
            &QWebSocket::errorOccurred);
        QSignalSpy plaintextDisconnected(
            &plaintext,
            &QWebSocket::disconnected);
        plaintext.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("ws"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_VERIFY_WITH_TIMEOUT(
            plaintextError.size() > 0
                || plaintextDisconnected
                       .size()
                    > 0,
            5'000);
        QCOMPARE(
            plaintextConnected.size(),
            0);

        QWebSocket wrongPath;
        configureTestTlsClient(wrongPath);
        QSignalSpy wrongConnected(
            &wrongPath,
            &QWebSocket::connected);
        QSignalSpy wrongDisconnected(
            &wrongPath,
            &QWebSocket::disconnected);
        wrongPath.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                QStringLiteral(
                    "/wrong")));
        QTRY_COMPARE_WITH_TIMEOUT(
            wrongConnected.size(),
            1,
            5'000);
        QTRY_COMPARE_WITH_TIMEOUT(
            wrongDisconnected.size(),
            1,
            5'000);
        QCOMPARE(
            static_cast<int>(
                wrongPath.closeCode()),
            4404);
        QCOMPARE(
            wrongPath.closeReason(),
            QStringLiteral(
                "wrong_path"));

        QWebSocket unsafeInvitation;
        configureTestTlsClient(
            unsafeInvitation);
        QSignalSpy unsafeConnected(
            &unsafeInvitation,
            &QWebSocket::connected);
        QSignalSpy unsafeDisconnected(
            &unsafeInvitation,
            &QWebSocket::disconnected);
        unsafeInvitation.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            unsafeConnected.size(),
            1,
            5'000);
        const QByteArray unsafeJson(
            "{\"version\":1,"
            "\"deviceID\":\"iphone-unsafe\","
            "\"displayName\":\"Unsafe\","
            "\"issuedAtMilliseconds\":9007199254740992,"
            "\"nonce\":\"IiIiIiIiIiIiIiIiIiIiIg==\","
            "\"pairingCode\":\"123-456\"}");
        QCOMPARE(
            unsafeInvitation.sendTextMessage(
                QString::fromUtf8(
                    unsafeJson)),
            unsafeJson.size());
        QTRY_COMPARE_WITH_TIMEOUT(
            unsafeDisconnected.size(),
            1,
            5'000);
        QCOMPARE(
            static_cast<int>(
                unsafeInvitation
                    .closeCode()),
            4400);
        QCOMPARE(
            unsafeInvitation
                .closeReason(),
            QStringLiteral(
                "invalid_invitation"));

        auto stopped = server.stop();
        stopped.waitForFinished();
    }

    void trustedInvitationBindsDeviceAndDispatchesAuthenticatedFrames()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        const QByteArray secret =
            repeatedSecret('\x53');
        QVERIFY(
            pairingStore.save({
                QStringLiteral(
                    "iphone-alpha"),
                QStringLiteral(
                    "Harlin iPhone"),
                secret,
                fixedNow(),
                std::nullopt,
            }).hasValue());
        PairingCoordinator pairing(
            pairingStore,
            fixedNow);

        QVector<QString> deviceIds;
        QVector<BridgeRequest> requests;
        NearbyWebSocketServerOptions options;
        options.sslConfiguration = tls;
        options.installationId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        options.computerName =
            QStringLiteral(
                "Companion-Test");
        options.hostDisplayName =
            QStringLiteral(
                "Codex Companion Windows");
        options.listenAddress =
            QHostAddress::LocalHost;
        options.transferRootPath =
            directory.filePath(
                QStringLiteral(
                    "incoming"));

        NearbyWebSocketServer server(
            pairing,
            [&deviceIds, &requests](
                QString deviceId,
                BridgeRequest request) {
                deviceIds.append(
                    deviceId);
                requests.append(request);
                BridgeResponse response;
                response.id = request.id;
                response.operation =
                    request.operation;
                response.succeeded = true;
                response.message =
                    QStringLiteral(
                        "trusted-response");
                return readyResponse(
                    std::move(response));
            },
            std::move(options));
        QVERIFY(server.start().hasValue());

        QWebSocket client(
            QStringLiteral(
                "https://untrusted-origin.invalid"));
        configureTestTlsClient(client);
        QSignalSpy connected(
            &client,
            &QWebSocket::connected);
        QSignalSpy binaryMessages(
            &client,
            &QWebSocket::
                binaryMessageReceived);
        client.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            connected.size(),
            1,
            5'000);

        const auto invitation =
            BridgeSecurity::
                authenticatedInvitation(
                    QStringLiteral(
                        "iphone-alpha"),
                    QStringLiteral(
                        "Changed display name"),
                    secret,
                    fixedNow(),
                    QByteArray(16, '\x41'));
        QVERIFY(invitation.hasValue());
        const QByteArray invitationBytes =
            encodeInvitation(
                invitation.value());
        QCOMPARE(
            client.sendTextMessage(
                QString::fromUtf8(
                    invitationBytes)),
            invitationBytes.size());
        QTRY_VERIFY_WITH_TIMEOUT(
            server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-alpha")),
            5'000);

        BridgeRequest request;
        request.id = QUuid(
            QStringLiteral(
                "33333333-4444-5555-6666-777777777777"));
        request.operation =
            BridgeOperation::ListTasks;
        request.limit = 20;
        const auto requestPayload =
            BridgeJsonCodec::encodeRequest(
                request,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(
            requestPayload.hasValue());
        const auto encodedFrame =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        Request,
                    0,
                    request.id,
                    {},
                    0,
                    0,
                    requestPayload.value(),
                },
                secret);
        QVERIFY(encodedFrame.hasValue());
        QCOMPARE(
            client.sendBinaryMessage(
                encodedFrame.value()),
            encodedFrame
                .value()
                .size());

        QTRY_COMPARE_WITH_TIMEOUT(
            requests.size(),
            1,
            5'000);
        const QVector<QString>
            expectedDeviceIds{
                QStringLiteral(
                    "iphone-alpha"),
            };
        QCOMPARE(
            deviceIds,
            expectedDeviceIds);
        QCOMPARE(
            requests.first(),
            request);
        QTRY_COMPARE_WITH_TIMEOUT(
            binaryMessages.size(),
            1,
            5'000);

        const QByteArray responseBytes =
            binaryMessages
                .first()
                .first()
                .toByteArray();
        const auto responseFrame =
            NearbyFrameCodec::decode(
                responseBytes,
                secret);
        QVERIFY(
            responseFrame.hasValue());
        QCOMPARE(
            responseFrame.value().type,
            NearbyFrameType::Request);
        QCOMPARE(
            responseFrame.value()
                .transferId,
            request.id);
        const auto response =
            BridgeJsonCodec::
                decodeResponse(
                    responseFrame.value()
                        .payload,
                    BridgeWireProfile::
                        NearbyV1Milliseconds);
        QVERIFY(response.hasValue());
        QCOMPARE(
            response.value().id,
            request.id);
        QVERIFY(
            response.value().succeeded);
        QCOMPARE(
            response.value().message,
            std::optional<QString>(
                QStringLiteral(
                    "trusted-response")));

        client.close();
        auto stopped = server.stop();
        stopped.waitForFinished();
    }

    void pairingAllowsOneRawHandshakeAndReturnsSecretOnlyOnce()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        const QByteArray generatedSecret =
            repeatedSecret('\x7A');
        PairingCoordinator pairing(
            pairingStore,
            fixedNow,
            [] {
                return Result<QString>::
                    success(
                        QStringLiteral(
                            "123456"));
            },
            [generatedSecret] {
                return Result<QByteArray>::
                    success(
                        generatedSecret);
            });
        QVERIFY(
            pairing.beginPairing()
                .hasValue());

        int handlerCalls = 0;
        NearbyWebSocketServerOptions options;
        options.sslConfiguration = tls;
        options.installationId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        options.computerName =
            QStringLiteral(
                "Companion-Test");
        options.hostDisplayName =
            QStringLiteral(
                "Codex Companion Windows");
        options.listenAddress =
            QHostAddress::LocalHost;
        options.transferRootPath =
            directory.filePath(
                QStringLiteral(
                    "incoming"));
        auto presencePetCatalogService =
            std::make_shared<
                MobilePresencePetCatalogService>();
        QVERIFY(
            presencePetCatalogService
                ->replaceSnapshot({
                    QStringLiteral(
                        "shadow-16"),
                    {},
                })
                .isEmpty());
        options.presencePetCatalogService =
            presencePetCatalogService;

        NearbyWebSocketServer server(
            pairing,
            [&handlerCalls](
                QString,
                BridgeRequest request) {
                ++handlerCalls;
                BridgeResponse response;
                response.id = request.id;
                response.operation =
                    request.operation;
                response.succeeded = true;
                return readyResponse(
                    std::move(response));
            },
            std::move(options));
        QVERIFY(server.start().hasValue());

        QWebSocket pairingClient;
        configureTestTlsClient(
            pairingClient);
        QSignalSpy pairingConnected(
            &pairingClient,
            &QWebSocket::connected);
        QSignalSpy pairingBinary(
            &pairingClient,
            &QWebSocket::
                binaryMessageReceived);
        QSignalSpy pairingDisconnected(
            &pairingClient,
            &QWebSocket::disconnected);
        pairingClient.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            pairingConnected.size(),
            1,
            5'000);

        const BridgeInvitation
            pairingInvitation{
                BridgeSecurity::
                    invitationVersion,
                QStringLiteral(
                    "iphone-new"),
                QStringLiteral(
                    "Harlin iPhone"),
                kNowMilliseconds,
                QByteArray(16, '\x22'),
                std::nullopt,
                QStringLiteral(
                    "123-456"),
            };
        const QByteArray invitationBytes =
            encodeInvitation(
                pairingInvitation);
        QCOMPARE(
            pairingClient.sendTextMessage(
                QString::fromUtf8(
                    invitationBytes)),
            invitationBytes.size());
        QVERIFY(
            !server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-new")));

        BridgeRequest firstHandshake;
        firstHandshake.id = QUuid(
            QStringLiteral(
                "44444444-5555-6666-7777-888888888888"));
        firstHandshake.operation =
            BridgeOperation::Handshake;
        const auto rawHandshake =
            BridgeJsonCodec::encodeRequest(
                firstHandshake,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(rawHandshake.hasValue());
        QCOMPARE(
            pairingClient
                .sendBinaryMessage(
                    rawHandshake.value()),
            rawHandshake.value().size());
        QTRY_COMPARE_WITH_TIMEOUT(
            pairingBinary.size(),
            1,
            5'000);

        const auto pairedResponse =
            BridgeJsonCodec::
                decodeResponse(
                    pairingBinary
                        .first()
                        .first()
                        .toByteArray(),
                    BridgeWireProfile::
                        NearbyV1Milliseconds);
        QVERIFY(
            pairedResponse.hasValue());
        QVERIFY(
            pairedResponse.value()
                .succeeded);
        QCOMPARE(
            pairedResponse.value()
                .pairingSecret,
            std::optional<QByteArray>(
                generatedSecret));
        QCOMPARE(
            pairedResponse.value()
                .macDeviceId,
            std::optional<QString>(
                QStringLiteral(
                    "11111111-2222-3333-4444-555555555555")));
        const std::optional<
            QVector<BridgeFeature>>
            expectedFeatures =
                QVector<BridgeFeature>{
                    BridgeFeature::
                        PresencePetPackageV1,
                };
        QCOMPARE(
            pairedResponse.value()
                .features,
            expectedFeatures);
        QCOMPARE(
            pairedResponse.value()
                .selectedDesktopPetId,
            std::optional<QString>(
                QStringLiteral(
                    "shadow-16")));
        QVERIFY(
            pairedResponse.value()
                .presencePetCatalog
                .has_value());
        QVERIFY(
            pairedResponse.value()
                .presencePetCatalog
                ->isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(
            server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-new")),
            5'000);
        QVERIFY(
            pairingStore.record(
                QStringLiteral(
                    "iphone-new"))
                .has_value());
        QCOMPARE(handlerCalls, 0);

        QCOMPARE(
            pairingClient
                .sendBinaryMessage(
                    rawHandshake.value()),
            rawHandshake.value().size());
        QTRY_COMPARE_WITH_TIMEOUT(
            pairingDisconnected.size(),
            1,
            5'000);
        QCOMPARE(
            static_cast<int>(
                pairingClient
                    .closeCode()),
            4401);

        QWebSocket trustedClient;
        configureTestTlsClient(
            trustedClient);
        QSignalSpy trustedConnected(
            &trustedClient,
            &QWebSocket::connected);
        QSignalSpy trustedBinary(
            &trustedClient,
            &QWebSocket::
                binaryMessageReceived);
        trustedClient.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            trustedConnected.size(),
            1,
            5'000);
        const auto trustedInvitation =
            BridgeSecurity::
                authenticatedInvitation(
                    QStringLiteral(
                        "iphone-new"),
                    QStringLiteral(
                        "Renamed iPhone"),
                    generatedSecret,
                    fixedNow(),
                    QByteArray(16, '\x33'));
        QVERIFY(
            trustedInvitation
                .hasValue());
        const QByteArray trustedBytes =
            encodeInvitation(
                trustedInvitation
                    .value());
        QCOMPARE(
            trustedClient.sendTextMessage(
                QString::fromUtf8(
                    trustedBytes)),
            trustedBytes.size());
        QTRY_VERIFY_WITH_TIMEOUT(
            server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-new")),
            5'000);

        BridgeRequest trustedHandshake;
        trustedHandshake.id = QUuid(
            QStringLiteral(
                "55555555-6666-7777-8888-999999999999"));
        trustedHandshake.operation =
            BridgeOperation::Handshake;
        const auto trustedPayload =
            BridgeJsonCodec::encodeRequest(
                trustedHandshake,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(
            trustedPayload.hasValue());
        const auto trustedFrame =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        Request,
                    0,
                    trustedHandshake.id,
                    {},
                    0,
                    0,
                    trustedPayload.value(),
                },
                generatedSecret);
        QVERIFY(trustedFrame.hasValue());
        QCOMPARE(
            trustedClient
                .sendBinaryMessage(
                    trustedFrame.value()),
            trustedFrame.value().size());
        QTRY_COMPARE_WITH_TIMEOUT(
            trustedBinary.size(),
            1,
            5'000);
        const auto framedResponse =
            NearbyFrameCodec::decode(
                trustedBinary
                    .first()
                    .first()
                    .toByteArray(),
                generatedSecret);
        QVERIFY(
            framedResponse.hasValue());
        const auto reconnectResponse =
            BridgeJsonCodec::
                decodeResponse(
                    framedResponse.value()
                        .payload,
                    BridgeWireProfile::
                        NearbyV1Milliseconds);
        QVERIFY(
            reconnectResponse.hasValue());
        QVERIFY(
            reconnectResponse.value()
                .succeeded);
        QVERIFY(
            !reconnectResponse.value()
                 .pairingSecret
                 .has_value());
        QCOMPARE(
            reconnectResponse.value()
                .features,
            pairedResponse.value()
                .features);
        QCOMPARE(
            reconnectResponse.value()
                .selectedDesktopPetId,
            pairedResponse.value()
                .selectedDesktopPetId);
        QCOMPARE(
            reconnectResponse.value()
                .presencePetCatalog,
            pairedResponse.value()
                .presencePetCatalog);
        QCOMPARE(handlerCalls, 0);

        trustedClient.close();
        auto stopped = server.stop();
        stopped.waitForFinished();
    }

    void deviceIdIsTheIdentityAndDisconnectRemovesPartialTransfers()
    {
        const QSslConfiguration tls =
            testTlsConfiguration();
        QVERIFY(
            !tls.localCertificate()
                 .isNull());

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        TestProtector protector;
        PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        const QByteArray secret =
            repeatedSecret('\x35');
        QVERIFY(
            pairingStore.save({
                QStringLiteral(
                    "iphone-alpha"),
                QStringLiteral(
                    "Shared display name"),
                secret,
                fixedNow(),
                std::nullopt,
            }).hasValue());
        PairingCoordinator pairing(
            pairingStore,
            fixedNow);

        int handlerCalls = 0;
        const QString transferRoot =
            directory.filePath(
                QStringLiteral(
                    "incoming"));
        NearbyWebSocketServerOptions options;
        options.sslConfiguration = tls;
        options.installationId =
            QStringLiteral(
                "11111111-2222-3333-4444-555555555555");
        options.computerName =
            QStringLiteral(
                "Companion-Test");
        options.hostDisplayName =
            QStringLiteral(
                "Codex Companion Windows");
        options.listenAddress =
            QHostAddress::LocalHost;
        options.transferRootPath =
            transferRoot;

        NearbyWebSocketServer server(
            pairing,
            [&handlerCalls](
                QString,
                BridgeRequest request) {
                ++handlerCalls;
                BridgeResponse response;
                response.id = request.id;
                response.operation =
                    request.operation;
                response.succeeded = true;
                return readyResponse(
                    std::move(response));
            },
            std::move(options));
        QVERIFY(server.start().hasValue());

        QWebSocket impostor;
        configureTestTlsClient(impostor);
        QSignalSpy impostorConnected(
            &impostor,
            &QWebSocket::connected);
        QSignalSpy impostorDisconnected(
            &impostor,
            &QWebSocket::disconnected);
        impostor.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            impostorConnected.size(),
            1,
            5'000);
        const auto impostorInvitation =
            BridgeSecurity::
                authenticatedInvitation(
                    QStringLiteral(
                        "iphone-other"),
                    QStringLiteral(
                        "Shared display name"),
                    secret,
                    fixedNow(),
                    QByteArray(16, '\x44'));
        QVERIFY(
            impostorInvitation
                .hasValue());
        const QByteArray impostorBytes =
            encodeInvitation(
                impostorInvitation
                    .value());
        QCOMPARE(
            impostor.sendTextMessage(
                QString::fromUtf8(
                    impostorBytes)),
            impostorBytes.size());
        QTRY_COMPARE_WITH_TIMEOUT(
            impostorDisconnected.size(),
            1,
            5'000);
        QCOMPARE(
            static_cast<int>(
                impostor.closeCode()),
            4401);
        QVERIFY(
            !server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-other")));

        QWebSocket client;
        configureTestTlsClient(client);
        QSignalSpy connected(
            &client,
            &QWebSocket::connected);
        QSignalSpy disconnected(
            &client,
            &QWebSocket::disconnected);
        client.open(
            serverUrl(
                server.serverPort(),
                QStringLiteral("wss"),
                NearbyWebSocketServer::
                    requestPath.toString()));
        QTRY_COMPARE_WITH_TIMEOUT(
            connected.size(),
            1,
            5'000);
        const auto invitation =
            BridgeSecurity::
                authenticatedInvitation(
                    QStringLiteral(
                        "iphone-alpha"),
                    QStringLiteral(
                        "Another display name"),
                    secret,
                    fixedNow(),
                    QByteArray(16, '\x45'));
        QVERIFY(invitation.hasValue());
        const QByteArray invitationBytes =
            encodeInvitation(
                invitation.value());
        QCOMPARE(
            client.sendTextMessage(
                QString::fromUtf8(
                    invitationBytes)),
            invitationBytes.size());
        QTRY_VERIFY_WITH_TIMEOUT(
            server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-alpha")),
            5'000);

        const QUuid attachmentId(
            QStringLiteral(
                "66666666-7777-8888-9999-AAAAAAAAAAAA"));
        BridgeRequest request;
        request.id = QUuid(
            QStringLiteral(
                "77777777-8888-9999-AAAA-BBBBBBBBBBBB"));
        request.operation =
            BridgeOperation::SendMessage;
        request.threadId =
            QStringLiteral("thread-1");
        request.text =
            QStringLiteral("Partial upload");
        request.attachments =
            QVector<BridgeAttachment>{
                {
                    attachmentId,
                    AttachmentKind::File,
                    QStringLiteral(
                        "partial.bin"),
                    QStringLiteral(
                        "application/octet-stream"),
                    {},
                },
            };
        const auto requestPayload =
            BridgeJsonCodec::encodeRequest(
                request,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        QVERIFY(
            requestPayload.hasValue());
        const auto requestFrame =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        Request,
                    0,
                    request.id,
                    {},
                    0,
                    0,
                    requestPayload.value(),
                },
                secret);
        QVERIFY(requestFrame.hasValue());
        QCOMPARE(
            client.sendBinaryMessage(
                requestFrame.value()),
            requestFrame
                .value()
                .size());

        const QByteArray attachmentData(
            300'000,
            '\x5C');
        const QJsonObject beginObject{
            {
                QStringLiteral("id"),
                attachmentId.toString(
                    QUuid::WithoutBraces)
                    .toUpper(),
            },
            {
                QStringLiteral("kind"),
                QStringLiteral("file"),
            },
            {
                QStringLiteral(
                    "filename"),
                QStringLiteral(
                    "partial.bin"),
            },
            {
                QStringLiteral(
                    "mimeType"),
                QStringLiteral(
                    "application/octet-stream"),
            },
            {
                QStringLiteral(
                    "byteCount"),
                static_cast<qint64>(
                    attachmentData
                        .size()),
            },
            {
                QStringLiteral(
                    "sha256"),
                QString::fromLatin1(
                    QCryptographicHash::
                        hash(
                            attachmentData,
                            QCryptographicHash::
                                Sha256)
                        .toHex()),
            },
        };
        const auto beginFrame =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        AttachmentBegin,
                    0,
                    request.id,
                    attachmentId,
                    0,
                    2,
                    QJsonDocument(
                        beginObject)
                        .toJson(
                            QJsonDocument::
                                Compact),
                },
                secret);
        QVERIFY(beginFrame.hasValue());
        QCOMPARE(
            client.sendBinaryMessage(
                beginFrame.value()),
            beginFrame
                .value()
                .size());

        const auto chunkFrame =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        AttachmentChunk,
                    0,
                    request.id,
                    attachmentId,
                    0,
                    2,
                    attachmentData.left(
                        NearbyFrameCodec::
                            maximumChunkBytes),
                },
                secret);
        QVERIFY(chunkFrame.hasValue());
        QCOMPARE(
            client.sendBinaryMessage(
                chunkFrame.value()),
            chunkFrame
                .value()
                .size());

        QTRY_VERIFY_WITH_TIMEOUT(
            QDir(transferRoot)
                    .entryList(
                        QDir::Dirs
                            | QDir::
                                  NoDotAndDotDot)
                    .size()
                == 1,
            5'000);
        client.abort();
        QTRY_COMPARE_WITH_TIMEOUT(
            disconnected.size(),
            1,
            5'000);
        QTRY_VERIFY_WITH_TIMEOUT(
            QDir(transferRoot)
                .entryList(
                    QDir::AllEntries
                        | QDir::
                              NoDotAndDotDot)
                .isEmpty(),
            5'000);
        QCOMPARE(handlerCalls, 0);
        QVERIFY(
            !server.hasAuthenticatedDevice(
                QStringLiteral(
                    "iphone-alpha")));

        auto stopped = server.stop();
        stopped.waitForFinished();
    }
};

QTEST_MAIN(NearbyWebSocketServerTests)

#include "NearbyWebSocketServerTests.moc"
