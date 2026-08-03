#include "mobile/relay/RelayModels.h"

#include "mobile/relay/RelayWireCodec.h"

namespace companion {
namespace {

CompanionError invalidPacket(QString message)
{
    return {
        QStringLiteral("relay.invalid_packet"),
        std::move(message),
        false,
        {},
    };
}

CompanionError metadataMismatch()
{
    return {
        QStringLiteral("relay.metadata_mismatch"),
        QStringLiteral(
            "Relay packet metadata does not match its encrypted envelope."),
        false,
        {},
    };
}

} // namespace

QString RelayModels::wireName(RelayWireType type)
{
    switch (type) {
    case RelayWireType::Register:
        return QStringLiteral("register");
    case RelayWireType::Registered:
        return QStringLiteral("registered");
    case RelayWireType::PeerPresence:
        return QStringLiteral("peerPresence");
    case RelayWireType::Packet:
        return QStringLiteral("packet");
    case RelayWireType::PacketResult:
        return QStringLiteral("packetResult");
    case RelayWireType::Ping:
        return QStringLiteral("ping");
    case RelayWireType::Pong:
        return QStringLiteral("pong");
    case RelayWireType::Error:
        return QStringLiteral("error");
    }
    return {};
}

QString RelayModels::wireName(
    RelayPacketResultStatus status)
{
    switch (status) {
    case RelayPacketResultStatus::Accepted:
        return QStringLiteral("accepted");
    case RelayPacketResultStatus::Undeliverable:
        return QStringLiteral("undeliverable");
    }
    return {};
}

std::optional<RelayWireType>
RelayModels::wireType(QStringView value)
{
    if (value == u"register") {
        return RelayWireType::Register;
    }
    if (value == u"registered") {
        return RelayWireType::Registered;
    }
    if (value == u"peerPresence") {
        return RelayWireType::PeerPresence;
    }
    if (value == u"packet") {
        return RelayWireType::Packet;
    }
    if (value == u"packetResult") {
        return RelayWireType::PacketResult;
    }
    if (value == u"ping") {
        return RelayWireType::Ping;
    }
    if (value == u"pong") {
        return RelayWireType::Pong;
    }
    if (value == u"error") {
        return RelayWireType::Error;
    }
    return std::nullopt;
}

std::optional<RelayPacketResultStatus>
RelayModels::packetResultStatus(QStringView value)
{
    if (value == u"accepted") {
        return RelayPacketResultStatus::Accepted;
    }
    if (value == u"undeliverable") {
        return RelayPacketResultStatus::Undeliverable;
    }
    return std::nullopt;
}

bool RelayModels::isValidOpaqueId(QStringView value)
{
    const QByteArray bytes =
        value.toString().toUtf8();
    if (bytes.isEmpty() || bytes.size() > 128) {
        return false;
    }
    for (const char byte : bytes) {
        const bool valid =
            (byte >= 'A' && byte <= 'Z')
            || (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9')
            || byte == '_' || byte == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

Result<RelayWireMessage> RelayModels::registration(
    QString channelId,
    QString endpointId)
{
    if (channelId.isEmpty() || endpointId.isEmpty()) {
        return Result<RelayWireMessage>::failure(
            invalidPacket(
                QStringLiteral(
                    "Relay registration identifiers must not be empty.")));
    }
    RelayWireMessage message;
    message.type = RelayWireType::Register;
    message.channelId = std::move(channelId);
    message.endpointId = std::move(endpointId);
    return Result<RelayWireMessage>::success(
        std::move(message));
}

Result<RelayWireMessage> RelayModels::packet(
    const EncryptedEnvelope& envelope,
    QString packetId)
{
    if (!isValidOpaqueId(packetId)
        || envelope.version != 1
        || envelope.channelId.isEmpty()
        || envelope.senderId.isEmpty()) {
        return Result<RelayWireMessage>::failure(
            invalidPacket(
                QStringLiteral(
                    "Relay packet identifiers or envelope metadata are invalid.")));
    }

    auto encodedEnvelope =
        RelayWireCodec::encodeEnvelope(envelope);
    if (!encodedEnvelope.hasValue()) {
        return Result<RelayWireMessage>::failure(
            encodedEnvelope.error());
    }

    RelayWireMessage message;
    message.type = RelayWireType::Packet;
    message.packetId = std::move(packetId);
    message.channelId = envelope.channelId;
    message.senderId = envelope.senderId;
    message.envelope =
        std::move(encodedEnvelope.value());
    return Result<RelayWireMessage>::success(
        std::move(message));
}

RelayWireMessage RelayModels::ping()
{
    RelayWireMessage message;
    message.type = RelayWireType::Ping;
    return message;
}

RelayWireMessage RelayModels::pong()
{
    RelayWireMessage message;
    message.type = RelayWireType::Pong;
    return message;
}

Result<EncryptedEnvelope>
RelayModels::decodedEnvelope(
    const RelayWireMessage& message)
{
    if (message.type != RelayWireType::Packet
        || message.protocolVersion != protocolVersion
        || !message.packetId.has_value()
        || !isValidOpaqueId(*message.packetId)
        || !message.channelId.has_value()
        || !message.senderId.has_value()
        || !message.envelope.has_value()) {
        return Result<EncryptedEnvelope>::failure(
            invalidPacket(
                QStringLiteral(
                    "Relay packet fields are incomplete or invalid.")));
    }

    auto decoded = RelayWireCodec::decodeEnvelope(
        *message.envelope);
    if (!decoded.hasValue()) {
        return decoded;
    }
    if (decoded.value().channelId
            != *message.channelId
        || decoded.value().senderId
            != *message.senderId) {
        return Result<EncryptedEnvelope>::failure(
            metadataMismatch());
    }
    return decoded;
}

MobileTransportRoute RelayModels::preferredRoute(
    bool nearbyConnected,
    bool relayRegistered,
    bool relayHandshakeVerified)
{
    if (nearbyConnected) {
        return MobileTransportRoute::Nearby;
    }
    if (relayRegistered
        && relayHandshakeVerified) {
        return MobileTransportRoute::Relay;
    }
    return MobileTransportRoute::Unavailable;
}

} // namespace companion
