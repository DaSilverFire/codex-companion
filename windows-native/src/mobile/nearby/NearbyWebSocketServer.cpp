#include "mobile/nearby/NearbyWebSocketServer.h"

#include "codex/models/BridgeJsonCodec.h"
#include "mobile/nearby/NearbyFrameCodec.h"
#include "mobile/nearby/NearbyTransferAssembler.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"
#include "mobile/security/PairingCoordinator.h"
#include "platform/windows/security/WindowsCrypto.h"

#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QSslCertificate>
#include <QSslKey>
#include <QUuid>
#include <QtWebSockets/QWebSocket>
#include <QtWebSockets/QWebSocketProtocol>
#include <QtWebSockets/QWebSocketServer>

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace companion {
namespace {

CompanionError nearbyServerError(
    QString code,
    QString message,
    bool retryable = false)
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        {},
    };
}

QFuture<void> readyVoidFuture()
{
    QPromise<void> promise;
    promise.start();
    QFuture<void> future =
        promise.future();
    promise.finish();
    return future;
}

QFuture<Result<void>> readyResultFuture(
    Result<void> result)
{
    QPromise<Result<void>> promise;
    promise.start();
    QFuture<Result<void>> future =
        promise.future();
    promise.addResult(
        std::move(result));
    promise.finish();
    return future;
}

bool mayAdvertise(
    NearbyNetworkProfile profile,
    bool allowPublicNetwork)
{
    return profile
            == NearbyNetworkProfile::
                Private
        || profile
            == NearbyNetworkProfile::
                Domain
        || (profile
                == NearbyNetworkProfile::
                    Public
            && allowPublicNetwork);
}

QString trimUtf8(
    QString value,
    qsizetype maximumBytes)
{
    QString result;
    const QList<uint> scalars =
        value.toUcs4();
    for (const uint scalarValue :
         scalars) {
        const char32_t scalar =
            static_cast<char32_t>(
                scalarValue);
        const QString piece =
            QString::fromUcs4(
                &scalar,
                1);
        if ((result + piece)
                .toUtf8()
                .size()
            > maximumBytes) {
            break;
        }
        result += piece;
    }
    return result;
}

bool txtFitsDnsSd(
    const QMap<QString, QString>& txt)
{
    for (auto iterator = txt.cbegin();
         iterator != txt.cend();
         ++iterator) {
        if (iterator.key().isEmpty()
            || iterator.key()
                   .contains(
                       QLatin1Char('='))
            || (iterator.key()
                    + QLatin1Char('=')
                    + iterator.value())
                       .toUtf8()
                       .size()
                > 255) {
            return false;
        }
    }
    return true;
}

Result<QByteArray> decodeBase64(
    const QJsonValue& value)
{
    if (!value.isString()) {
        return Result<QByteArray>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }
    const auto decoded =
        QByteArray::fromBase64Encoding(
            value.toString().toLatin1(),
            QByteArray::
                AbortOnBase64DecodingErrors);
    if (!decoded) {
        return Result<QByteArray>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }
    return Result<QByteArray>::success(
        decoded.decoded);
}

std::optional<qint64> exactInteger(
    const QJsonValue& value)
{
    constexpr double kMaximumExactJsonInteger =
        9'007'199'254'740'991.0;
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number =
        value.toDouble();
    if (!std::isfinite(number)
        || std::trunc(number) != number
        || number
            < -kMaximumExactJsonInteger
        || number
            > kMaximumExactJsonInteger) {
        return std::nullopt;
    }
    const qint64 integer =
        static_cast<qint64>(number);
    if (static_cast<double>(integer)
        != number) {
        return std::nullopt;
    }
    return integer;
}

Result<BridgeInvitation>
decodeInvitation(
    QByteArrayView bytes)
{
    if (bytes.isEmpty()
        || bytes.size() > 16 * 1024) {
        return Result<
            BridgeInvitation>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            bytes.toByteArray(),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<
            BridgeInvitation>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }
    const QJsonObject object =
        document.object();
    const QSet<QString> allowed{
        QStringLiteral("version"),
        QStringLiteral("deviceID"),
        QStringLiteral("displayName"),
        QStringLiteral(
            "issuedAtMilliseconds"),
        QStringLiteral("nonce"),
        QStringLiteral(
            "authenticator"),
        QStringLiteral("pairingCode"),
    };
    for (auto iterator =
             object.constBegin();
         iterator
         != object.constEnd();
         ++iterator) {
        if (!allowed.contains(
                iterator.key())) {
            return Result<
                BridgeInvitation>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.invalid_invitation"),
                    QStringLiteral(
                        "The nearby invitation is invalid.")));
        }
    }

    const auto version =
        exactInteger(
            object.value(
                QStringLiteral(
                    "version")));
    const auto issuedAt =
        exactInteger(
            object.value(
                QStringLiteral(
                    "issuedAtMilliseconds")));
    const QJsonValue deviceValue =
        object.value(
            QStringLiteral("deviceID"));
    const QJsonValue displayValue =
        object.value(
            QStringLiteral(
                "displayName"));
    const auto nonce =
        decodeBase64(
            object.value(
                QStringLiteral("nonce")));
    if (!version.has_value()
        || *version
            < std::numeric_limits<int>::
                  min()
        || *version
            > std::numeric_limits<int>::
                  max()
        || !issuedAt.has_value()
        || !deviceValue.isString()
        || !displayValue.isString()
        || !nonce.hasValue()) {
        return Result<
            BridgeInvitation>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }

    const QString deviceId =
        deviceValue.toString();
    const QString displayName =
        displayValue.toString();
    if (deviceId.trimmed().isEmpty()
        || deviceId.toUtf8().size() > 256
        || displayName.toUtf8().size()
            > 256
        || nonce.value().size() != 16) {
        return Result<
            BridgeInvitation>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_invitation"),
                QStringLiteral(
                    "The nearby invitation is invalid.")));
    }

    std::optional<QByteArray>
        authenticator;
    const auto authenticatorValue =
        object.constFind(
            QStringLiteral(
                "authenticator"));
    if (authenticatorValue
        != object.constEnd()) {
        const auto decoded =
            decodeBase64(
                *authenticatorValue);
        if (!decoded.hasValue()
            || decoded.value().size()
                != 32) {
            return Result<
                BridgeInvitation>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.invalid_invitation"),
                    QStringLiteral(
                        "The nearby invitation is invalid.")));
        }
        authenticator =
            decoded.value();
    }

    std::optional<QString> pairingCode;
    const auto pairingValue =
        object.constFind(
            QStringLiteral(
                "pairingCode"));
    if (pairingValue
        != object.constEnd()) {
        if (!pairingValue->isString()
            || pairingValue
                   ->toString()
                   .size()
                > 64) {
            return Result<
                BridgeInvitation>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.invalid_invitation"),
                    QStringLiteral(
                        "The nearby invitation is invalid.")));
        }
        pairingCode =
            pairingValue->toString();
    }

    return Result<
        BridgeInvitation>::success({
        static_cast<int>(*version),
        deviceId,
        displayName,
        *issuedAt,
        nonce.value(),
        std::move(authenticator),
        std::move(pairingCode),
    });
}

BridgeResponse failureResponse(
    const BridgeRequest& request,
    QString code,
    QString message)
{
    BridgeResponse response;
    response.id = request.id;
    response.operation =
        request.operation;
    response.succeeded = false;
    response.errorCode =
        std::move(code);
    response.message =
        std::move(message);
    return response;
}

} // namespace

struct NearbyWebSocketServer::
Implementation final {
    struct ClientState final {
        enum class Authorization {
            AwaitingInvitation,
            Pairing,
            Authenticated,
        };

        ClientState(
            QWebSocket* requestedSocket,
            const QString& rootPath)
            : socket(requestedSocket),
              assembler(
                  std::make_unique<
                      NearbyTransferAssembler>(
                      rootPath))
        {
        }

        ~ClientState()
        {
            WindowsCrypto::secureZero(
                secret);
        }

        QPointer<QWebSocket> socket;
        Authorization authorization =
            Authorization::
                AwaitingInvitation;
        QString deviceId;
        QByteArray secret;
        std::optional<BridgeInvitation>
            pairingInvitation;
        std::unique_ptr<
            NearbyTransferAssembler>
            assembler;
        quint64 generation = 1;
    };

    Implementation(
        PairingCoordinator& requestedPairing,
        NearbyRequestHandler requestedHandler,
        NearbyWebSocketServerOptions
            requestedOptions)
        : pairingCoordinator(
              &requestedPairing),
          requestHandler(
              std::move(requestedHandler)),
          options(
              std::move(requestedOptions)),
          server(
              QStringLiteral(
                  "Codex Companion Nearby"),
              QWebSocketServer::SecureMode)
    {
        QObject::connect(
            &server,
            &QWebSocketServer::
                newConnection,
            &server,
            [this]() {
                acceptConnections();
            });
    }

    void closeSocket(
        QWebSocket* socket,
        int code,
        const QString& reason)
    {
        if (socket == nullptr) {
            return;
        }
        socket->close(
            static_cast<
                QWebSocketProtocol::
                    CloseCode>(code),
            reason);
    }

    std::shared_ptr<ClientState>
    clientState(
        QWebSocket* socket) const
    {
        return clients.value(socket);
    }

    void removeSocket(
        QWebSocket* socket)
    {
        const auto state =
            clients.take(socket);
        if (state != nullptr
            && !state->deviceId.isEmpty()
            && devices.value(
                   state->deviceId)
                == socket) {
            devices.remove(
                state->deviceId);
        }
        if (state != nullptr
            && state->assembler
                != nullptr) {
            state->assembler
                ->cancelAll();
        }
        if (socket != nullptr) {
            socket->deleteLater();
        }
    }

    void invalidateState(
        const std::shared_ptr<
            ClientState>& state)
    {
        if (state == nullptr) {
            return;
        }
        ++state->generation;
        state->authorization =
            ClientState::Authorization::
                AwaitingInvitation;
        state->deviceId.clear();
        state->pairingInvitation.reset();
        WindowsCrypto::secureZero(
            state->secret);
        if (state->assembler != nullptr) {
            state->assembler
                ->cancelAll();
        }
    }

    Result<void> bindAuthenticated(
        const std::shared_ptr<
            ClientState>& state,
        QString deviceId,
        QByteArray secret)
    {
        if (state == nullptr
            || state->socket == nullptr
            || deviceId.trimmed().isEmpty()
            || secret.size() != 32) {
            WindowsCrypto::secureZero(
                secret);
            return Result<void>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.invalid_pairing_record"),
                    QStringLiteral(
                        "The nearby pairing record is invalid.")));
        }

        QWebSocket* const existing =
            devices.value(deviceId);
        if (existing != nullptr
            && existing
                != state->socket) {
            const auto existingState =
                clientState(existing);
            invalidateState(
                existingState);
            closeSocket(
                existing,
                4409,
                QStringLiteral(
                    "connection_replaced"));
        }

        state->authorization =
            ClientState::Authorization::
                Authenticated;
        state->deviceId =
            std::move(deviceId);
        state->pairingInvitation.reset();
        WindowsCrypto::secureZero(
            state->secret);
        state->secret =
            std::move(secret);
        devices.insert(
            state->deviceId,
            state->socket);
        return Result<void>::success();
    }

    void handleInvitation(
        QWebSocket* socket,
        const QString& text)
    {
        const auto state =
            clientState(socket);
        if (state == nullptr
            || state->authorization
                != ClientState::
                       Authorization::
                           AwaitingInvitation) {
            closeSocket(
                socket,
                4403,
                QStringLiteral(
                    "identity_locked"));
            return;
        }
        const QByteArray bytes =
            text.toUtf8();
        const auto invitation =
            decodeInvitation(bytes);
        if (!invitation.hasValue()) {
            closeSocket(
                socket,
                4400,
                QStringLiteral(
                    "invalid_invitation"));
            return;
        }

        const InvitationDecision decision =
            pairingCoordinator
                ->invitationDecision(
                    invitation.value());
        if (decision
            == InvitationDecision::
                AcceptTrusted) {
            auto record =
                pairingCoordinator
                    ->trustedRecord(
                        invitation.value()
                            .deviceId);
            if (!record.has_value()) {
                closeSocket(
                    socket,
                    4401,
                    QStringLiteral(
                        "invitation_rejected"));
                return;
            }
            QByteArray secret =
                std::move(
                    record->secret);
            const auto bound =
                bindAuthenticated(
                    state,
                    invitation.value()
                        .deviceId,
                    std::move(secret));
            if (!bound.hasValue()) {
                closeSocket(
                    socket,
                    4401,
                    QStringLiteral(
                        "invitation_rejected"));
            }
            return;
        }
        if (decision
            == InvitationDecision::
                AcceptPairing) {
            state->authorization =
                ClientState::
                    Authorization::
                        Pairing;
            state->pairingInvitation =
                invitation.value();
            return;
        }

        closeSocket(
            socket,
            4401,
            QStringLiteral(
                "invitation_rejected"));
    }

    Result<void> sendResponse(
        const std::shared_ptr<
            ClientState>& state,
        const BridgeResponse& response)
    {
        if (state == nullptr
            || state->socket == nullptr
            || state->authorization
                != ClientState::
                       Authorization::
                           Authenticated
            || state->secret.size()
                != 32
            || state->socket->state()
                != QAbstractSocket::
                       ConnectedState) {
            return Result<void>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.not_authenticated"),
                    QStringLiteral(
                        "The nearby device is not connected."),
                    true));
        }
        const auto payload =
            BridgeJsonCodec::
                encodeResponse(
                    response,
                    BridgeWireProfile::
                        NearbyV1Milliseconds);
        if (!payload.hasValue()) {
            return Result<void>::failure(
                payload.error());
        }
        const auto encoded =
            NearbyFrameCodec::encode(
                {
                    NearbyFrameType::
                        Request,
                    0,
                    response.id,
                    {},
                    0,
                    0,
                    payload.value(),
                },
                state->secret);
        if (!encoded.hasValue()) {
            return Result<void>::failure(
                encoded.error());
        }
        if (state->socket
                ->sendBinaryMessage(
                    encoded.value())
            != encoded.value().size()) {
            return Result<void>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.send_failed"),
                    QStringLiteral(
                        "The nearby response could not be queued."),
                    true));
        }
        return Result<void>::success();
    }

    BridgeResponse handshakeResponse(
        const BridgeRequest& request,
        std::optional<QByteArray>
            pairingSecret =
                std::nullopt) const
    {
        BridgeResponse response;
        response.id = request.id;
        response.operation =
            request.operation;
        response.succeeded = true;
        response.macName =
            options.hostDisplayName
                    .trimmed()
                    .isEmpty()
                ? NearbyWebSocketServer::
                      serviceInstanceName(
                          options.computerName)
                : options.hostDisplayName;
        response.macDeviceId =
            options.installationId;
        response.pairingSecret =
            std::move(pairingSecret);
        response.relayUrlString =
            options.relayUrlString;
        if (options
                .presencePetCatalogService) {
            response.features =
                QVector<BridgeFeature>{
                    BridgeFeature::
                        PresencePetPackageV1,
                };
            const auto presentation =
                options
                    .presencePetCatalogService
                    ->presentation();
            response.selectedDesktopPetId =
                presentation
                    .selectedDesktopPetId;
            response.presencePetCatalog =
                presentation.catalog;
        }
        return response;
    }

    void handlePairingHandshake(
        const std::shared_ptr<
            ClientState>& state,
        const QByteArray& bytes)
    {
        if (state == nullptr
            || state->socket == nullptr
            || !state
                    ->pairingInvitation
                    .has_value()
            || bytes.isEmpty()
            || bytes.size()
                > NearbyFrameCodec::
                      maximumRequestBytes) {
            closeSocket(
                state != nullptr
                        ? state->socket
                              .data()
                        : nullptr,
                4403,
                QStringLiteral(
                    "pairing_incomplete"));
            return;
        }

        const auto decoded =
            BridgeJsonCodec::decodeRequest(
                bytes,
                BridgeWireProfile::
                    NearbyV1Milliseconds);
        if (!decoded.hasValue()
            || decoded.value().id.isNull()
            || decoded.value()
                   .protocolVersion
                != kBridgeProtocolVersion
            || decoded.value().operation
                != BridgeOperation::
                       Handshake
            || (decoded.value()
                    .attachments
                    .has_value()
                && !decoded.value()
                         .attachments
                         ->isEmpty())) {
            closeSocket(
                state->socket,
                4403,
                QStringLiteral(
                    "pairing_incomplete"));
            return;
        }

        auto completed =
            pairingCoordinator
                ->completePairing(
                    *state
                         ->pairingInvitation);
        if (!completed.hasValue()) {
            closeSocket(
                state->socket,
                4401,
                QStringLiteral(
                    "pairing_rejected"));
            return;
        }
        PairingRecord record =
            std::move(
                completed.value());
        BridgeResponse response =
            handshakeResponse(
                decoded.value(),
                record.secret);
        auto encoded =
            BridgeJsonCodec::
                encodeResponse(
                    response,
                    BridgeWireProfile::
                        NearbyV1Milliseconds);
        if (response.pairingSecret
                .has_value()) {
            WindowsCrypto::secureZero(
                *response
                     .pairingSecret);
        }
        if (!encoded.hasValue()) {
            WindowsCrypto::secureZero(
                record.secret);
            closeSocket(
                state->socket,
                4400,
                QStringLiteral(
                    "pairing_failed"));
            return;
        }

        const qsizetype encodedSize =
            encoded.value().size();
        const qint64 queued =
            state->socket
                ->sendBinaryMessage(
                    encoded.value());
        WindowsCrypto::secureZero(
            encoded.value());
        if (queued != encodedSize) {
            WindowsCrypto::secureZero(
                record.secret);
            closeSocket(
                state->socket,
                4411,
                QStringLiteral(
                    "send_failed"));
            return;
        }

        const auto bound =
            bindAuthenticated(
                state,
                record.deviceId,
                std::move(
                    record.secret));
        if (!bound.hasValue()) {
            closeSocket(
                state->socket,
                4401,
                QStringLiteral(
                    "pairing_rejected"));
        }
    }

    void dispatchRequest(
        const std::shared_ptr<
            ClientState>& state,
        BridgeRequest request)
    {
        if (state == nullptr
            || state->authorization
                != ClientState::
                       Authorization::
                           Authenticated) {
            return;
        }
        if (request.protocolVersion
            != kBridgeProtocolVersion) {
            sendResponse(
                state,
                failureResponse(
                    request,
                    QStringLiteral(
                        "protocol_mismatch"),
                    QStringLiteral(
                        "Update Codex Companion on Windows and iPhone.")));
            return;
        }
        if (request.operation
            == BridgeOperation::
                Handshake) {
            sendResponse(
                state,
                handshakeResponse(
                    request));
            return;
        }
        if (!requestHandler) {
            sendResponse(
                state,
                failureResponse(
                    request,
                    QStringLiteral(
                        "bridge_unavailable"),
                    QStringLiteral(
                        "The Windows Companion bridge is unavailable.")));
            return;
        }

        QFuture<BridgeResponse> future;
        try {
            future = requestHandler(
                state->deviceId,
                request);
        } catch (...) {
            sendResponse(
                state,
                failureResponse(
                    request,
                    QStringLiteral(
                        "bridge_failed"),
                    QStringLiteral(
                        "The Windows Companion bridge failed.")));
            return;
        }
        if (!future.isValid()) {
            sendResponse(
                state,
                failureResponse(
                    request,
                    QStringLiteral(
                        "bridge_failed"),
                    QStringLiteral(
                        "The Windows Companion bridge failed.")));
            return;
        }

        auto* watcher =
            new QFutureWatcher<
                BridgeResponse>(
                &server);
        const QPointer<QWebSocket>
            guardedSocket =
                state->socket;
        const quint64 generation =
            state->generation;
        QObject::connect(
            watcher,
            &QFutureWatcherBase::
                finished,
            &server,
            [this,
             watcher,
             guardedSocket,
             generation,
             request]() {
                BridgeResponse response =
                    failureResponse(
                        request,
                        QStringLiteral(
                            "bridge_failed"),
                        QStringLiteral(
                            "The Windows Companion bridge failed."));
                try {
                    const auto completed =
                        watcher->future();
                    if (completed.isValid()
                        && !completed
                                .isCanceled()
                        && completed
                               .resultCount()
                            == 1) {
                        response =
                            completed.result();
                    }
                } catch (...) {
                }
                watcher->deleteLater();

                if (guardedSocket
                    == nullptr) {
                    return;
                }
                const auto current =
                    clientState(
                        guardedSocket);
                if (current == nullptr
                    || current->generation
                        != generation
                    || current
                           ->authorization
                        != ClientState::
                               Authorization::
                                   Authenticated) {
                    return;
                }
                sendResponse(
                    current,
                    response);
            },
            Qt::QueuedConnection);
        watcher->setFuture(future);
    }

    void handleBinary(
        QWebSocket* socket,
        const QByteArray& bytes)
    {
        const auto state =
            clientState(socket);
        if (state == nullptr) {
            return;
        }
        if (state->authorization
            == ClientState::
                   Authorization::
                       Pairing) {
            handlePairingHandshake(
                state,
                bytes);
            return;
        }
        if (state->authorization
                != ClientState::
                       Authorization::
                           Authenticated
            || state->secret.size()
                != 32) {
            closeSocket(
                socket,
                4403,
                QStringLiteral(
                    "invitation_required"));
            return;
        }

        const auto frame =
            NearbyFrameCodec::decode(
                bytes,
                state->secret);
        if (!frame.hasValue()) {
            closeSocket(
                socket,
                4401,
                QStringLiteral(
                    "invalid_frame"));
            return;
        }
        const auto assembled =
            state->assembler->consume(
                frame.value());
        if (!assembled.hasValue()) {
            closeSocket(
                socket,
                4400,
                QStringLiteral(
                    "invalid_transfer"));
            return;
        }
        if (assembled.value()
                .disposition
                == NearbyAssemblyDisposition::
                    Dispatch
            && assembled.value()
                   .request
                   .has_value()) {
            dispatchRequest(
                state,
                std::move(
                    *assembled.value()
                         .request));
        }
    }

    void trackSocket(
        QWebSocket* socket)
    {
        if (socket == nullptr) {
            return;
        }
        socket
            ->setMaxAllowedIncomingFrameSize(
                static_cast<quint64>(
                    NearbyFrameCodec::
                        headerBytes
                    + NearbyFrameCodec::
                          maximumRequestBytes));
        socket
            ->setMaxAllowedIncomingMessageSize(
                static_cast<quint64>(
                    NearbyFrameCodec::
                        headerBytes
                    + NearbyFrameCodec::
                          maximumRequestBytes));
        auto state =
            std::make_shared<
                ClientState>(
                socket,
                options.transferRootPath);
        clients.insert(
            socket,
            state);
        QObject::connect(
            socket,
            &QWebSocket::
                textMessageReceived,
            &server,
            [this, socket](
                const QString& text) {
                handleInvitation(
                    socket,
                    text);
            });
        QObject::connect(
            socket,
            &QWebSocket::
                binaryMessageReceived,
            &server,
            [this, socket](
                const QByteArray& bytes) {
                handleBinary(
                    socket,
                    bytes);
            });
        QObject::connect(
            socket,
            &QWebSocket::disconnected,
            &server,
            [this, socket]() {
                removeSocket(socket);
            });
    }

    void acceptConnections()
    {
        while (server
                   .hasPendingConnections()) {
            QWebSocket* socket =
                server
                    .nextPendingConnection();
            if (socket == nullptr) {
                continue;
            }
            trackSocket(socket);
            const QUrl request =
                socket->requestUrl();
            if (request.path()
                    != requestPath
                           .toString()
                || request.hasQuery()) {
                socket->close(
                    static_cast<
                        QWebSocketProtocol::
                            CloseCode>(4404),
                    QStringLiteral(
                        "wrong_path"));
            }
        }
    }

    void closeClients()
    {
        const QList<QWebSocket*>
            sockets = clients.keys();
        for (QWebSocket* socket :
             sockets) {
            if (socket == nullptr) {
                continue;
            }
            QObject::disconnect(
                socket,
                nullptr,
                &server,
                nullptr);
            socket->close(
                QWebSocketProtocol::
                    CloseCodeGoingAway,
                QStringLiteral(
                    "server_stopped"));
            socket->deleteLater();
        }
        devices.clear();
        clients.clear();
    }

    NearbyServiceAdvertisement
    advertisement() const
    {
        const auto certificate =
            options.sslConfiguration
                .localCertificate();
        const QString fingerprint =
            QString::fromLatin1(
                QCryptographicHash::hash(
                    certificate.toDer(),
                    QCryptographicHash::
                        Sha256)
                    .toHex());
        return {
            serviceType.toString(),
            NearbyWebSocketServer::
                serviceInstanceName(
                    options.computerName),
            server.serverPort(),
            {
                {
                    QStringLiteral(
                        "frame"),
                    QStringLiteral("1"),
                },
                {
                    QStringLiteral("id"),
                    options.installationId,
                },
                {
                    QStringLiteral(
                        "path"),
                    requestPath.toString(),
                },
                {
                    QStringLiteral("pv"),
                    QStringLiteral("1"),
                },
                {
                    QStringLiteral(
                        "tlsfp"),
                    fingerprint,
                },
                {
                    QStringLiteral(
                        "transport"),
                    QStringLiteral("wss"),
                },
            },
        };
    }

    void withdraw()
    {
        if (!advertised) {
            return;
        }
        if (options
                .withdrawAdvertisement) {
            options
                .withdrawAdvertisement();
        }
        advertised = false;
    }

    Result<void> refreshAdvertisement()
    {
        if (!server.isListening()
            || !mayAdvertise(
                profile,
                allowPublicNetwork)) {
            withdraw();
            return Result<void>::success();
        }

        if (advertised) {
            withdraw();
        }
        if (!options.publishAdvertisement) {
            return Result<void>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.advertiser_unavailable"),
                    QStringLiteral(
                        "Windows nearby discovery is unavailable."),
                    true));
        }
        const auto descriptor =
            advertisement();
        if (!txtFitsDnsSd(
                descriptor.txt)) {
            return Result<void>::failure(
                nearbyServerError(
                    QStringLiteral(
                        "nearby.invalid_advertisement"),
                    QStringLiteral(
                        "Windows nearby discovery metadata is invalid.")));
        }
        const auto published =
            options.publishAdvertisement(
                descriptor);
        if (!published.hasValue()) {
            return published;
        }
        advertised = true;
        return Result<void>::success();
    }

    PairingCoordinator*
        pairingCoordinator = nullptr;
    NearbyRequestHandler requestHandler;
    NearbyWebSocketServerOptions options;
    QWebSocketServer server;
    QHash<
        QWebSocket*,
        std::shared_ptr<ClientState>>
        clients;
    QHash<
        QString,
        QPointer<QWebSocket>>
        devices;
    NearbyNetworkProfile profile =
        NearbyNetworkProfile::
            Unavailable;
    bool allowPublicNetwork = false;
    bool advertised = false;
};

NearbyWebSocketServer::
NearbyWebSocketServer(
    PairingCoordinator& pairingCoordinator,
    NearbyRequestHandler requestHandler,
    NearbyWebSocketServerOptions options,
    QObject* parent)
    : QObject(parent),
      implementation_(
          std::make_unique<
              Implementation>(
              pairingCoordinator,
              std::move(requestHandler),
              std::move(options)))
{
}

NearbyWebSocketServer::
~NearbyWebSocketServer()
{
    if (implementation_ != nullptr) {
        implementation_->withdraw();
        implementation_->closeClients();
        implementation_->server.close();
    }
}

Result<void>
NearbyWebSocketServer::start()
{
    auto& state = *implementation_;
    if (state.server.isListening()) {
        return Result<void>::success();
    }
    const auto certificate =
        state.options
            .sslConfiguration
            .localCertificate();
    if (certificate.isNull()
        || state.options
               .sslConfiguration
               .privateKey()
               .isNull()) {
        return Result<void>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_tls_identity"),
                QStringLiteral(
                    "Windows nearby TLS identity is unavailable.")));
    }
    if (QUuid(
            state.options
                .installationId)
            .isNull()) {
        return Result<void>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.invalid_installation_id"),
                QStringLiteral(
                    "Windows nearby installation identity is invalid.")));
    }
    state.options.installationId =
        state.options
            .installationId
            .trimmed();
    state.server.setSslConfiguration(
        state.options
            .sslConfiguration);
    if (!state.server.listen(
            state.options.listenAddress,
            state.options.listenPort)) {
        return Result<void>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.listen_failed"),
                QStringLiteral(
                    "Windows nearby connections could not start."),
                true));
    }
    const auto advertised =
        state.refreshAdvertisement();
    if (!advertised.hasValue()) {
        state.server.close();
        state.withdraw();
        return advertised;
    }
    return Result<void>::success();
}

QFuture<void>
NearbyWebSocketServer::stop()
{
    auto& state = *implementation_;
    state.withdraw();
    state.closeClients();
    state.server.close();
    return readyVoidFuture();
}

bool
NearbyWebSocketServer::
hasAuthenticatedDevice(
    const QString& deviceId) const
{
    QWebSocket* const socket =
        implementation_
            ->devices.value(
                deviceId);
    const auto state =
        implementation_
            ->clientState(socket);
    return state != nullptr
        && state->authorization
            == Implementation::
                   ClientState::
                       Authorization::
                           Authenticated
        && state->deviceId
            == deviceId
        && state->secret.size()
            == 32
        && socket != nullptr
        && socket->state()
            == QAbstractSocket::
                   ConnectedState;
}

QFuture<Result<void>>
NearbyWebSocketServer::send(
    const QString& deviceId,
    const BridgeResponse& response)
{
    QWebSocket* const socket =
        implementation_
            ->devices.value(
                deviceId);
    const auto state =
        implementation_
            ->clientState(socket);
    if (state != nullptr
        && state->deviceId
            == deviceId) {
        return readyResultFuture(
            implementation_
                ->sendResponse(
                    state,
                    response));
    }
    return readyResultFuture(
        Result<void>::failure(
            nearbyServerError(
                QStringLiteral(
                    "nearby.not_authenticated"),
                QStringLiteral(
                    "The nearby device is not connected."),
                true)));
}

Result<void>
NearbyWebSocketServer::
setNetworkProfile(
    NearbyNetworkProfile profile)
{
    implementation_->profile = profile;
    return implementation_
        ->refreshAdvertisement();
}

Result<void>
NearbyWebSocketServer::
setAllowPublicNetwork(bool allowed)
{
    implementation_->allowPublicNetwork =
        allowed;
    return implementation_
        ->refreshAdvertisement();
}

void NearbyWebSocketServer::
setRelayUrlString(
    std::optional<QString> value)
{
    implementation_->options
        .relayUrlString =
        std::move(value);
}

NearbyNetworkProfile
NearbyWebSocketServer::
networkProfile() const noexcept
{
    return implementation_->profile;
}

bool
NearbyWebSocketServer::
isListening() const noexcept
{
    return implementation_
        ->server.isListening();
}

quint16
NearbyWebSocketServer::
serverPort() const noexcept
{
    return implementation_
        ->server.serverPort();
}

QString
NearbyWebSocketServer::
serviceInstanceName(
    QString computerName)
{
    computerName =
        computerName.trimmed();
    if (computerName.isEmpty()) {
        computerName =
            QStringLiteral(
                "Codex Companion Windows");
    }
    QString result =
        trimUtf8(
            computerName,
            63);
    if (result.isEmpty()) {
        result =
            QStringLiteral(
                "Codex Companion Windows");
    }
    return result;
}

} // namespace companion
