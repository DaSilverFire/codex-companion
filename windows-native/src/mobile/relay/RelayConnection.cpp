#include "mobile/relay/RelayConnection.h"

#include "mobile/relay/RelaySettings.h"
#include "mobile/relay/RelayWireCodec.h"

#include <QAbstractSocket>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>
#include <QtWebSockets/QWebSocket>

#include <algorithm>
#include <utility>

namespace companion {
namespace {

CompanionError relayError(
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

QFuture<Result<void>> readyFuture(
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

CompanionError notRegisteredError()
{
    return relayError(
        QStringLiteral(
            "relay.not_registered"),
        QStringLiteral(
            "The encrypted relay is not connected."),
        true);
}

CompanionError transportStoppedError()
{
    return relayError(
        QStringLiteral(
            "relay.transport_stopped"),
        QStringLiteral(
            "The encrypted relay connection stopped."),
        true);
}

CompanionError transportFailedError()
{
    return relayError(
        QStringLiteral(
            "relay.transport_failed"),
        QStringLiteral(
            "The encrypted relay connection failed."),
        true);
}

CompanionError invalidWireError()
{
    return relayError(
        QStringLiteral(
            "relay.invalid_wire_message"),
        QStringLiteral(
            "The relay returned an invalid message."));
}

} // namespace

RelayConnection::RelayConnection(
    QUrl url,
    QString channelId,
    QString endpointId,
    QString remoteSenderId,
    RelayConnectionTiming timing,
    QObject* parent)
    : RelayConnection(
          std::move(url),
          std::move(channelId),
          std::move(endpointId),
          std::move(remoteSenderId),
          RelaySenderMode::PairedExact,
          std::move(timing),
          parent)
{
}

RelayConnection::RelayConnection(
    QUrl url,
    QString channelId,
    QString endpointId,
    RelaySenderMode senderMode,
    RelayConnectionTiming timing,
    QObject* parent)
    : RelayConnection(
          std::move(url),
          std::move(channelId),
          std::move(endpointId),
          {},
          senderMode,
          std::move(timing),
          parent)
{
}

RelayConnection::RelayConnection(
    QUrl url,
    QString channelId,
    QString endpointId,
    QString remoteSenderId,
    RelaySenderMode senderMode,
    RelayConnectionTiming timing,
    QObject* parent)
    : QObject(parent),
      channelId_(
          std::move(channelId)),
      endpointId_(
          std::move(endpointId)),
      remoteSenderId_(
          std::move(remoteSenderId)),
      senderMode_(senderMode),
      timing_(
          std::move(timing))
{
    timing_.packetResultTimeoutMilliseconds =
        std::max(
            1,
            timing_
                .packetResultTimeoutMilliseconds);
    timing_.pingIntervalMilliseconds =
        std::max(
            1,
            timing_
                .pingIntervalMilliseconds);
    if (timing_
            .reconnectDelayMilliseconds
            .isEmpty()) {
        timing_
            .reconnectDelayMilliseconds = {
                1'000,
                2'000,
                4'000,
                8'000,
                16'000,
                30'000,
            };
    }
    for (int& delay :
         timing_
             .reconnectDelayMilliseconds) {
        delay = std::max(1, delay);
    }
    if (!timing_.packetIdGenerator) {
        timing_.packetIdGenerator = [] {
            return QUuid::createUuid()
                .toString(
                    QUuid::WithoutBraces);
        };
    }

    const bool senderConfigurationValid =
        senderMode_
                == RelaySenderMode::
                       BootstrapUnknown
            ? remoteSenderId_.isEmpty()
            : RelayModels::isValidOpaqueId(
                  remoteSenderId_);
    if (!RelayModels::isValidOpaqueId(
            channelId_)
        || !RelayModels::isValidOpaqueId(
            endpointId_)
        || !senderConfigurationValid) {
        configurationError_ =
            invalidWireError();
    } else {
        const auto configured =
            RelaySettings::withChannel(
                std::move(url),
                channelId_);
        if (!configured.hasValue()) {
            configurationError_ =
                configured.error();
        } else {
            effectiveUrl_ =
                configured.value();
        }
    }

    reconnectTimer_ =
        new QTimer(this);
    reconnectTimer_->setSingleShot(true);
    QObject::connect(
        reconnectTimer_,
        &QTimer::timeout,
        this,
        [this]() {
            if (shouldRun_) {
                connectNow();
            }
        });

    pingTimer_ = new QTimer(this);
    pingTimer_->setInterval(
        timing_
            .pingIntervalMilliseconds);
    QObject::connect(
        pingTimer_,
        &QTimer::timeout,
        this,
        [this]() {
            if (!shouldRun_
                || !registrationAcknowledged_
                || socket_ == nullptr
                || socket_->state()
                    != QAbstractSocket::
                        ConnectedState) {
                return;
            }
            socket_->ping();
            emit transportPingSent();
        });
}

RelayConnection::~RelayConnection()
{
    shouldRun_ = false;
    reconnectTimer_->stop();
    pingTimer_->stop();
    failPending(
        transportStoppedError());
    destroySocket();
}

void RelayConnection::start()
{
    if (shouldRun_) {
        return;
    }
    if (configurationError_.has_value()) {
        emit failureOccurred(
            *configurationError_);
        return;
    }
    shouldRun_ = true;
    reconnectAttempt_ = 0;
    connectNow();
}

QFuture<Result<void>>
RelayConnection::stop()
{
    shouldRun_ = false;
    advanceGeneration();
    reconnectTimer_->stop();
    pingTimer_->stop();
    registrationAcknowledged_ =
        false;
    failPending(
        transportStoppedError());
    destroySocket();
    publish(
        RelayConnectionState::Stopped);
    return readyFuture(
        Result<void>::success());
}

QFuture<Result<void>>
RelayConnection::send(
    const EncryptedEnvelope& envelope)
{
    if (!registrationAcknowledged_
        || socket_ == nullptr
        || socket_->state()
            != QAbstractSocket::
                ConnectedState) {
        return readyFuture(
            Result<void>::failure(
                notRegisteredError()));
    }
    if (envelope.channelId
            != channelId_
        || envelope.senderId
            != endpointId_) {
        return readyFuture(
            Result<void>::failure(
                relayError(
                    QStringLiteral(
                        "relay.metadata_mismatch"),
                    QStringLiteral(
                        "Outgoing relay metadata does not match this endpoint."))));
    }

    const QString packetId =
        nextPacketId();
    if (!RelayModels::isValidOpaqueId(
            packetId)
        || pendingSends_.contains(
            packetId)) {
        return readyFuture(
            Result<void>::failure(
                relayError(
                    QStringLiteral(
                        "relay.invalid_packet_id"),
                    QStringLiteral(
                        "The relay packet identifier is invalid or already pending."))));
    }
    const auto packet =
        RelayModels::packet(
            envelope,
            packetId);
    if (!packet.hasValue()) {
        return readyFuture(
            Result<void>::failure(
                packet.error()));
    }
    const auto encoded =
        RelayWireCodec::encode(
            packet.value());
    if (!encoded.hasValue()) {
        return readyFuture(
            Result<void>::failure(
                encoded.error()));
    }
    const auto sizeResult =
        validateFinalMessageSize(
            encoded.value());
    if (!sizeResult.hasValue()) {
        return readyFuture(
            Result<void>::failure(
                sizeResult.error()));
    }

    auto promise =
        std::make_shared<
            QPromise<Result<void>>>();
    promise->start();
    QFuture<Result<void>> future =
        promise->future();

    QTimer* timeout =
        new QTimer(this);
    timeout->setSingleShot(true);
    timeout->setInterval(
        timing_
            .packetResultTimeoutMilliseconds);
    QObject::connect(
        timeout,
        &QTimer::timeout,
        this,
        [this, packetId]() {
            resolvePending(
                packetId,
                Result<void>::failure(
                    relayError(
                        QStringLiteral(
                            "relay.packet_result_timeout"),
                        QStringLiteral(
                            "The relay did not confirm packet delivery in time."),
                        true)));
        });
    pendingSends_.insert(
        packetId,
        {promise, timeout});

    const qint64 queued =
        socket_->sendTextMessage(
            QString::fromUtf8(
                encoded.value()));
    if (queued
        != encoded.value().size()) {
        resolvePending(
            packetId,
            Result<void>::failure(
                transportFailedError()));
        return future;
    }
    timeout->start();
    return future;
}

RelayConnectionState
RelayConnection::state() const noexcept
{
    return state_;
}

const QUrl&
RelayConnection::effectiveUrl()
    const noexcept
{
    return effectiveUrl_;
}

bool RelayConnection::
registrationAcknowledged()
    const noexcept
{
    return registrationAcknowledged_;
}

Result<void>
RelayConnection::validateFinalMessageSize(
    QByteArrayView bytes)
{
    if (bytes.size()
        > kMaximumMessageBytes) {
        return Result<void>::failure(
            relayError(
                QStringLiteral(
                    "relay.payload_too_large"),
                QStringLiteral(
                    "This response is too large for remote relay. Reconnect nearby and retry.")));
    }
    return Result<void>::success();
}

void RelayConnection::connectNow()
{
    if (!shouldRun_) {
        return;
    }
    reconnectTimer_->stop();
    pingTimer_->stop();
    registrationAcknowledged_ =
        false;
    destroySocket();
    const quint64 generation =
        advanceGeneration();
    publish(
        RelayConnectionState::Connecting);

    socket_ = new QWebSocket(
        QString(),
        QWebSocketProtocol::
            VersionLatest,
        this);
    socket_->
        setMaxAllowedIncomingMessageSize(
            kMaximumMessageBytes);
    socket_->
        setMaxAllowedIncomingFrameSize(
            kMaximumMessageBytes);

    QObject::connect(
        socket_,
        &QWebSocket::connected,
        this,
        [this, generation]() {
            if (!isCurrentGeneration(
                    generation)) {
                return;
            }
            const auto registration =
                RelayModels::registration(
                    channelId_,
                    endpointId_);
            if (!registration.hasValue()) {
                handleFailure(
                    registration.error(),
                    generation);
                return;
            }
            const auto sent =
                sendWire(
                    registration.value());
            if (!sent.hasValue()) {
                handleFailure(
                    sent.error(),
                    generation);
            }
        });
    QObject::connect(
        socket_,
        &QWebSocket::
            textMessageReceived,
        this,
        [this, generation](
            const QString& message) {
            handlePayload(
                message.toUtf8(),
                generation);
        });
    QObject::connect(
        socket_,
        &QWebSocket::
            binaryMessageReceived,
        this,
        [this, generation](
            const QByteArray& message) {
            handlePayload(
                message,
                generation);
        });
    QObject::connect(
        socket_,
        &QWebSocket::
            errorOccurred,
        this,
        [this, generation](
            QAbstractSocket::
                SocketError) {
            handleFailure(
                transportFailedError(),
                generation);
        });
    QObject::connect(
        socket_,
        &QWebSocket::disconnected,
        this,
        [this, generation]() {
            if (isCurrentGeneration(
                    generation)
                && shouldRun_) {
                handleFailure(
                    transportFailedError(),
                    generation);
            }
        });

    socket_->open(
        QNetworkRequest(
            effectiveUrl_));
}

void RelayConnection::destroySocket()
{
    if (socket_ == nullptr) {
        return;
    }
    QObject::disconnect(
        socket_,
        nullptr,
        this,
        nullptr);
    socket_->abort();
    socket_->deleteLater();
    socket_ = nullptr;
}

Result<void> RelayConnection::sendWire(
    const RelayWireMessage& message)
{
    if (socket_ == nullptr
        || socket_->state()
            != QAbstractSocket::
                ConnectedState) {
        return Result<void>::failure(
            notRegisteredError());
    }
    const auto encoded =
        RelayWireCodec::encode(message);
    if (!encoded.hasValue()) {
        return Result<void>::failure(
            encoded.error());
    }
    const auto size =
        validateFinalMessageSize(
            encoded.value());
    if (!size.hasValue()) {
        return size;
    }
    if (socket_->sendTextMessage(
            QString::fromUtf8(
                encoded.value()))
        != encoded.value().size()) {
        return Result<void>::failure(
            transportFailedError());
    }
    return Result<void>::success();
}

void RelayConnection::handlePayload(
    QByteArray bytes,
    quint64 generation)
{
    if (!isCurrentGeneration(generation)
        || bytes.size()
            > kMaximumMessageBytes) {
        if (isCurrentGeneration(
                generation)) {
            handleFailure(
                invalidWireError(),
                generation);
        }
        return;
    }
    const auto decoded =
        RelayWireCodec::decode(bytes);
    if (!decoded.hasValue()) {
        handleFailure(
            decoded.error(),
            generation);
        return;
    }
    const auto handled =
        handleWire(
            decoded.value());
    if (!handled.hasValue()) {
        handleFailure(
            handled.error(),
            generation);
    }
}

Result<void> RelayConnection::handleWire(
    const RelayWireMessage& message)
{
    switch (message.type) {
    case RelayWireType::Registered:
        reconnectAttempt_ = 0;
        registrationAcknowledged_ =
            true;
        pingTimer_->start();
        return Result<void>::success();
    case RelayWireType::PeerPresence:
        if (!registrationAcknowledged_
            || !message.peerCount
                    .has_value()) {
            return Result<void>::failure(
                invalidWireError());
        }
        publish(
            *message.peerCount > 0
                ? RelayConnectionState::
                      Registered
                : RelayConnectionState::
                      Connecting);
        return Result<void>::success();
    case RelayWireType::Packet: {
        if (state_
            != RelayConnectionState::
                Registered) {
            return Result<void>::failure(
                invalidWireError());
        }
        const auto envelope =
            RelayModels::decodedEnvelope(
                message);
        if (!envelope.hasValue()) {
            return Result<void>::failure(
                envelope.error());
        }
        const bool senderMatches =
            senderMode_
                    == RelaySenderMode::
                           BootstrapUnknown
                ? envelope.value().senderId
                          != endpointId_
                    && RelayModels::
                        isValidOpaqueId(
                            envelope.value()
                                .senderId)
                : envelope.value().senderId
                      == remoteSenderId_;
        if (envelope.value().channelId
                != channelId_
            || !senderMatches) {
            return Result<void>::failure(
                relayError(
                    QStringLiteral(
                        "relay.metadata_mismatch"),
                    QStringLiteral(
                        "Incoming relay metadata does not match the paired endpoint.")));
        }
        emit envelopeReceived(
            envelope.value());
        return Result<void>::success();
    }
    case RelayWireType::PacketResult:
        if (!registrationAcknowledged_
            || !message.packetId
                    .has_value()
            || !message.status
                    .has_value()) {
            return Result<void>::failure(
                invalidWireError());
        }
        if (*message.status
            == RelayPacketResultStatus::
                Accepted) {
            resolvePending(
                *message.packetId,
                Result<void>::success());
            return Result<void>::success();
        }
        if (resolvePending(
                *message.packetId,
                Result<void>::failure(
                    relayError(
                        QStringLiteral(
                            "relay.peer_unavailable"),
                        QStringLiteral(
                            "The paired relay endpoint is not connected."),
                        true)))) {
            publish(
                RelayConnectionState::
                    Connecting);
        }
        return Result<void>::success();
    case RelayWireType::Ping:
        return sendWire(
            RelayModels::pong());
    case RelayWireType::Pong:
        return Result<void>::success();
    case RelayWireType::Error:
        return Result<void>::failure(
            relayError(
                QStringLiteral(
                    "relay.rejected"),
                message.message
                    .value_or(
                        message.code
                            .value_or(
                                QStringLiteral(
                                    "The relay rejected the connection.")))));
    case RelayWireType::Register:
        return Result<void>::failure(
            invalidWireError());
    }
    return Result<void>::failure(
        invalidWireError());
}

void RelayConnection::handleFailure(
    CompanionError error,
    quint64 generation)
{
    if (!isCurrentGeneration(
            generation)
        || !shouldRun_) {
        return;
    }
    registrationAcknowledged_ =
        false;
    pingTimer_->stop();
    failPending(error);
    emit failureOccurred(error);
    destroySocket();
    scheduleReconnect();
}

void RelayConnection::scheduleReconnect()
{
    if (!shouldRun_) {
        return;
    }
    publish(
        RelayConnectionState::
            WaitingToReconnect);
    const int delay =
        currentReconnectDelay();
    ++reconnectAttempt_;
    emit reconnectScheduled(delay);
    reconnectTimer_->start(delay);
}

int RelayConnection::
currentReconnectDelay() const
{
    const qsizetype index =
        std::min(
            qsizetype(reconnectAttempt_),
            timing_
                    .reconnectDelayMilliseconds
                    .size()
                - 1);
    return timing_
        .reconnectDelayMilliseconds
        .at(index);
}

bool RelayConnection::resolvePending(
    const QString& packetId,
    Result<void> result)
{
    const auto iterator =
        pendingSends_.find(packetId);
    if (iterator
        == pendingSends_.end()) {
        return false;
    }
    PendingSend pending =
        iterator.value();
    pendingSends_.erase(iterator);
    if (pending.timeout != nullptr) {
        pending.timeout->stop();
        pending.timeout->deleteLater();
    }
    pending.promise->addResult(
        std::move(result));
    pending.promise->finish();
    return true;
}

void RelayConnection::failPending(
    const CompanionError& error)
{
    const QStringList packetIds =
        pendingSends_.keys();
    for (const QString& packetId :
         packetIds) {
        resolvePending(
            packetId,
            Result<void>::failure(error));
    }
}

void RelayConnection::publish(
    RelayConnectionState state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit stateChanged(state_);
}

bool RelayConnection::
isCurrentGeneration(
    quint64 generation) const noexcept
{
    return generation == generation_;
}

quint64 RelayConnection::
advanceGeneration() noexcept
{
    ++generation_;
    if (generation_ == 0) {
        ++generation_;
    }
    return generation_;
}

QString RelayConnection::
nextPacketId() const
{
    return timing_
        .packetIdGenerator();
}

} // namespace companion
