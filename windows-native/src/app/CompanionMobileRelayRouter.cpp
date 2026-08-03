#include "app/CompanionMobileRelayRouter.h"

#include "codex/models/BridgeJsonCodec.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/RelayStateStore.h"
#include "platform/windows/security/WindowsCrypto.h"

#include <QDateTime>
#include <QFutureWatcher>

#include <optional>
#include <utility>

namespace companion {
namespace {

CompanionError relayRouterError(
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

CompanionError dispatchFailure()
{
    return relayRouterError(
        QStringLiteral(
            "relay.dispatch_failed"),
        QStringLiteral(
            "The mobile relay request could not be completed."),
        true);
}

CompanionError sendFailure()
{
    return relayRouterError(
        QStringLiteral(
            "relay.response_send_failed"),
        QStringLiteral(
            "The mobile relay response could not be sent."),
        true);
}

} // namespace

CompanionMobileRelayRouter::
CompanionMobileRelayRouter(
    QString hostDeviceId,
    PairingCoordinator& pairingCoordinator,
    RelayStateStore& relayStateStore,
    MobileRelayRequestHandler requestHandler,
    MobileRelaySender sender,
    MobileRelayClock clock,
    QObject* parent)
    : QObject(parent),
      hostDeviceId_(
          std::move(hostDeviceId)),
      pairingCoordinator_(
          &pairingCoordinator),
      relayStateStore_(
          &relayStateStore),
      requestHandler_(
          std::move(requestHandler)),
      sender_(std::move(sender)),
      clock_(
          clock
              ? std::move(clock)
              : MobileRelayClock([] {
                    return QDateTime::
                        currentMSecsSinceEpoch();
                }))
{
}

void CompanionMobileRelayRouter::receive(
    QString deviceId,
    EncryptedEnvelope envelope)
{
    if (deviceId.trimmed().isEmpty()
        || envelope.senderId
            != deviceId
        || pairingCoordinator_
            == nullptr
        || relayStateStore_
            == nullptr
        || !requestHandler_
        || !sender_) {
        return;
    }

    std::optional<PairingRecord> record =
        pairingCoordinator_
            ->trustedRecord(deviceId);
    if (!record.has_value()) {
        return;
    }

    auto plaintext =
        BridgeSecurity::open(
            envelope,
            record->secret);
    if (!plaintext.hasValue()) {
        return;
    }
    const auto request =
        BridgeJsonCodec::decodeRequest(
            plaintext.value(),
            BridgeWireProfile::
                RelayV1Canonical);
    WindowsCrypto::secureZero(
        plaintext.value());
    if (!request.hasValue()) {
        return;
    }

    const auto accepted =
        relayStateStore_
            ->acceptInbound(
                envelope.channelId,
                deviceId,
                envelope.sequence);
    if (!accepted.hasValue()) {
        reportFailure(
            std::move(deviceId),
            accepted.error());
        return;
    }
    if (!accepted.value()) {
        return;
    }

    dispatch(
        std::move(deviceId),
        std::move(
            envelope.channelId),
        request.value());
}

void CompanionMobileRelayRouter::dispatch(
    QString deviceId,
    QString channelId,
    BridgeRequest request)
{
    QFuture<BridgeResponse> future;
    try {
        future = requestHandler_(
            deviceId,
            std::move(request));
    } catch (...) {
        reportFailure(
            std::move(deviceId),
            dispatchFailure());
        return;
    }
    if (!future.isValid()) {
        reportFailure(
            std::move(deviceId),
            dispatchFailure());
        return;
    }

    auto* watcher =
        new QFutureWatcher<
            BridgeResponse>(this);
    connect(
        watcher,
        &QFutureWatcher<
            BridgeResponse>::finished,
        this,
        [this,
         watcher,
         deviceId =
             std::move(deviceId),
         channelId =
             std::move(channelId)]() mutable {
            if (watcher->isCanceled()
                || watcher->future()
                       .resultCount()
                    != 1) {
                watcher->deleteLater();
                reportFailure(
                    std::move(deviceId),
                    dispatchFailure());
                return;
            }
            BridgeResponse response =
                watcher->result();
            watcher->deleteLater();
            sendResponse(
                std::move(deviceId),
                std::move(channelId),
                std::move(response));
        });
    watcher->setFuture(
        std::move(future));
}

void CompanionMobileRelayRouter::
sendResponse(
    QString deviceId,
    QString expectedChannelId,
    BridgeResponse response)
{
    std::optional<PairingRecord> record =
        pairingCoordinator_
            ->trustedRecord(deviceId);
    if (!record.has_value()) {
        return;
    }
    const auto channel =
        BridgeSecurity::channelId(
            record->secret);
    if (!channel.hasValue()) {
        reportFailure(
            std::move(deviceId),
            channel.error());
        return;
    }
    if (channel.value()
        != expectedChannelId) {
        return;
    }

    auto encoded =
        BridgeJsonCodec::encodeResponse(
            response,
            BridgeWireProfile::
                RelayV1Canonical);
    if (!encoded.hasValue()) {
        reportFailure(
            std::move(deviceId),
            encoded.error());
        return;
    }
    const auto sequence =
        relayStateStore_->nextOutbound(
            channel.value(),
            hostDeviceId_);
    if (!sequence.hasValue()) {
        WindowsCrypto::secureZero(
            encoded.value());
        reportFailure(
            std::move(deviceId),
            sequence.error());
        return;
    }
    auto envelope =
        BridgeSecurity::seal(
            encoded.value(),
            record->secret,
            hostDeviceId_,
            sequence.value(),
            clock_());
    WindowsCrypto::secureZero(
        encoded.value());
    if (!envelope.hasValue()) {
        reportFailure(
            std::move(deviceId),
            envelope.error());
        return;
    }

    QFuture<Result<void>> sent;
    try {
        sent = sender_(
            deviceId,
            std::move(
                envelope.value()));
    } catch (...) {
        reportFailure(
            std::move(deviceId),
            sendFailure());
        return;
    }
    if (!sent.isValid()) {
        reportFailure(
            std::move(deviceId),
            sendFailure());
        return;
    }
    observeSend(
        std::move(deviceId),
        std::move(sent));
}

void CompanionMobileRelayRouter::observeSend(
    QString deviceId,
    QFuture<Result<void>> future)
{
    auto* watcher =
        new QFutureWatcher<
            Result<void>>(this);
    connect(
        watcher,
        &QFutureWatcher<
            Result<void>>::finished,
        this,
        [this,
         watcher,
         deviceId =
             std::move(deviceId)]() mutable {
            if (watcher->isCanceled()
                || watcher->future()
                       .resultCount()
                    != 1) {
                watcher->deleteLater();
                reportFailure(
                    std::move(deviceId),
                    sendFailure());
                return;
            }
            const Result<void> result =
                watcher->result();
            watcher->deleteLater();
            if (!result.hasValue()) {
                reportFailure(
                    std::move(deviceId),
                    result.error());
            }
        });
    watcher->setFuture(
        std::move(future));
}

void CompanionMobileRelayRouter::
reportFailure(
    QString deviceId,
    CompanionError error)
{
    emit failureOccurred(
        std::move(deviceId),
        std::move(error));
}

} // namespace companion
