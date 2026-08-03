#include "app/CompanionMobileHost.h"

#include "codex/models/BridgeJsonCodec.h"
#include "mobile/nearby/NearbyFrameCodec.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/security/WindowsDpapiProtector.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPromise>
#include <QSaveFile>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QUuid>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketProtocol>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {

using namespace companion;

constexpr QStringView kInstallationId =
    u"11111111-2222-3333-4444-555555555555";
constexpr QStringView kHostName =
    u"Codex Companion Windows Probe";
constexpr QStringView kComputerName =
    u"Companion-Probe";

struct Arguments final {
    QString scenario;
    QString outputPath;
    QString fixtureRoot;
    QString deviceId;
    std::optional<QUrl> relayUrl;
    int timeoutMilliseconds = 60'000;
};

struct HandlerState final {
    QSet<int> operations;
    int dispatchCount = 0;
    bool inlineAttachmentPassed = false;
    bool streamedAttachmentPassed = false;
};

struct ProbeState final {
    QStringList failures;
    int operationsPassed = 0;
    int securityFailuresRejected = 0;
    bool fixtureContractPassed = false;
    bool advertisementPassed = false;
    bool certificateFingerprintPassed = false;
    bool pairingPassed = false;
    bool pairingSecretOneTime = false;
    bool trustedReconnectPassed = false;
    bool dpapiPersistencePassed = false;
    bool inlineAttachmentPassed = false;
    bool streamedAttachmentPassed = false;
    bool metadataMismatchRejected = false;
    bool relayReplayRejected = false;
    bool lifecyclePassed = false;
    bool forgetPassed = false;

    bool fail(
        QString stage,
        QString code)
    {
        failures.append(
            std::move(stage)
            + QLatin1Char(':')
            + std::move(code));
        return false;
    }
};

class RecordingDnsSdApi final
    : public IWindowsDnsSdApi {
public:
    Result<void> registerService(
        const WindowsDnsSdService& service)
        override
    {
        ++registerCalls;
        active = service;
        return Result<void>::success();
    }

    Result<void> deregisterService(
        const WindowsDnsSdService& service)
        override
    {
        ++deregisterCalls;
        if (!active.has_value()
            || *active != service) {
            return Result<void>::failure({
                QStringLiteral(
                    "probe.dns_registration_mismatch"),
                QStringLiteral(
                    "The probe DNS-SD registration changed unexpectedly."),
                false,
                {},
            });
        }
        active.reset();
        return Result<void>::success();
    }

    int registerCalls = 0;
    int deregisterCalls = 0;
    std::optional<WindowsDnsSdService>
        active;
};

class PrivateNetworkProfileApi final
    : public IWindowsNetworkProfileApi {
public:
    Result<WindowsNetworkProfile>
    currentProfile() const override
    {
        return Result<WindowsNetworkProfile>::
            success(
                WindowsNetworkProfile::
                    Private);
    }

    void setChangeCallback(
        std::function<void()> callback)
        override
    {
        callback_ = std::move(callback);
    }

private:
    std::function<void()> callback_;
};

class SocketSession final {
public:
    SocketSession()
        : socket(
              QStringLiteral(
                  "https://codex-companion-probe.invalid"))
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
            [this](
                const QList<QSslError>&) {
                socket.ignoreSslErrors();
            });
        QObject::connect(
            &socket,
            &QWebSocket::connected,
            &socket,
            [this] {
                connected = true;
            });
        QObject::connect(
            &socket,
            &QWebSocket::disconnected,
            &socket,
            [this] {
                disconnected = true;
            });
        QObject::connect(
            &socket,
            &QWebSocket::
                binaryMessageReceived,
            &socket,
            [this](
                const QByteArray& message) {
                binaryMessages.append(
                    message);
            });
    }

    QWebSocket socket;
    bool connected = false;
    bool disconnected = false;
    QVector<QByteArray> binaryMessages;
};

bool waitUntil(
    const std::function<bool()>& predicate,
    int timeoutMilliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()
           && timer.elapsed()
               < timeoutMilliseconds) {
        QCoreApplication::processEvents(
            QEventLoop::AllEvents,
            20);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(
        QEventLoop::AllEvents,
        20);
    return predicate();
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

QString operationName(
    BridgeOperation operation)
{
    switch (operation) {
    case BridgeOperation::Handshake:
        return QStringLiteral("handshake");
    case BridgeOperation::ListTasks:
        return QStringLiteral("listTasks");
    case BridgeOperation::LoadMessages:
        return QStringLiteral("loadMessages");
    case BridgeOperation::SendMessage:
        return QStringLiteral("sendMessage");
    case BridgeOperation::RespondToApproval:
        return QStringLiteral(
            "respondToApproval");
    case BridgeOperation::CreateTask:
        return QStringLiteral("createTask");
    case BridgeOperation::LoadCapabilities:
        return QStringLiteral(
            "loadCapabilities");
    case BridgeOperation::SendCasualChat:
        return QStringLiteral(
            "sendCasualChat");
    case BridgeOperation::LoadUsage:
        return QStringLiteral("loadUsage");
    case BridgeOperation::ConsumeUsageReset:
        return QStringLiteral(
            "consumeUsageReset");
    case BridgeOperation::CreateGoal:
        return QStringLiteral("createGoal");
    case BridgeOperation::ResumeGoal:
        return QStringLiteral("resumeGoal");
    case BridgeOperation::UpdateGoal:
        return QStringLiteral("updateGoal");
    }
    return QStringLiteral("unknown");
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
            invitation
                .issuedAtMilliseconds,
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

QUrl serverUrl(quint16 port)
{
    QUrl url;
    url.setScheme(
        QStringLiteral("wss"));
    url.setHost(
        QStringLiteral("127.0.0.1"));
    url.setPort(port);
    url.setPath(
        NearbyWebSocketServer::
            requestPath.toString());
    return url;
}

bool openSocket(
    SocketSession& session,
    quint16 port,
    int timeoutMilliseconds,
    ProbeState& state,
    const QString& stage)
{
    session.socket.open(
        serverUrl(port));
    if (!waitUntil(
            [&session] {
                return session.connected
                    || session.disconnected;
            },
            timeoutMilliseconds)
        || !session.connected) {
        return state.fail(
            stage,
            QStringLiteral(
                "socket_connect_failed"));
    }
    return true;
}

void closeSocket(
    SocketSession& session,
    int timeoutMilliseconds)
{
    if (session.socket.state()
        == QAbstractSocket::
               ConnectedState) {
        session.socket.close();
        waitUntil(
            [&session] {
                return session.disconnected;
            },
            timeoutMilliseconds);
    }
}

bool sendInvitation(
    SocketSession& session,
    const BridgeInvitation& invitation,
    ProbeState& state,
    const QString& stage)
{
    const QByteArray bytes =
        encodeInvitation(invitation);
    if (session.socket.sendTextMessage(
            QString::fromUtf8(bytes))
        != bytes.size()) {
        return state.fail(
            stage,
            QStringLiteral(
                "invitation_send_failed"));
    }
    return true;
}

bool sendRawRequest(
    SocketSession& session,
    const BridgeRequest& request,
    int timeoutMilliseconds,
    BridgeResponse& response,
    ProbeState& state,
    const QString& stage)
{
    const auto encoded =
        BridgeJsonCodec::encodeRequest(
            request,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!encoded.hasValue()) {
        return state.fail(
            stage,
            encoded.error().code);
    }

    const qsizetype before =
        session.binaryMessages.size();
    if (session.socket.sendBinaryMessage(
            encoded.value())
        != encoded.value().size()) {
        return state.fail(
            stage,
            QStringLiteral(
                "request_send_failed"));
    }
    if (!waitUntil(
            [&session, before] {
                return session
                           .binaryMessages
                           .size()
                           > before
                    || session.disconnected;
            },
            timeoutMilliseconds)
        || session.binaryMessages.size()
               <= before) {
        return state.fail(
            stage,
            QStringLiteral(
                "response_timeout"));
    }

    const auto decoded =
        BridgeJsonCodec::decodeResponse(
            session.binaryMessages.at(
                before),
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!decoded.hasValue()) {
        return state.fail(
            stage,
            decoded.error().code);
    }
    response = decoded.value();
    if (response.id != request.id
        || response.operation
            != request.operation
        || !response.succeeded) {
        return state.fail(
            stage,
            QStringLiteral(
                "response_mismatch"));
    }
    return true;
}

bool sendFramedRequest(
    SocketSession& session,
    const BridgeRequest& request,
    QByteArrayView secret,
    int timeoutMilliseconds,
    BridgeResponse& response,
    ProbeState& state,
    const QString& stage)
{
    const auto payload =
        BridgeJsonCodec::encodeRequest(
            request,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!payload.hasValue()) {
        return state.fail(
            stage,
            payload.error().code);
    }
    const auto encoded =
        NearbyFrameCodec::encode(
            {
                NearbyFrameType::Request,
                0,
                request.id,
                {},
                0,
                0,
                payload.value(),
            },
            secret);
    if (!encoded.hasValue()) {
        return state.fail(
            stage,
            encoded.error().code);
    }

    const qsizetype before =
        session.binaryMessages.size();
    if (session.socket.sendBinaryMessage(
            encoded.value())
        != encoded.value().size()) {
        return state.fail(
            stage,
            QStringLiteral(
                "frame_send_failed"));
    }
    if (!waitUntil(
            [&session, before] {
                return session
                           .binaryMessages
                           .size()
                           > before
                    || session.disconnected;
            },
            timeoutMilliseconds)
        || session.binaryMessages.size()
               <= before) {
        return state.fail(
            stage,
            QStringLiteral(
                "framed_response_timeout"));
    }

    const auto frame =
        NearbyFrameCodec::decode(
            session.binaryMessages.at(
                before),
            secret);
    if (!frame.hasValue()) {
        return state.fail(
            stage,
            frame.error().code);
    }
    if (frame.value().type
            != NearbyFrameType::Request
        || frame.value().transferId
            != request.id) {
        return state.fail(
            stage,
            QStringLiteral(
                "response_frame_mismatch"));
    }
    const auto decoded =
        BridgeJsonCodec::decodeResponse(
            frame.value().payload,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!decoded.hasValue()) {
        return state.fail(
            stage,
            decoded.error().code);
    }
    response = decoded.value();
    if (response.id != request.id
        || response.operation
            != request.operation
        || !response.succeeded) {
        return state.fail(
            stage,
            QStringLiteral(
                "framed_response_mismatch"));
    }
    return true;
}

BridgeRequest operationRequest(
    BridgeOperation operation)
{
    BridgeRequest request;
    request.id = QUuid::createUuid();
    request.operation = operation;

    switch (operation) {
    case BridgeOperation::Handshake:
        break;
    case BridgeOperation::ListTasks:
        request.limit = 20;
        break;
    case BridgeOperation::LoadMessages:
        request.threadId =
            QStringLiteral("probe-thread");
        request.limit = 30;
        break;
    case BridgeOperation::SendMessage:
        request.threadId =
            QStringLiteral("probe-thread");
        request.text =
            QStringLiteral("inline-probe");
        request.sendAction =
            SendAction::Reply;
        request.attachments =
            QVector<BridgeAttachment>{
                {
                    QUuid::createUuid(),
                    AttachmentKind::File,
                    QStringLiteral(
                        "probe.txt"),
                    QStringLiteral(
                        "text/plain"),
                    QByteArray(
                        "inline-probe"),
                },
            };
        request.idempotencyKey =
            QUuid::createUuid();
        break;
    case BridgeOperation::
        RespondToApproval:
        request.threadId =
            QStringLiteral("probe-thread");
        request.approvalDecision =
            ApprovalDecision::ApproveOnce;
        break;
    case BridgeOperation::CreateTask:
        request.text =
            QStringLiteral("Probe task");
        request.cwd =
            QStringLiteral("C:\\probe");
        request.model =
            QStringLiteral("probe-model");
        request.reasoningEffort =
            QStringLiteral("medium");
        request.idempotencyKey =
            QUuid::createUuid();
        break;
    case BridgeOperation::
        LoadCapabilities:
        break;
    case BridgeOperation::SendCasualChat:
        request.text =
            QStringLiteral("Probe chat");
        request.chatAgentId =
            QStringLiteral("probe-agent");
        request.chatProvider =
            ChatProvider::OnDevice;
        request.chatModelId =
            QStringLiteral("probe-chat");
        break;
    case BridgeOperation::LoadUsage:
        break;
    case BridgeOperation::
        ConsumeUsageReset:
        request.resetCreditId =
            QStringLiteral("probe-credit");
        break;
    case BridgeOperation::CreateGoal:
        request.goalObjective =
            QStringLiteral(
                "Verify the mobile bridge");
        request.goalTokenBudget = 4096;
        break;
    case BridgeOperation::ResumeGoal:
        request.threadId =
            QStringLiteral("probe-goal");
        break;
    case BridgeOperation::UpdateGoal:
        request.threadId =
            QStringLiteral("probe-goal");
        request.goalObjective =
            QStringLiteral(
                "Verify the assembled mobile bridge");
        request.goalTokenBudget = 8192;
        break;
    }
    return request;
}

QSslConfiguration testTlsConfiguration()
{
    const QString root =
        QStringLiteral(
            COMPANION_QTWEBSOCKET_FIXTURE_ROOT);
    QFile certificateFile(
        QDir(root).filePath(
            QStringLiteral(
                "localhost.cert")));
    QFile keyFile(
        QDir(root).filePath(
            QStringLiteral(
                "localhost.key")));
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

QString certificateFingerprint(
    const QSslCertificate& certificate)
{
    return QString::fromLatin1(
        certificate.digest(
            QCryptographicHash::Sha256)
            .toHex())
        .toLower();
}

bool validateFixtures(
    const QString& root,
    ProbeState& state)
{
    QFile requestFile(
        QDir(root).filePath(
            QStringLiteral(
                "bridge-request-nearby.json")));
    QFile responseFile(
        QDir(root).filePath(
            QStringLiteral(
                "bridge-response-nearby.json")));
    if (!requestFile.open(
            QIODevice::ReadOnly)
        || !responseFile.open(
            QIODevice::ReadOnly)) {
        return state.fail(
            QStringLiteral("fixtures"),
            QStringLiteral(
                "fixture_read_failed"));
    }
    const auto request =
        BridgeJsonCodec::decodeRequest(
            requestFile.readAll(),
            BridgeWireProfile::
                NearbyV1Milliseconds);
    const auto response =
        BridgeJsonCodec::decodeResponse(
            responseFile.readAll(),
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!request.hasValue()) {
        return state.fail(
            QStringLiteral("fixtures"),
            request.error().code);
    }
    if (!response.hasValue()) {
        return state.fail(
            QStringLiteral("fixtures"),
            response.error().code);
    }
    if (request.value().protocolVersion
            != kBridgeProtocolVersion
        || response.value()
               .protocolVersion
            != kBridgeProtocolVersion) {
        return state.fail(
            QStringLiteral("fixtures"),
            QStringLiteral(
                "protocol_mismatch"));
    }
    state.fixtureContractPassed = true;
    return true;
}

bool verifyProtectedPairingStore(
    const QString& path,
    const QString& deviceId,
    QByteArrayView secret,
    ProbeState& state)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return state.fail(
            QStringLiteral("dpapi"),
            QStringLiteral(
                "pairing_store_unreadable"));
    }
    const QByteArray stored =
        file.readAll();
    const QByteArray plain(
        secret.data(),
        secret.size());
    if (stored.contains(plain)
        || stored.contains(
            plain.toBase64())
        || stored.contains(
            plain.toHex())) {
        return state.fail(
            QStringLiteral("dpapi"),
            QStringLiteral(
                "plaintext_secret_found"));
    }

    WindowsDpapiProtector protector;
    PairingRecordStore reloaded(
        path,
        protector);
    if (reloaded.loadError()
            .has_value()) {
        return state.fail(
            QStringLiteral("dpapi"),
            reloaded.loadError()
                ->code);
    }
    const auto record =
        reloaded.record(deviceId);
    if (!record.has_value()
        || record->secret != plain) {
        return state.fail(
            QStringLiteral("dpapi"),
            QStringLiteral(
                "pairing_store_reload_failed"));
    }
    state.dpapiPersistencePassed = true;
    return true;
}

bool authenticateTrusted(
    SocketSession& session,
    quint16 port,
    const QString& deviceId,
    QByteArrayView secret,
    int timeoutMilliseconds,
    ProbeState& state,
    const QString& stage,
    bool sendHandshake = true)
{
    if (!openSocket(
            session,
            port,
            timeoutMilliseconds,
            state,
            stage)) {
        return false;
    }
    const auto nonce =
        BridgeSecurity::
            randomInvitationNonce();
    if (!nonce.hasValue()) {
        return state.fail(
            stage,
            nonce.error().code);
    }
    const auto invitation =
        BridgeSecurity::
            authenticatedInvitation(
                deviceId,
                QStringLiteral(
                    "Windows Probe Client"),
                secret,
                QDateTime::
                    currentDateTimeUtc(),
                nonce.value());
    if (!invitation.hasValue()) {
        return state.fail(
            stage,
            invitation.error().code);
    }
    if (!sendInvitation(
            session,
            invitation.value(),
            state,
            stage)) {
        return false;
    }
    if (!sendHandshake) {
        return true;
    }

    BridgeResponse response;
    if (!sendFramedRequest(
            session,
            operationRequest(
                BridgeOperation::
                    Handshake),
            secret,
            timeoutMilliseconds,
            response,
            state,
            stage)) {
        return false;
    }
    if (response.pairingSecret
            .has_value()) {
        return state.fail(
            stage,
            QStringLiteral(
                "secret_repeated"));
    }
    return true;
}

bool sendStreamedAttachment(
    SocketSession& session,
    QByteArrayView secret,
    int timeoutMilliseconds,
    ProbeState& state)
{
    const QUuid attachmentId =
        QUuid::createUuid();
    const QByteArray data(
        NearbyFrameCodec::
                maximumChunkBytes
            + 19,
        '\x5A');

    BridgeRequest request;
    request.id = QUuid::createUuid();
    request.operation =
        BridgeOperation::SendMessage;
    request.threadId =
        QStringLiteral("probe-thread");
    request.text =
        QStringLiteral("streamed-probe");
    request.attachments =
        QVector<BridgeAttachment>{
            {
                attachmentId,
                AttachmentKind::File,
                QStringLiteral(
                    "../streamed-probe.bin"),
                QStringLiteral(
                    "application/octet-stream"),
                {},
            },
        };
    request.idempotencyKey =
        QUuid::createUuid();

    const auto requestPayload =
        BridgeJsonCodec::encodeRequest(
            request,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!requestPayload.hasValue()) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            requestPayload.error().code);
    }
    const auto requestFrame =
        NearbyFrameCodec::encode(
            {
                NearbyFrameType::Request,
                0,
                request.id,
                {},
                0,
                0,
                requestPayload.value(),
            },
            secret);
    if (!requestFrame.hasValue()) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            requestFrame.error().code);
    }

    const QJsonObject metadata{
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
            QStringLiteral("filename"),
            QStringLiteral(
                "../streamed-probe.bin"),
        },
        {
            QStringLiteral("mimeType"),
            QStringLiteral(
                "application/octet-stream"),
        },
        {
            QStringLiteral("byteCount"),
            data.size(),
        },
        {
            QStringLiteral("sha256"),
            QString::fromLatin1(
                QCryptographicHash::hash(
                    data,
                    QCryptographicHash::
                        Sha256)
                    .toHex()),
        },
    };
    const QVector<NearbyFrame> frames{
        {
            NearbyFrameType::
                AttachmentBegin,
            0,
            request.id,
            attachmentId,
            0,
            2,
            QJsonDocument(metadata)
                .toJson(
                    QJsonDocument::Compact),
        },
        {
            NearbyFrameType::
                AttachmentChunk,
            0,
            request.id,
            attachmentId,
            0,
            2,
            data.left(
                NearbyFrameCodec::
                    maximumChunkBytes),
        },
        {
            NearbyFrameType::
                AttachmentChunk,
            0,
            request.id,
            attachmentId,
            1,
            2,
            data.mid(
                NearbyFrameCodec::
                    maximumChunkBytes),
        },
        {
            NearbyFrameType::
                AttachmentCommit,
            0,
            request.id,
            attachmentId,
            0,
            2,
            {},
        },
    };

    const qsizetype before =
        session.binaryMessages.size();
    if (session.socket.sendBinaryMessage(
            requestFrame.value())
        != requestFrame.value().size()) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            QStringLiteral(
                "request_send_failed"));
    }
    for (const NearbyFrame& frame :
         frames) {
        const auto encoded =
            NearbyFrameCodec::encode(
                frame,
                secret);
        if (!encoded.hasValue()
            || session.socket
                   .sendBinaryMessage(
                       encoded.value())
                != encoded.value().size()) {
            return state.fail(
                QStringLiteral(
                    "streamed_attachment"),
                encoded.hasValue()
                    ? QStringLiteral(
                          "frame_send_failed")
                    : encoded.error().code);
        }
    }

    if (!waitUntil(
            [&session, before] {
                return session
                           .binaryMessages
                           .size()
                           > before
                    || session.disconnected;
            },
            timeoutMilliseconds)
        || session.binaryMessages.size()
               <= before) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            QStringLiteral(
                "response_timeout"));
    }
    const auto responseFrame =
        NearbyFrameCodec::decode(
            session.binaryMessages.at(
                before),
            secret);
    if (!responseFrame.hasValue()) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            responseFrame.error().code);
    }
    const auto response =
        BridgeJsonCodec::decodeResponse(
            responseFrame.value()
                .payload,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!response.hasValue()
        || response.value().id
            != request.id
        || !response.value().succeeded) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            response.hasValue()
                ? QStringLiteral(
                      "response_mismatch")
                : response.error().code);
    }
    return true;
}

bool expectRejectedInvitation(
    quint16 port,
    const BridgeInvitation& invitation,
    int expectedCode,
    int timeoutMilliseconds,
    ProbeState& state,
    const QString& stage)
{
    SocketSession session;
    if (!openSocket(
            session,
            port,
            timeoutMilliseconds,
            state,
            stage)
        || !sendInvitation(
            session,
            invitation,
            state,
            stage)) {
        return false;
    }
    if (!waitUntil(
            [&session] {
                return session.disconnected;
            },
            timeoutMilliseconds)
        || static_cast<int>(
               session.socket
                   .closeCode())
               != expectedCode) {
        return state.fail(
            stage,
            QStringLiteral(
                "rejection_mismatch"));
    }
    ++state.securityFailuresRejected;
    return true;
}

bool expectInvalidFrame(
    quint16 port,
    const QString& deviceId,
    QByteArrayView secret,
    QByteArray frame,
    int timeoutMilliseconds,
    ProbeState& state,
    const QString& stage,
    int expectedCode)
{
    SocketSession session;
    if (!authenticateTrusted(
            session,
            port,
            deviceId,
            secret,
            timeoutMilliseconds,
            state,
            stage,
            false)) {
        return false;
    }
    if (session.socket.sendBinaryMessage(
            frame)
        != frame.size()) {
        return state.fail(
            stage,
            QStringLiteral(
                "invalid_frame_send_failed"));
    }
    if (!waitUntil(
            [&session] {
                return session.disconnected;
            },
            timeoutMilliseconds)
        || static_cast<int>(
               session.socket
                   .closeCode())
               != expectedCode) {
        return state.fail(
            stage,
            QStringLiteral(
                "invalid_frame_not_rejected"));
    }
    ++state.securityFailuresRejected;
    return true;
}

bool expectAttachmentMetadataMismatch(
    quint16 port,
    const QString& deviceId,
    QByteArrayView secret,
    int timeoutMilliseconds,
    ProbeState& state)
{
    SocketSession session;
    const QString stage =
        QStringLiteral(
            "attachment_metadata_mismatch");
    if (!authenticateTrusted(
            session,
            port,
            deviceId,
            secret,
            timeoutMilliseconds,
            state,
            stage,
            false)) {
        return false;
    }

    const QUuid attachmentId =
        QUuid::createUuid();
    BridgeRequest request;
    request.id = QUuid::createUuid();
    request.operation =
        BridgeOperation::SendMessage;
    request.threadId =
        QStringLiteral("probe-thread");
    request.text =
        QStringLiteral(
            "metadata-mismatch-probe");
    request.attachments =
        QVector<BridgeAttachment>{
            {
                attachmentId,
                AttachmentKind::File,
                QStringLiteral(
                    "expected.bin"),
                QStringLiteral(
                    "application/octet-stream"),
                {},
            },
        };

    const auto payload =
        BridgeJsonCodec::encodeRequest(
            request,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    if (!payload.hasValue()) {
        return state.fail(
            stage,
            payload.error().code);
    }
    const auto requestFrame =
        NearbyFrameCodec::encode(
            {
                NearbyFrameType::Request,
                0,
                request.id,
                {},
                0,
                0,
                payload.value(),
            },
            secret);
    const QByteArray attachmentData(
        "metadata-probe");
    const QJsonObject metadata{
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
            QStringLiteral("filename"),
            QStringLiteral(
                "different.bin"),
        },
        {
            QStringLiteral("mimeType"),
            QStringLiteral(
                "application/octet-stream"),
        },
        {
            QStringLiteral("byteCount"),
            attachmentData.size(),
        },
        {
            QStringLiteral("sha256"),
            QString::fromLatin1(
                QCryptographicHash::hash(
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
                1,
                QJsonDocument(metadata)
                    .toJson(
                        QJsonDocument::
                            Compact),
            },
            secret);
    if (!requestFrame.hasValue()
        || !beginFrame.hasValue()) {
        return state.fail(
            stage,
            requestFrame.hasValue()
                ? beginFrame.error().code
                : requestFrame.error().code);
    }
    if (session.socket.sendBinaryMessage(
            requestFrame.value())
            != requestFrame
                   .value()
                   .size()
        || session.socket.sendBinaryMessage(
               beginFrame.value())
               != beginFrame
                      .value()
                      .size()) {
        return state.fail(
            stage,
            QStringLiteral(
                "frame_send_failed"));
    }
    if (!waitUntil(
            [&session] {
                return session.disconnected;
            },
            timeoutMilliseconds)
        || static_cast<int>(
               session.socket
                   .closeCode())
               != 4400) {
        return state.fail(
            stage,
            QStringLiteral(
                "metadata_mismatch_not_rejected"));
    }
    ++state.securityFailuresRejected;
    state.metadataMismatchRejected =
        true;
    return true;
}

bool verifyRelayReplayProtection(
    const QString& statePath,
    ProbeState& state)
{
    WindowsDpapiProtector protector;
    {
        RelayStateStore store(
            statePath,
            protector);
        const auto accepted =
            store.acceptInbound(
                QStringLiteral(
                    "probe-channel"),
                QStringLiteral(
                    "probe-sender"),
                41);
        const auto duplicate =
            store.acceptInbound(
                QStringLiteral(
                    "probe-channel"),
                QStringLiteral(
                    "probe-sender"),
                41);
        const auto lower =
            store.acceptInbound(
                QStringLiteral(
                    "probe-channel"),
                QStringLiteral(
                    "probe-sender"),
                40);
        if (!accepted.hasValue()
            || !accepted.value()
            || !duplicate.hasValue()
            || duplicate.value()
            || !lower.hasValue()
            || lower.value()) {
            return state.fail(
                QStringLiteral(
                    "relay_replay"),
                QStringLiteral(
                    "in_memory_replay_check_failed"));
        }
    }

    QFile protectedState(statePath);
    if (!protectedState.open(
            QIODevice::ReadOnly)) {
        return state.fail(
            QStringLiteral(
                "relay_replay"),
            QStringLiteral(
                "state_unreadable"));
    }
    const QByteArray protectedBytes =
        protectedState.readAll();
    protectedState.close();
    if (protectedBytes.contains(
            "probe-channel")
        || protectedBytes.contains(
            "probe-sender")) {
        return state.fail(
            QStringLiteral(
                "relay_replay"),
            QStringLiteral(
                "state_not_protected"));
    }

    RelayStateStore restarted(
        statePath,
        protector);
    const auto replay =
        restarted.acceptInbound(
            QStringLiteral(
                "probe-channel"),
            QStringLiteral(
                "probe-sender"),
            41);
    const auto next =
        restarted.acceptInbound(
            QStringLiteral(
                "probe-channel"),
            QStringLiteral(
                "probe-sender"),
            42);
    if (!replay.hasValue()
        || replay.value()
        || !next.hasValue()
        || !next.value()) {
        return state.fail(
            QStringLiteral(
                "relay_replay"),
            QStringLiteral(
                "persistent_replay_check_failed"));
    }
    ++state.securityFailuresRejected;
    state.relayReplayRejected = true;
    return true;
}

bool runLoopback(
    const Arguments& arguments,
    ProbeState& state,
    QByteArray& secretForRedaction,
    QString& pairingCodeForRedaction)
{
    const bool useSystemPlatform =
        arguments.scenario
        == QStringLiteral(
            "system-nearby");
    if (!validateFixtures(
            arguments.fixtureRoot,
            state)) {
        return false;
    }

    const QSslConfiguration tls =
        testTlsConfiguration();
    if (tls.localCertificate()
            .isNull()
        || tls.privateKey().isNull()) {
        return state.fail(
            QStringLiteral("tls"),
            QStringLiteral(
                "fixture_identity_unavailable"));
    }
    const QString expectedFingerprint =
        certificateFingerprint(
            tls.localCertificate());

    QTemporaryDir temporary(
        QDir(arguments.outputPath)
            .filePath(
                QStringLiteral(
                    "mobile-state-XXXXXX")));
    if (!temporary.isValid()) {
        return state.fail(
            QStringLiteral("storage"),
            QStringLiteral(
                "temporary_state_failed"));
    }
    const QString pairingPath =
        temporary.filePath(
            QStringLiteral(
                "paired-devices.json"));

    RecordingDnsSdApi* dnsApi = nullptr;
    CompanionMobileHostDependencies
        dependencies;
    if (useSystemPlatform) {
        dependencies.nearbyListenAddress =
            QHostAddress::AnyIPv4;
    } else {
        auto dns =
            std::make_unique<
                RecordingDnsSdApi>();
        dnsApi = dns.get();
        dependencies.dnsSdApi =
            std::move(dns);
        dependencies.networkProfileApi =
            std::make_unique<
                PrivateNetworkProfileApi>();
        dependencies.nearbyListenAddress =
            QHostAddress::LocalHost;
    }

    HandlerState handlerState;
    const NearbyRequestHandler handler =
        [&handlerState](
            QString,
            BridgeRequest request) {
            ++handlerState.dispatchCount;
            handlerState.operations.insert(
                static_cast<int>(
                    request.operation));
            if (request.operation
                    == BridgeOperation::
                           SendMessage
                && request.text
                    == QStringLiteral(
                        "inline-probe")
                && request.attachments
                       .has_value()
                && request.attachments
                           ->size()
                       == 1
                && request.attachments
                           ->first()
                           .data
                    == QByteArray(
                        "inline-probe")) {
                handlerState
                    .inlineAttachmentPassed =
                    true;
            }
            if (request.operation
                    == BridgeOperation::
                           SendMessage
                && request.text
                    == QStringLiteral(
                        "streamed-probe")
                && request.attachments
                       .has_value()
                && request.attachments
                           ->size()
                       == 1
                && request.attachments
                           ->first()
                           .data
                           .size()
                    == NearbyFrameCodec::
                               maximumChunkBytes
                           + 19
                && std::all_of(
                    request.attachments
                        ->first()
                        .data
                        .cbegin(),
                    request.attachments
                        ->first()
                        .data
                        .cend(),
                    [](char value) {
                        return value
                            == '\x5A';
                    })) {
                handlerState
                    .streamedAttachmentPassed =
                    true;
            }

            BridgeResponse response;
            response.id = request.id;
            response.operation =
                request.operation;
            response.succeeded = true;
            response.message =
                QStringLiteral("probe-ok");
            return readyResponse(
                std::move(response));
        };

    CompanionMobileHostConfiguration
        configuration;
    configuration.enabled = true;
    configuration
        .allowNearbyOnPublicNetworks =
        useSystemPlatform;
    configuration.installationId =
        kInstallationId.toString();
    configuration.computerName =
        kComputerName.toString();
    configuration.hostDisplayName =
        kHostName.toString();
    configuration.sslConfiguration =
        tls;
    configuration.tlsFingerprintSha256 =
        expectedFingerprint;
    configuration.pairingRecordsPath =
        pairingPath;
    configuration.relayStatePath =
        temporary.filePath(
            QStringLiteral(
                "relay-state.json"));
    configuration.transferRootPath =
        temporary.filePath(
            QStringLiteral(
                "transfers"));
    configuration.relayUrl =
        arguments.relayUrl;

    auto created =
        CompanionMobileHost::create(
            std::move(configuration),
            handler,
            std::move(dependencies));
    if (!created.hasValue()) {
        return state.fail(
            QStringLiteral("host_create"),
            created.error().code);
    }
    std::unique_ptr<CompanionMobileHost>
        host =
            std::move(created.value());
    if (!host->start().hasValue()
        || !host->isListening()
        || !host->isAdvertising()
        || host->serverPort() == 0) {
        return state.fail(
            QStringLiteral("host_start"),
            QStringLiteral(
                "host_not_ready"));
    }
    if (!useSystemPlatform) {
        if (dnsApi == nullptr
            || dnsApi->registerCalls != 1
            || !dnsApi->active.has_value()
            || dnsApi->active->port
                != host->serverPort()
            || dnsApi->active->serviceType
                != WindowsDnsSdAdvertiser::
                       serviceType()
            || dnsApi->active->txt.value(
                   QStringLiteral("path"))
                != NearbyWebSocketServer::
                       requestPath.toString()
            || dnsApi->active->txt.value(
                   QStringLiteral("pv"))
                != QStringLiteral("1")
            || dnsApi->active->txt.value(
                   QStringLiteral("frame"))
                != QStringLiteral("1")
            || dnsApi->active->txt.value(
                   QStringLiteral(
                       "transport"))
                != QStringLiteral("wss")
            || dnsApi->active->txt.value(
                   QStringLiteral("tlsfp"))
                != expectedFingerprint) {
            return state.fail(
                QStringLiteral(
                    "advertisement"),
                QStringLiteral(
                    "advertisement_mismatch"));
        }
    }
    state.advertisementPassed = true;

    const auto activePairing =
        host->pairingCoordinator()
            .beginPairing();
    if (!activePairing.hasValue()) {
        return state.fail(
            QStringLiteral("pairing"),
            activePairing.error().code);
    }
    pairingCodeForRedaction =
        activePairing.value().code;

    SocketSession pairingClient;
    if (!openSocket(
            pairingClient,
            host->serverPort(),
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral("pairing"))) {
        return false;
    }
    const QString peerFingerprint =
        certificateFingerprint(
            pairingClient.socket
                .sslConfiguration()
                .peerCertificate());
    if (peerFingerprint
        != expectedFingerprint) {
        return state.fail(
            QStringLiteral("tls"),
            QStringLiteral(
                "peer_fingerprint_mismatch"));
    }
    state.certificateFingerprintPassed =
        true;

    const auto nonce =
        BridgeSecurity::
            randomInvitationNonce();
    if (!nonce.hasValue()) {
        return state.fail(
            QStringLiteral("pairing"),
            nonce.error().code);
    }
    const BridgeInvitation invitation{
        BridgeSecurity::
            invitationVersion,
        arguments.deviceId,
        QStringLiteral(
            "Windows Probe Client"),
        QDateTime::
            currentMSecsSinceEpoch(),
        nonce.value(),
        std::nullopt,
        activePairing.value().code,
    };
    if (!sendInvitation(
            pairingClient,
            invitation,
            state,
            QStringLiteral("pairing"))) {
        return false;
    }

    BridgeResponse pairingResponse;
    if (!sendRawRequest(
            pairingClient,
            operationRequest(
                BridgeOperation::
                    Handshake),
            arguments.timeoutMilliseconds,
            pairingResponse,
            state,
            QStringLiteral("pairing"))
        || !pairingResponse
                .pairingSecret
                .has_value()
        || pairingResponse
                   .pairingSecret
                   ->size()
               != 32
        || pairingResponse.macDeviceId
            != std::optional<QString>(
                   kInstallationId
                       .toString())) {
        return state.fail(
            QStringLiteral("pairing"),
            QStringLiteral(
                "pairing_response_invalid"));
    }
    secretForRedaction =
        *pairingResponse.pairingSecret;
    pairingResponse.pairingSecret
        ->fill('\0');
    pairingResponse
        .pairingSecret.reset();
    state.pairingPassed = true;
    closeSocket(
        pairingClient,
        arguments.timeoutMilliseconds);

    if (!verifyProtectedPairingStore(
            pairingPath,
            arguments.deviceId,
            secretForRedaction,
            state)) {
        return false;
    }

    SocketSession trustedClient;
    if (!authenticateTrusted(
            trustedClient,
            host->serverPort(),
            arguments.deviceId,
            secretForRedaction,
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "trusted_reconnect"))) {
        return false;
    }
    state.pairingSecretOneTime = true;
    state.trustedReconnectPassed = true;

    const QVector<BridgeOperation>
        operations{
            BridgeOperation::ListTasks,
            BridgeOperation::LoadMessages,
            BridgeOperation::SendMessage,
            BridgeOperation::
                RespondToApproval,
            BridgeOperation::CreateTask,
            BridgeOperation::
                LoadCapabilities,
            BridgeOperation::SendCasualChat,
            BridgeOperation::LoadUsage,
            BridgeOperation::
                ConsumeUsageReset,
            BridgeOperation::CreateGoal,
            BridgeOperation::ResumeGoal,
            BridgeOperation::UpdateGoal,
        };
    for (const BridgeOperation operation :
         operations) {
        BridgeResponse response;
        if (!sendFramedRequest(
                trustedClient,
                operationRequest(
                    operation),
                secretForRedaction,
                arguments
                    .timeoutMilliseconds,
                response,
                state,
                QStringLiteral(
                    "operation.")
                    + operationName(
                        operation))) {
            return false;
        }
    }
    state.operationsPassed =
        1 + handlerState
                .operations.size();
    if (state.operationsPassed != 13
        || !handlerState
                .inlineAttachmentPassed) {
        return state.fail(
            QStringLiteral("operations"),
            QStringLiteral(
                "operation_coverage_incomplete"));
    }
    state.inlineAttachmentPassed =
        true;

    if (!sendStreamedAttachment(
            trustedClient,
            secretForRedaction,
            arguments.timeoutMilliseconds,
            state)
        || !handlerState
                .streamedAttachmentPassed) {
        return state.fail(
            QStringLiteral(
                "streamed_attachment"),
            QStringLiteral(
                "streamed_dispatch_failed"));
    }
    state.streamedAttachmentPassed =
        true;
    closeSocket(
        trustedClient,
        arguments.timeoutMilliseconds);

    QByteArray wrongSecret(
        secretForRedaction.data(),
        secretForRedaction.size());
    wrongSecret[0] =
        static_cast<char>(
            wrongSecret.at(0) ^ 0x5A);
    const auto wrongNonce =
        BridgeSecurity::
            randomInvitationNonce();
    if (!wrongNonce.hasValue()) {
        return state.fail(
            QStringLiteral(
                "wrong_invitation_hmac"),
            wrongNonce.error().code);
    }
    const auto wrongInvitation =
        BridgeSecurity::
            authenticatedInvitation(
                arguments.deviceId,
                QStringLiteral(
                    "Windows Probe Client"),
                wrongSecret,
                QDateTime::
                    currentDateTimeUtc(),
                wrongNonce.value());
    if (!wrongInvitation.hasValue()
        || !expectRejectedInvitation(
            host->serverPort(),
            wrongInvitation.value(),
            4401,
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "wrong_invitation_hmac"))) {
        wrongSecret.fill('\0');
        return false;
    }

    const auto staleNonce =
        BridgeSecurity::
            randomInvitationNonce();
    if (!staleNonce.hasValue()) {
        wrongSecret.fill('\0');
        return state.fail(
            QStringLiteral(
                "stale_invitation"),
            staleNonce.error().code);
    }
    const auto staleInvitation =
        BridgeSecurity::
            authenticatedInvitation(
                arguments.deviceId,
                QStringLiteral(
                    "Windows Probe Client"),
                secretForRedaction,
                QDateTime::
                    currentDateTimeUtc()
                    .addSecs(-300),
                staleNonce.value());
    if (!staleInvitation.hasValue()
        || !expectRejectedInvitation(
            host->serverPort(),
            staleInvitation.value(),
            4401,
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "stale_invitation"))) {
        wrongSecret.fill('\0');
        return false;
    }

    BridgeRequest invalidRequest =
        operationRequest(
            BridgeOperation::ListTasks);
    const auto invalidPayload =
        BridgeJsonCodec::encodeRequest(
            invalidRequest,
            BridgeWireProfile::
                NearbyV1Milliseconds);
    const auto wrongFrame =
        invalidPayload.hasValue()
        ? NearbyFrameCodec::encode(
              {
                  NearbyFrameType::Request,
                  0,
                  invalidRequest.id,
                  {},
                  0,
                  0,
                  invalidPayload.value(),
              },
              wrongSecret)
        : Result<QByteArray>::failure(
              invalidPayload.error());
    wrongSecret.fill('\0');
    if (!wrongFrame.hasValue()
        || !expectInvalidFrame(
            host->serverPort(),
            arguments.deviceId,
            secretForRedaction,
            wrongFrame.value(),
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "wrong_frame_hmac"),
            4401)
        || !expectInvalidFrame(
            host->serverPort(),
            arguments.deviceId,
            secretForRedaction,
            QByteArray("corrupt-frame"),
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "corrupt_frame"),
            4401)) {
        return false;
    }
    if (!expectAttachmentMetadataMismatch(
            host->serverPort(),
            arguments.deviceId,
            secretForRedaction,
            arguments.timeoutMilliseconds,
            state)
        || !verifyRelayReplayProtection(
            temporary.filePath(
                QStringLiteral(
                    "relay-replay-state.dpapi")),
            state)) {
        return false;
    }

    if (!host->applyConfiguration(
                 false,
                 false,
                 std::nullopt)
             .hasValue()
        || !waitUntil(
            [&host] {
                return !host->isListening()
                    && !host
                            ->isAdvertising();
            },
            arguments
                .timeoutMilliseconds)
        || !host->applyConfiguration(
                 true,
                 useSystemPlatform,
                 std::nullopt)
             .hasValue()
        || !waitUntil(
            [&host] {
                return host->isListening()
                    && host
                           ->isAdvertising()
                    && host->serverPort()
                           > 0;
            },
            arguments
                .timeoutMilliseconds)) {
        return state.fail(
            QStringLiteral("lifecycle"),
            QStringLiteral(
                "disable_enable_failed"));
    }
    SocketSession lifecycleClient;
    if (!authenticateTrusted(
            lifecycleClient,
            host->serverPort(),
            arguments.deviceId,
            secretForRedaction,
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "lifecycle_reconnect"))) {
        return false;
    }
    closeSocket(
        lifecycleClient,
        arguments.timeoutMilliseconds);
    state.lifecyclePassed = true;

    const auto forgotten =
        host->pairingCoordinator()
            .forget(
                arguments.deviceId);
    if (!forgotten.hasValue()
        || host->pairingCoordinator()
               .trustedRecord(
                   arguments.deviceId)
               .has_value()) {
        return state.fail(
            QStringLiteral("forget"),
            forgotten.hasValue()
                ? QStringLiteral(
                      "record_retained")
                : forgotten.error().code);
    }
    const auto forgottenNonce =
        BridgeSecurity::
            randomInvitationNonce();
    if (!forgottenNonce.hasValue()) {
        return state.fail(
            QStringLiteral("forget"),
            forgottenNonce.error().code);
    }
    const auto forgottenInvitation =
        BridgeSecurity::
            authenticatedInvitation(
                arguments.deviceId,
                QStringLiteral(
                    "Windows Probe Client"),
                secretForRedaction,
                QDateTime::
                    currentDateTimeUtc(),
                forgottenNonce.value());
    if (!forgottenInvitation.hasValue()
        || !expectRejectedInvitation(
            host->serverPort(),
            forgottenInvitation.value(),
            4401,
            arguments.timeoutMilliseconds,
            state,
            QStringLiteral(
                "forget_reconnect"))) {
        return false;
    }
    state.forgetPassed = true;

    host->stop();
    if (host->isListening()
        || host->isAdvertising()
        || (!useSystemPlatform
            && (dnsApi == nullptr
                || dnsApi->active
                       .has_value()
                || dnsApi
                       ->deregisterCalls
                       < 2))) {
        return state.fail(
            QStringLiteral("host_stop"),
            QStringLiteral(
                "host_still_active"));
    }
    return true;
}

std::optional<Arguments> parseArguments(
    QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral(
            "Codex Companion Windows mobile transport probe"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption scenario(
        QStringLiteral("scenario"),
        QStringLiteral(
            "Probe scenario: loopback, system-nearby, nearby, relay, or all."),
        QStringLiteral("name"));
    const QCommandLineOption output(
        QStringLiteral("output"),
        QStringLiteral(
            "Absolute output directory."),
        QStringLiteral("path"));
    const QCommandLineOption fixtureRoot(
        QStringLiteral("fixture-root"),
        QStringLiteral(
            "Absolute mobile fixture directory."),
        QStringLiteral("path"));
    const QCommandLineOption deviceId(
        QStringLiteral("device-id"),
        QStringLiteral(
            "Test client device identifier."),
        QStringLiteral("id"),
        QStringLiteral(
            "windows-loopback-probe"));
    const QCommandLineOption relayUrl(
        QStringLiteral("relay-url"),
        QStringLiteral(
            "Optional secure test relay URL."),
        QStringLiteral("url"));
    const QCommandLineOption timeout(
        QStringLiteral(
            "timeout-seconds"),
        QStringLiteral(
            "Per-step timeout in seconds."),
        QStringLiteral("seconds"),
        QStringLiteral("60"));
    parser.addOptions({
        scenario,
        output,
        fixtureRoot,
        deviceId,
        relayUrl,
        timeout,
    });
    parser.process(application);

    Arguments result;
    result.scenario =
        parser.value(scenario)
            .trimmed()
            .toLower();
    result.outputPath =
        QFileInfo(
            parser.value(output))
            .absoluteFilePath();
    result.fixtureRoot =
        QFileInfo(
            parser.value(
                fixtureRoot))
            .absoluteFilePath();
    result.deviceId =
        parser.value(deviceId)
            .trimmed();

    bool timeoutOk = false;
    const int timeoutSeconds =
        parser.value(timeout)
            .toInt(&timeoutOk);
    const QString rawOutput =
        parser.value(output);
    const QString rawFixtureRoot =
        parser.value(fixtureRoot);
    if ((result.scenario
            != QStringLiteral(
                "loopback")
         && result.scenario
                != QStringLiteral(
                    "system-nearby")
         && result.scenario
                != QStringLiteral(
                    "nearby")
         && result.scenario
                != QStringLiteral(
                    "relay")
         && result.scenario
                != QStringLiteral(
                    "all"))
        || rawOutput.isEmpty()
        || !QFileInfo(rawOutput)
                .isAbsolute()
        || rawFixtureRoot.isEmpty()
        || !QFileInfo(rawFixtureRoot)
                .isAbsolute()
        || result.deviceId.isEmpty()
        || !timeoutOk
        || timeoutSeconds <= 0
        || timeoutSeconds > 600) {
        QTextStream(stderr)
            << "Invalid probe arguments. Use --help for the contract.\n";
        return std::nullopt;
    }
    result.timeoutMilliseconds =
        timeoutSeconds * 1000;

    if (parser.isSet(relayUrl)) {
        const QUrl url(
            parser.value(relayUrl));
        if (!url.isValid()
            || url.scheme()
                != QStringLiteral(
                    "wss")) {
            QTextStream(stderr)
                << "--relay-url must be a valid wss URL.\n";
            return std::nullopt;
        }
        result.relayUrl = url;
    }
    return result;
}

QJsonObject checkSummary(
    const ProbeState& state)
{
    return {
        {
            QStringLiteral(
                "fixtureContract"),
            state.fixtureContractPassed,
        },
        {
            QStringLiteral(
                "advertisement"),
            state.advertisementPassed,
        },
        {
            QStringLiteral(
                "certificateFingerprint"),
            state
                .certificateFingerprintPassed,
        },
        {
            QStringLiteral("pairing"),
            state.pairingPassed,
        },
        {
            QStringLiteral(
                "pairingSecretOneTime"),
            state.pairingSecretOneTime,
        },
        {
            QStringLiteral(
                "trustedReconnect"),
            state.trustedReconnectPassed,
        },
        {
            QStringLiteral(
                "dpapiPersistence"),
            state.dpapiPersistencePassed,
        },
        {
            QStringLiteral(
                "inlineAttachment"),
            state.inlineAttachmentPassed,
        },
        {
            QStringLiteral(
                "streamedAttachment"),
            state.streamedAttachmentPassed,
        },
        {
            QStringLiteral(
                "metadataMismatchRejected"),
            state.metadataMismatchRejected,
        },
        {
            QStringLiteral(
                "relayReplayRejected"),
            state.relayReplayRejected,
        },
        {
            QStringLiteral(
                "disableEnable"),
            state.lifecyclePassed,
        },
        {
            QStringLiteral("forget"),
            state.forgetPassed,
        },
    };
}

QJsonObject makeSummary(
    const Arguments& arguments,
    const ProbeState& state,
    bool passed,
    int forbiddenMatches)
{
    QJsonArray failures;
    for (const QString& failure :
         state.failures) {
        failures.append(failure);
    }
    QJsonArray externalGates;
    if (arguments.scenario
            != QStringLiteral(
                "loopback")) {
        externalGates.append(
            QStringLiteral(
                "published_iphone_nearby"));
        externalGates.append(
            QStringLiteral(
                "published_iphone_relay"));
    }
    return {
        {
            QStringLiteral("passed"),
            passed,
        },
        {
            QStringLiteral("scenario"),
            arguments.scenario,
        },
        {
            QStringLiteral(
                "bridgeProtocol"),
            kBridgeProtocolVersion,
        },
        {
            QStringLiteral(
                "nearbyContract"),
            QStringLiteral("CCN1-v1"),
        },
        {
            QStringLiteral(
                "relayProtocol"),
            1,
        },
        {
            QStringLiteral(
                "operationsPassed"),
            state.operationsPassed,
        },
        {
            QStringLiteral(
                "securityFailuresRejected"),
            state
                .securityFailuresRejected,
        },
        {
            QStringLiteral(
                "forbiddenLogMatches"),
            forbiddenMatches,
        },
        {
            QStringLiteral("checks"),
            checkSummary(state),
        },
        {
            QStringLiteral("failures"),
            failures,
        },
        {
            QStringLiteral(
                "externalGates"),
            externalGates,
        },
        {
            QStringLiteral("artifacts"),
            QJsonArray{
                QStringLiteral(
                    "summary.json"),
            },
        },
    };
}

bool writeSummary(
    const QString& outputPath,
    const QJsonObject& summary)
{
    QDir directory;
    if (!directory.mkpath(
            outputPath)) {
        return false;
    }
    QSaveFile file(
        QDir(outputPath).filePath(
            QStringLiteral(
                "summary.json")));
    if (!file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate)) {
        return false;
    }
    file.write(
        QJsonDocument(summary)
            .toJson(
                QJsonDocument::Indented));
    return file.commit();
}

int forbiddenMatchCount(
    const QJsonObject& summary,
    const QString& pairingCode,
    QByteArrayView secret)
{
    const QByteArray bytes =
        QJsonDocument(summary)
            .toJson(
                QJsonDocument::Compact);
    int matches = 0;
    if (!pairingCode.isEmpty()
        && bytes.contains(
            pairingCode.toUtf8())) {
        ++matches;
    }
    if (!secret.isEmpty()) {
        const QByteArray plain(
            secret.data(),
            secret.size());
        if (bytes.contains(plain)) {
            ++matches;
        }
        if (bytes.contains(
                plain.toBase64())) {
            ++matches;
        }
        if (bytes.contains(
                plain.toHex())) {
            ++matches;
        }
    }
    return matches;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(
        argc,
        argv);
    QCoreApplication::setApplicationName(
        QStringLiteral(
            "companion-mobile-probe"));
    QCoreApplication::
        setApplicationVersion(
            QStringLiteral("0.3.4"));

    const auto parsed =
        parseArguments(application);
    if (!parsed.has_value()) {
        return 2;
    }
    const Arguments arguments =
        *parsed;
    if (!QDir().mkpath(
            arguments.outputPath)) {
        QTextStream(stderr)
            << "The probe output directory could not be created.\n";
        return 2;
    }

    ProbeState state;
    QByteArray secret;
    bool passed = false;
    QString pairingCode;
    if (arguments.scenario
            == QStringLiteral(
                "loopback")
        || arguments.scenario
            == QStringLiteral(
                "system-nearby")) {
        passed = runLoopback(
            arguments,
            state,
            secret,
            pairingCode);
    } else {
        state.fail(
            QStringLiteral("scenario"),
            QStringLiteral(
                "published_client_gate_required"));
    }

    QJsonObject summary =
        makeSummary(
            arguments,
            state,
            passed
                && state.failures
                       .isEmpty(),
            0);
    const int forbiddenMatches =
        forbiddenMatchCount(
            summary,
            pairingCode,
            secret);
    if (forbiddenMatches > 0) {
        passed = false;
        state.fail(
            QStringLiteral("redaction"),
            QStringLiteral(
                "forbidden_content_found"));
        summary = makeSummary(
            arguments,
            state,
            false,
            forbiddenMatches);
    }
    secret.fill('\0');

    if (!writeSummary(
            arguments.outputPath,
            summary)) {
        QTextStream(stderr)
            << "summary.json could not be written.\n";
        return 2;
    }

    if (passed
        && state.failures.isEmpty()) {
        QTextStream(stdout)
            << "Mobile "
            << arguments.scenario
            << " probe passed: 13 operations, "
            << state.securityFailuresRejected
            << " rejection checks.\n";
        return 0;
    }
    QTextStream(stderr)
        << "Mobile probe failed; see summary.json.\n";
    return arguments.scenario
                   == QStringLiteral(
                       "loopback")
        ? 1
        : 3;
}
