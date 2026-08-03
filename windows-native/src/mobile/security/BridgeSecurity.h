#pragma once

#include "core/Result.h"
#include "mobile/security/SecurityModels.h"

#include <QByteArrayView>
#include <QString>

#include <optional>

namespace companion {

class BridgeSecurityTestAccess;

class BridgeSecurity final {
public:
    static constexpr int invitationVersion = 1;
    static constexpr int envelopeVersion = 1;
    static constexpr qint64 invitationClockSkewMilliseconds =
        120000;

    static Result<BridgeInvitation>
    authenticatedInvitation(
        QString deviceId,
        QString displayName,
        QByteArrayView secret,
        QDateTime now,
        QByteArray nonce);

    static InvitationDecision decideInvitation(
        const BridgeInvitation& invitation,
        std::optional<QByteArray> trustedSecret,
        std::optional<ActivePairing> activePairing,
        QDateTime now);

    static Result<QString> channelId(
        QByteArrayView secret);

    static Result<EncryptedEnvelope> seal(
        QByteArrayView plaintext,
        QByteArrayView secret,
        QString senderId,
        quint64 sequence,
        qint64 sentAtMilliseconds);

    static Result<QByteArray> open(
        const EncryptedEnvelope& envelope,
        QByteArrayView secret);

    static std::optional<QString>
    normalizedPairingCode(const QString& code);

    static Result<QString> randomPairingCode();
    static Result<QByteArray> randomSecret();
    static Result<QByteArray>
    randomInvitationNonce();

private:
    friend class BridgeSecurityTestAccess;

    static Result<EncryptedEnvelope>
    sealWithNonce(
        QByteArrayView plaintext,
        QByteArrayView secret,
        QString senderId,
        quint64 sequence,
        qint64 sentAtMilliseconds,
        QByteArray nonce);

    static Result<QByteArray>
    invitationAuthenticationData(
        const BridgeInvitation& invitation);

    static Result<QByteArray>
    envelopeAuthenticationData(
        const EncryptedEnvelope& envelope);
};

} // namespace companion
