#pragma once

#include "codex/models/BridgeModels.h"
#include "core/CompanionError.h"
#include "core/Result.h"
#include "mobile/security/SecurityModels.h"

#include <QFuture>
#include <QObject>
#include <QString>

#include <functional>

namespace companion {

class PairingCoordinator;
class RelayStateStore;

using MobileRelayRequestHandler =
    std::function<QFuture<BridgeResponse>(
        QString deviceId,
        BridgeRequest request)>;
using MobileRelaySender =
    std::function<QFuture<Result<void>>(
        QString deviceId,
        EncryptedEnvelope envelope)>;
using MobileRelayClock =
    std::function<qint64()>;

class CompanionMobileRelayRouter final
    : public QObject {
    Q_OBJECT

public:
    CompanionMobileRelayRouter(
        QString hostDeviceId,
        PairingCoordinator& pairingCoordinator,
        RelayStateStore& relayStateStore,
        MobileRelayRequestHandler requestHandler,
        MobileRelaySender sender,
        MobileRelayClock clock = {},
        QObject* parent = nullptr);

    void receive(
        QString deviceId,
        EncryptedEnvelope envelope);

signals:
    void failureOccurred(
        QString deviceId,
        companion::CompanionError error);

private:
    void dispatch(
        QString deviceId,
        QString channelId,
        BridgeRequest request);
    void sendResponse(
        QString deviceId,
        QString expectedChannelId,
        BridgeResponse response);
    void observeSend(
        QString deviceId,
        QFuture<Result<void>> future);
    void reportFailure(
        QString deviceId,
        CompanionError error);

    QString hostDeviceId_;
    PairingCoordinator*
        pairingCoordinator_ = nullptr;
    RelayStateStore*
        relayStateStore_ = nullptr;
    MobileRelayRequestHandler
        requestHandler_;
    MobileRelaySender sender_;
    MobileRelayClock clock_;
};

} // namespace companion
