#include "mobile/relay/RelayWireCodec.h"

#include <algorithm>
#include <charconv>
#include <limits>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

namespace companion {
namespace {

CompanionError invalidWire(QString message)
{
    return {
        QStringLiteral("relay.invalid_wire_message"),
        std::move(message),
        false,
        {},
    };
}

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

QByteArray encodeCanonicalString(
    const QString& value)
{
    QJsonArray scalar;
    scalar.append(value);
    const QByteArray bytes =
        QJsonDocument(scalar).toJson(
            QJsonDocument::Compact);
    QByteArray encoded =
        bytes.mid(1, bytes.size() - 2);
    encoded.replace("/", "\\/");
    return encoded;
}

QByteArray encodeCanonicalJson(
    const QJsonValue& value)
{
    if (value.isObject()) {
        const QJsonObject object =
            value.toObject();
        QStringList keys = object.keys();
        keys.sort();
        QByteArray output("{");
        for (qsizetype index = 0;
             index < keys.size();
             ++index) {
            if (index > 0) {
                output.append(',');
            }
            output.append(
                encodeCanonicalString(
                    keys.at(index)));
            output.append(':');
            output.append(
                encodeCanonicalJson(
                    object.value(
                        keys.at(index))));
        }
        output.append('}');
        return output;
    }
    if (value.isArray()) {
        const QJsonArray array =
            value.toArray();
        QByteArray output("[");
        for (qsizetype index = 0;
             index < array.size();
             ++index) {
            if (index > 0) {
                output.append(',');
            }
            output.append(
                encodeCanonicalJson(
                    array.at(index)));
        }
        output.append(']');
        return output;
    }
    if (value.isString()) {
        return encodeCanonicalString(
            value.toString());
    }
    QJsonArray scalar;
    scalar.append(value);
    const QByteArray bytes =
        QJsonDocument(scalar).toJson(
            QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2);
}

class TopLevelNumberIndex final {
public:
    explicit TopLevelNumberIndex(
        QByteArrayView bytes)
        : bytes_(bytes.data(), bytes.size())
    {
    }

    bool index()
    {
        skipWhitespace();
        if (!consume('{')) {
            return false;
        }
        skipWhitespace();
        if (consume('}')) {
            skipWhitespace();
            return position_ == bytes_.size();
        }

        QSet<QString> seenKeys;
        while (position_ < bytes_.size()) {
            QString key;
            if (!readString(&key)
                || seenKeys.contains(key)) {
                return false;
            }
            seenKeys.insert(key);
            skipWhitespace();
            if (!consume(':')) {
                return false;
            }
            skipWhitespace();

            if (position_ < bytes_.size()
                && (bytes_.at(position_) == '-'
                    || (bytes_.at(position_) >= '0'
                        && bytes_.at(position_)
                            <= '9'))) {
                const qsizetype start = position_;
                if (!skipNumber()) {
                    return false;
                }
                numbers_.insert(
                    key,
                    bytes_.mid(
                        start,
                        position_ - start));
            } else if (!skipValue()) {
                return false;
            }

            skipWhitespace();
            if (consume('}')) {
                skipWhitespace();
                return position_ == bytes_.size();
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    std::optional<QByteArray> number(
        QStringView key) const
    {
        const auto iterator =
            numbers_.constFind(key.toString());
        if (iterator == numbers_.constEnd()) {
            return std::nullopt;
        }
        return *iterator;
    }

private:
    bool skipValue()
    {
        skipWhitespace();
        if (position_ >= bytes_.size()) {
            return false;
        }
        const char current =
            bytes_.at(position_);
        if (current == '"') {
            return readString(nullptr);
        }
        if (current == '{') {
            return skipObject();
        }
        if (current == '[') {
            return skipArray();
        }
        if (current == '-'
            || (current >= '0'
                && current <= '9')) {
            return skipNumber();
        }
        return skipLiteral("true")
            || skipLiteral("false")
            || skipLiteral("null");
    }

    bool skipObject()
    {
        if (!consume('{')) {
            return false;
        }
        skipWhitespace();
        if (consume('}')) {
            return true;
        }
        while (position_ < bytes_.size()) {
            if (!readString(nullptr)) {
                return false;
            }
            skipWhitespace();
            if (!consume(':')
                || !skipValue()) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool skipArray()
    {
        if (!consume('[')) {
            return false;
        }
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (position_ < bytes_.size()) {
            if (!skipValue()) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                return false;
            }
            skipWhitespace();
        }
        return false;
    }

    bool readString(QString* value)
    {
        if (!consume('"')) {
            return false;
        }
        const qsizetype contentStart =
            position_ - 1;
        while (position_ < bytes_.size()) {
            const char current =
                bytes_.at(position_++);
            if (current == '\\') {
                if (position_ >= bytes_.size()) {
                    return false;
                }
                const char escape =
                    bytes_.at(position_++);
                if (escape == 'u') {
                    if (position_ + 4
                        > bytes_.size()) {
                        return false;
                    }
                    for (int index = 0;
                         index < 4;
                         ++index) {
                        const char digit =
                            bytes_.at(
                                position_ + index);
                        const bool hexadecimal =
                            (digit >= '0'
                             && digit <= '9')
                            || (digit >= 'a'
                                && digit <= 'f')
                            || (digit >= 'A'
                                && digit <= 'F');
                        if (!hexadecimal) {
                            return false;
                        }
                    }
                    position_ += 4;
                } else if (
                    escape != '"'
                    && escape != '\\'
                    && escape != '/'
                    && escape != 'b'
                    && escape != 'f'
                    && escape != 'n'
                    && escape != 'r'
                    && escape != 't') {
                    return false;
                }
                continue;
            }
            if (current == '"') {
                if (value != nullptr) {
                    QByteArray wrapped("[");
                    wrapped.append(
                        bytes_.mid(
                            contentStart,
                            position_
                                - contentStart));
                    wrapped.append(']');
                    const QJsonDocument document =
                        QJsonDocument::fromJson(
                            wrapped);
                    if (!document.isArray()
                        || document.array().size()
                            != 1
                        || !document.array()
                                .at(0)
                                .isString()) {
                        return false;
                    }
                    *value =
                        document.array()
                            .at(0)
                            .toString();
                }
                return true;
            }
            if (static_cast<unsigned char>(
                    current)
                < 0x20U) {
                return false;
            }
        }
        return false;
    }

    bool skipNumber()
    {
        if (consume('-')
            && position_ >= bytes_.size()) {
            return false;
        }
        if (consume('0')) {
        } else {
            if (position_ >= bytes_.size()
                || bytes_.at(position_) < '1'
                || bytes_.at(position_) > '9') {
                return false;
            }
            ++position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
        }
        if (consume('.')) {
            const qsizetype start = position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        if (position_ < bytes_.size()
            && (bytes_.at(position_) == 'e'
                || bytes_.at(position_)
                    == 'E')) {
            ++position_;
            consume('+');
            consume('-');
            const qsizetype start = position_;
            while (
                position_ < bytes_.size()
                && bytes_.at(position_) >= '0'
                && bytes_.at(position_) <= '9') {
                ++position_;
            }
            if (position_ == start) {
                return false;
            }
        }
        return true;
    }

    bool skipLiteral(const char* literal)
    {
        const QByteArrayView candidate(literal);
        if (bytes_.mid(
                position_,
                candidate.size())
            != candidate) {
            return false;
        }
        position_ += candidate.size();
        return true;
    }

    bool consume(char expected)
    {
        if (position_ < bytes_.size()
            && bytes_.at(position_)
                == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (
            position_ < bytes_.size()
            && (bytes_.at(position_) == ' '
                || bytes_.at(position_) == '\n'
                || bytes_.at(position_) == '\r'
                || bytes_.at(position_) == '\t')) {
            ++position_;
        }
    }

    QByteArray bytes_;
    qsizetype position_ = 0;
    QHash<QString, QByteArray> numbers_;
};

template <typename Integer>
std::optional<Integer> parseInteger(
    const std::optional<QByteArray>& token)
{
    if (!token.has_value()
        || token->isEmpty()
        || token->contains('.')
        || token->contains('e')
        || token->contains('E')
        || token->at(0) == '+') {
        return std::nullopt;
    }
    if (token->size() > 1
        && token->at(0) == '0') {
        return std::nullopt;
    }
    if (token->size() > 2
        && token->at(0) == '-'
        && token->at(1) == '0') {
        return std::nullopt;
    }

    Integer value{};
    const char* first = token->constData();
    const char* last = first + token->size();
    const auto parsed =
        std::from_chars(first, last, value);
    if (parsed.ec != std::errc()
        || parsed.ptr != last) {
        return std::nullopt;
    }
    return value;
}

Result<std::optional<QString>>
optionalString(
    const QJsonObject& object,
    QStringView key)
{
    const QJsonValue value =
        object.value(key);
    if (value.isUndefined()
        || value.isNull()) {
        return Result<std::optional<QString>>
            ::success(std::nullopt);
    }
    if (!value.isString()) {
        return Result<std::optional<QString>>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay field %1 must be a string.")
                    .arg(key)));
    }
    return Result<std::optional<QString>>
        ::success(value.toString());
}

Result<std::optional<QByteArray>>
optionalBase64(
    const QJsonObject& object,
    QStringView key)
{
    const auto encoded =
        optionalString(object, key);
    if (!encoded.hasValue()) {
        return Result<std::optional<QByteArray>>
            ::failure(encoded.error());
    }
    if (!encoded.value().has_value()) {
        return Result<std::optional<QByteArray>>
            ::success(std::nullopt);
    }
    const QByteArray text =
        encoded.value()->toLatin1();
    if (QString::fromLatin1(text)
            != *encoded.value()
        || text.size() % 4 != 0) {
        return Result<std::optional<QByteArray>>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay field %1 is not canonical base64.")
                    .arg(key)));
    }
    const auto decoded =
        QByteArray::fromBase64Encoding(
            text,
            QByteArray::
                AbortOnBase64DecodingErrors);
    if (!decoded
        || decoded.decoded.toBase64()
            != text) {
        return Result<std::optional<QByteArray>>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay field %1 is not canonical base64.")
                    .arg(key)));
    }
    return Result<std::optional<QByteArray>>
        ::success(decoded.decoded);
}

bool hasAnyPacketMetadata(
    const RelayWireMessage& message)
{
    return message.packetId.has_value()
        || message.channelId.has_value()
        || message.endpointId.has_value()
        || message.senderId.has_value()
        || message.envelope.has_value()
        || message.peerCount.has_value()
        || message.status.has_value()
        || message.code.has_value()
        || message.message.has_value();
}

Result<void> validateMessageForEncoding(
    const RelayWireMessage& message)
{
    if (message.protocolVersion
        != RelayModels::protocolVersion) {
        return Result<void>::failure(
            invalidWire(
                QStringLiteral(
                    "Unsupported relay protocol version.")));
    }

    switch (message.type) {
    case RelayWireType::Register:
        if (!message.channelId.has_value()
            || message.channelId->isEmpty()
            || !message.endpointId.has_value()
            || message.endpointId->isEmpty()
            || message.packetId.has_value()
            || message.senderId.has_value()
            || message.envelope.has_value()
            || message.peerCount.has_value()
            || message.status.has_value()
            || message.code.has_value()
            || message.message.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay registration fields are invalid.")));
        }
        break;
    case RelayWireType::Registered:
        if (hasAnyPacketMetadata(message)) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay registration acknowledgement must not contain metadata.")));
        }
        break;
    case RelayWireType::PeerPresence:
        if (!message.peerCount.has_value()
            || *message.peerCount < 0
            || message.packetId.has_value()
            || message.channelId.has_value()
            || message.endpointId.has_value()
            || message.senderId.has_value()
            || message.envelope.has_value()
            || message.status.has_value()
            || message.code.has_value()
            || message.message.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay peer-presence fields are invalid.")));
        }
        break;
    case RelayWireType::Packet: {
        const auto decoded =
            RelayModels::decodedEnvelope(message);
        if (!decoded.hasValue()) {
            return Result<void>::failure(
                decoded.error());
        }
        if (message.endpointId.has_value()
            || message.peerCount.has_value()
            || message.status.has_value()
            || message.code.has_value()
            || message.message.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay packet includes unrelated fields.")));
        }
        break;
    }
    case RelayWireType::PacketResult:
        if (!message.packetId.has_value()
            || !RelayModels::isValidOpaqueId(
                *message.packetId)
            || !message.status.has_value()
            || message.channelId.has_value()
            || message.endpointId.has_value()
            || message.senderId.has_value()
            || message.envelope.has_value()
            || message.peerCount.has_value()
            || message.code.has_value()
            || message.message.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay packet-result fields are invalid.")));
        }
        break;
    case RelayWireType::Ping:
    case RelayWireType::Pong:
        if (hasAnyPacketMetadata(message)) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay keepalive message must not contain metadata.")));
        }
        break;
    case RelayWireType::Error:
        if (message.packetId.has_value()
            || message.channelId.has_value()
            || message.endpointId.has_value()
            || message.senderId.has_value()
            || message.envelope.has_value()
            || message.peerCount.has_value()
            || message.status.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay error includes unrelated fields.")));
        }
        break;
    }
    return Result<void>::success();
}

Result<void> validateMessageForDecoding(
    const RelayWireMessage& message)
{
    if (message.protocolVersion
        != RelayModels::protocolVersion) {
        return Result<void>::failure(
            invalidWire(
                QStringLiteral(
                    "Unsupported relay protocol version.")));
    }

    switch (message.type) {
    case RelayWireType::Register:
        if (!message.channelId.has_value()
            || message.channelId->isEmpty()
            || !message.endpointId.has_value()
            || message.endpointId->isEmpty()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay registration fields are invalid.")));
        }
        break;
    case RelayWireType::PeerPresence:
        if (!message.peerCount.has_value()
            || *message.peerCount < 0) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay peer-presence fields are invalid.")));
        }
        break;
    case RelayWireType::Packet: {
        const auto decoded =
            RelayModels::decodedEnvelope(message);
        if (!decoded.hasValue()) {
            return Result<void>::failure(
                decoded.error());
        }
        break;
    }
    case RelayWireType::PacketResult:
        if (!message.packetId.has_value()
            || !RelayModels::isValidOpaqueId(
                *message.packetId)
            || !message.status.has_value()) {
            return Result<void>::failure(
                invalidWire(
                    QStringLiteral(
                        "Relay packet-result fields are invalid.")));
        }
        break;
    case RelayWireType::Registered:
    case RelayWireType::Ping:
    case RelayWireType::Pong:
    case RelayWireType::Error:
        break;
    }
    return Result<void>::success();
}

void putOptional(
    QJsonObject& object,
    QStringView key,
    const std::optional<QString>& value)
{
    if (value.has_value()) {
        object.insert(key, *value);
    }
}

} // namespace

Result<QByteArray> RelayWireCodec::encode(
    const RelayWireMessage& message)
{
    const auto validation =
        validateMessageForEncoding(message);
    if (!validation.hasValue()) {
        return Result<QByteArray>::failure(
            validation.error());
    }

    QJsonObject object;
    object.insert(
        u"type",
        RelayModels::wireName(message.type));
    object.insert(
        u"protocolVersion",
        message.protocolVersion);
    putOptional(
        object, u"packetID",
        message.packetId);
    putOptional(
        object, u"channelID",
        message.channelId);
    putOptional(
        object, u"endpointID",
        message.endpointId);
    putOptional(
        object, u"senderID",
        message.senderId);
    if (message.envelope.has_value()) {
        object.insert(
            u"envelope",
            QString::fromLatin1(
                message.envelope->toBase64()));
    }
    if (message.peerCount.has_value()) {
        object.insert(
            u"peerCount",
            *message.peerCount);
    }
    if (message.status.has_value()) {
        object.insert(
            u"status",
            RelayModels::wireName(
                *message.status));
    }
    putOptional(
        object, u"code",
        message.code);
    putOptional(
        object, u"message",
        message.message);

    return Result<QByteArray>::success(
        encodeCanonicalJson(object));
}

Result<RelayWireMessage> RelayWireCodec::decode(
    QByteArrayView bytes)
{
    QJsonParseError parseError;
    const QByteArray owned(
        bytes.data(), bytes.size());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            owned, &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<RelayWireMessage>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay payload is not a JSON object.")));
    }

    TopLevelNumberIndex numbers(bytes);
    if (!numbers.index()) {
        return Result<RelayWireMessage>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay payload contains ambiguous JSON.")));
    }
    const auto protocol =
        parseInteger<int>(
            numbers.number(
                u"protocolVersion"));
    if (!protocol.has_value()
        || *protocol
            != RelayModels::protocolVersion) {
        return Result<RelayWireMessage>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay protocolVersion is required and must equal 1.")));
    }

    const QJsonObject object =
        document.object();
    const QJsonValue typeValue =
        object.value(u"type");
    if (!typeValue.isString()) {
        return Result<RelayWireMessage>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay type is required.")));
    }
    const auto type =
        RelayModels::wireType(
            typeValue.toString());
    if (!type.has_value()) {
        return Result<RelayWireMessage>
            ::failure(invalidWire(
                QStringLiteral(
                    "Relay type is unknown.")));
    }

    RelayWireMessage message;
    message.type = *type;
    message.protocolVersion = *protocol;

    auto assignString =
        [&object](
            QStringView key,
            std::optional<QString>& target)
        -> Result<void> {
        const auto decoded =
            optionalString(object, key);
        if (!decoded.hasValue()) {
            return Result<void>::failure(
                decoded.error());
        }
        target = decoded.value();
        return Result<void>::success();
    };

    for (const auto& field :
         std::initializer_list<
             std::pair<QStringView,
                       std::optional<QString>*>>{
             {u"packetID",
              &message.packetId},
             {u"channelID",
              &message.channelId},
             {u"endpointID",
              &message.endpointId},
             {u"senderID",
              &message.senderId},
             {u"code", &message.code},
             {u"message",
              &message.message},
         }) {
        const auto assigned =
            assignString(
                field.first,
                *field.second);
        if (!assigned.hasValue()) {
            return Result<RelayWireMessage>
                ::failure(assigned.error());
        }
    }

    const auto envelope =
        optionalBase64(object, u"envelope");
    if (!envelope.hasValue()) {
        return Result<RelayWireMessage>
            ::failure(envelope.error());
    }
    message.envelope = envelope.value();

    const QJsonValue peerCountValue =
        object.value(u"peerCount");
    if (!peerCountValue.isUndefined()
        && !peerCountValue.isNull()) {
        const auto peerCount =
            parseInteger<int>(
                numbers.number(
                    u"peerCount"));
        if (!peerCount.has_value()) {
            return Result<RelayWireMessage>
                ::failure(invalidWire(
                    QStringLiteral(
                        "Relay peerCount must be an integer.")));
        }
        message.peerCount = *peerCount;
    }

    const QJsonValue statusValue =
        object.value(u"status");
    if (!statusValue.isUndefined()
        && !statusValue.isNull()) {
        if (!statusValue.isString()) {
            return Result<RelayWireMessage>
                ::failure(invalidWire(
                    QStringLiteral(
                        "Relay status must be a string.")));
        }
        const auto status =
            RelayModels::packetResultStatus(
                statusValue.toString());
        if (!status.has_value()) {
            return Result<RelayWireMessage>
                ::failure(invalidWire(
                    QStringLiteral(
                        "Relay packet status is unknown.")));
        }
        message.status = *status;
    }

    const auto validation =
        validateMessageForDecoding(
            message);
    if (!validation.hasValue()) {
        return Result<RelayWireMessage>
            ::failure(validation.error());
    }
    return Result<RelayWireMessage>::success(
        std::move(message));
}

Result<QByteArray>
RelayWireCodec::encodeEnvelope(
    const EncryptedEnvelope& envelope)
{
    if (envelope.version != 1
        || envelope.channelId.isEmpty()
        || envelope.senderId.isEmpty()) {
        return Result<QByteArray>::failure(
            invalidPacket(
                QStringLiteral(
                    "Encrypted relay envelope metadata is invalid.")));
    }

    QByteArray output("{\"channelID\":");
    output.append(
        encodeCanonicalString(
            envelope.channelId));
    output.append(
        ",\"sealedPayload\":");
    output.append(
        encodeCanonicalString(
            QString::fromLatin1(
                envelope.sealedPayload
                    .toBase64())));
    output.append(",\"senderID\":");
    output.append(
        encodeCanonicalString(
            envelope.senderId));
    output.append(
        ",\"sentAtMilliseconds\":");
    output.append(
        QByteArray::number(
            envelope.sentAtMilliseconds));
    output.append(",\"sequence\":");
    output.append(
        QByteArray::number(
            envelope.sequence));
    output.append(",\"version\":");
    output.append(
        QByteArray::number(
            envelope.version));
    output.append('}');
    return Result<QByteArray>::success(
        std::move(output));
}

Result<EncryptedEnvelope>
RelayWireCodec::decodeEnvelope(
    QByteArrayView bytes)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            QByteArray(bytes.data(), bytes.size()),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<EncryptedEnvelope>
            ::failure(invalidPacket(
                QStringLiteral(
                    "Encrypted relay envelope is not a JSON object.")));
    }

    TopLevelNumberIndex numbers(bytes);
    if (!numbers.index()) {
        return Result<EncryptedEnvelope>
            ::failure(invalidPacket(
                QStringLiteral(
                    "Encrypted relay envelope contains ambiguous JSON.")));
    }

    const auto version =
        parseInteger<int>(
            numbers.number(u"version"));
    const auto sequence =
        parseInteger<quint64>(
            numbers.number(u"sequence"));
    const auto sentAt =
        parseInteger<qint64>(
            numbers.number(
                u"sentAtMilliseconds"));
    if (!version.has_value()
        || *version != 1
        || !sequence.has_value()
        || !sentAt.has_value()) {
        return Result<EncryptedEnvelope>
            ::failure(invalidPacket(
                QStringLiteral(
                    "Encrypted relay envelope numbers are invalid.")));
    }

    const QJsonObject object =
        document.object();
    const auto channel =
        optionalString(object, u"channelID");
    const auto sender =
        optionalString(object, u"senderID");
    const auto payload =
        optionalBase64(
            object, u"sealedPayload");
    if (!channel.hasValue()) {
        return Result<EncryptedEnvelope>
            ::failure(channel.error());
    }
    if (!sender.hasValue()) {
        return Result<EncryptedEnvelope>
            ::failure(sender.error());
    }
    if (!payload.hasValue()) {
        return Result<EncryptedEnvelope>
            ::failure(payload.error());
    }
    if (!channel.value().has_value()
        || channel.value()->isEmpty()
        || !sender.value().has_value()
        || sender.value()->isEmpty()
        || !payload.value().has_value()) {
        return Result<EncryptedEnvelope>
            ::failure(invalidPacket(
                QStringLiteral(
                    "Encrypted relay envelope fields are incomplete.")));
    }

    return Result<EncryptedEnvelope>::success({
        *version,
        *channel.value(),
        *sender.value(),
        *sequence,
        *sentAt,
        *payload.value(),
    });
}

} // namespace companion
