#pragma once

#include "core/Result.h"
#include "mobile/MobileTypes.h"
#include "mobile/security/SecurityModels.h"

#include <QByteArray>
#include <QString>
#include <QStringView>

#include <optional>

namespace companion {

enum class RelayWireType {
    Register,
    Registered,
    PeerPresence,
    Packet,
    PacketResult,
    Ping,
    Pong,
    Error,
};

enum class RelayPacketResultStatus {
    Accepted,
    Undeliverable,
};

struct RelayWireMessage final {
    RelayWireType type = RelayWireType::Ping;
    int protocolVersion = 1;
    std::optional<QString> packetId;
    std::optional<QString> channelId;
    std::optional<QString> endpointId;
    std::optional<QString> senderId;
    std::optional<QByteArray> envelope;
    std::optional<int> peerCount;
    std::optional<RelayPacketResultStatus> status;
    std::optional<QString> code;
    std::optional<QString> message;

    friend bool operator==(
        const RelayWireMessage&,
        const RelayWireMessage&) = default;
};

class RelayModels final {
public:
    static constexpr int protocolVersion = 1;

    static QString wireName(RelayWireType type);
    static QString wireName(
        RelayPacketResultStatus status);
    static std::optional<RelayWireType>
    wireType(QStringView value);
    static std::optional<RelayPacketResultStatus>
    packetResultStatus(QStringView value);

    static bool isValidOpaqueId(QStringView value);

    static Result<RelayWireMessage> registration(
        QString channelId,
        QString endpointId);
    static Result<RelayWireMessage> packet(
        const EncryptedEnvelope& envelope,
        QString packetId);
    static RelayWireMessage ping();
    static RelayWireMessage pong();
    static Result<EncryptedEnvelope> decodedEnvelope(
        const RelayWireMessage& message);

    static MobileTransportRoute preferredRoute(
        bool nearbyConnected,
        bool relayRegistered,
        bool relayHandshakeVerified);
};

} // namespace companion
