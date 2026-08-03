#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QtTypes>

#include <optional>

namespace companion {

struct BridgeInvitation final {
    int version = 1;
    QString deviceId;
    QString displayName;
    qint64 issuedAtMilliseconds = 0;
    QByteArray nonce;
    std::optional<QByteArray> authenticator;
    std::optional<QString> pairingCode;

    friend bool operator==(
        const BridgeInvitation&,
        const BridgeInvitation&) = default;
};

struct ActivePairing final {
    QString code;
    QDateTime expiresAt;

    friend bool operator==(
        const ActivePairing&,
        const ActivePairing&) = default;
};

enum class InvitationDecision {
    AcceptTrusted,
    AcceptPairing,
    RejectVersion,
    RejectExpired,
    RejectAuthentication,
    RejectUnpaired,
};

struct EncryptedEnvelope final {
    int version = 1;
    QString channelId;
    QString senderId;
    quint64 sequence = 0;
    qint64 sentAtMilliseconds = 0;
    QByteArray sealedPayload;

    friend bool operator==(
        const EncryptedEnvelope&,
        const EncryptedEnvelope&) = default;
};

} // namespace companion

Q_DECLARE_METATYPE(
    companion::EncryptedEnvelope)
