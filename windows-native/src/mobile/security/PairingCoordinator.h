#pragma once

#include "core/Result.h"
#include "mobile/security/BridgeSecurity.h"
#include "mobile/security/PairingRecordStore.h"

#include <QObject>

#include <chrono>
#include <functional>
#include <optional>

namespace companion {

using PairingClock =
    std::function<QDateTime()>;
using PairingCodeGenerator =
    std::function<Result<QString>()>;
using PairingSecretGenerator =
    std::function<Result<QByteArray>()>;

class PairingCoordinator final
    : public QObject {
    Q_OBJECT

public:
    explicit PairingCoordinator(
        PairingRecordStore& store,
        PairingClock clock = {},
        PairingCodeGenerator codeGenerator =
            {},
        PairingSecretGenerator secretGenerator =
            {},
        QObject* parent = nullptr);

    Result<ActivePairing> beginPairing(
        std::chrono::seconds validFor =
            std::chrono::seconds(300));
    void cancelPairing();
    std::optional<ActivePairing>
    activePairing();

    QVector<PairingRecord>
    trustedRecords() const;
    std::optional<PairingRecord>
    trustedRecord(
        const QString& deviceId) const;

    Result<void> remember(
        const PairingRecord& record);
    InvitationDecision invitationDecision(
        const BridgeInvitation& invitation);
    Result<PairingRecord> completePairing(
        const BridgeInvitation& invitation);
    Result<void> forget(
        const QString& deviceId);

signals:
    void pairingStateChanged();

private:
    void releaseCompletionClaim(
        quint64 generation);

    PairingRecordStore* store_ = nullptr;
    PairingClock clock_;
    PairingCodeGenerator codeGenerator_;
    PairingSecretGenerator secretGenerator_;
    QMutex mutex_;
    std::optional<ActivePairing> pairing_;
    quint64 pairingGeneration_ = 0;
    std::optional<quint64>
        completingGeneration_;
};

} // namespace companion
