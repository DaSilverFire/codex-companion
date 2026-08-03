#include "mobile/nearby/NearbyFrameCodec.h"

#include "platform/windows/security/WindowsCrypto.h"

#include <limits>
#include <optional>
#include <utility>

namespace companion {
namespace {

constexpr qsizetype kAuthenticatedHeaderBytes = 56;
constexpr qsizetype kAuthenticationBytes = 32;
constexpr quint8 kVersion = 1;

CompanionError frameError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

Result<void> invalidFrame(
    QString code,
    QString message)
{
    return Result<void>::failure(
        frameError(
            std::move(code),
            std::move(message)));
}

void appendBigEndian16(
    QByteArray& bytes,
    quint16 value)
{
    bytes.append(
        static_cast<char>(
            (value >> 8) & 0xFFU));
    bytes.append(
        static_cast<char>(
            value & 0xFFU));
}

void appendBigEndian32(
    QByteArray& bytes,
    quint32 value)
{
    bytes.append(
        static_cast<char>(
            (value >> 24) & 0xFFU));
    bytes.append(
        static_cast<char>(
            (value >> 16) & 0xFFU));
    bytes.append(
        static_cast<char>(
            (value >> 8) & 0xFFU));
    bytes.append(
        static_cast<char>(
            value & 0xFFU));
}

quint16 readBigEndian16(
    QByteArrayView bytes,
    qsizetype offset)
{
    const auto* data =
        reinterpret_cast<
            const unsigned char*>(
            bytes.data() + offset);
    return static_cast<quint16>(
        (quint16(data[0]) << 8)
        | quint16(data[1]));
}

quint32 readBigEndian32(
    QByteArrayView bytes,
    qsizetype offset)
{
    const auto* data =
        reinterpret_cast<
            const unsigned char*>(
            bytes.data() + offset);
    return (quint32(data[0]) << 24)
        | (quint32(data[1]) << 16)
        | (quint32(data[2]) << 8)
        | quint32(data[3]);
}

std::optional<NearbyFrameType> frameType(
    quint8 value)
{
    switch (value) {
    case 1:
        return NearbyFrameType::Request;
    case 2:
        return NearbyFrameType::AttachmentBegin;
    case 3:
        return NearbyFrameType::AttachmentChunk;
    case 4:
        return NearbyFrameType::AttachmentCommit;
    case 5:
        return NearbyFrameType::TransferCancel;
    default:
        return std::nullopt;
    }
}

bool isStableAsciiCode(
    QByteArrayView value)
{
    if (value.isEmpty()
        || value.size()
            > NearbyFrameCodec::
                maximumCancelBytes) {
        return false;
    }
    for (const char character : value) {
        const unsigned char byte =
            static_cast<unsigned char>(
                character);
        const bool valid =
            (byte >= 'a' && byte <= 'z')
            || (byte >= '0' && byte <= '9')
            || byte == '.'
            || byte == '_'
            || byte == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

Result<void> validateFrame(
    const NearbyFrame& frame,
    QByteArrayView secret)
{
    if (secret.size() != 32) {
        return invalidFrame(
            QStringLiteral(
                "nearby.invalid_secret"),
            QStringLiteral(
                "Nearby authentication requires a 32-byte pairing secret."));
    }
    if (frame.flags != 0) {
        return invalidFrame(
            QStringLiteral(
                "nearby.invalid_flags"),
            QStringLiteral(
                "Nearby frame flags are unsupported."));
    }
    if (frame.transferId.isNull()) {
        return invalidFrame(
            QStringLiteral(
                "nearby.invalid_transfer"),
            QStringLiteral(
                "Nearby frames require a transfer identifier."));
    }
    if (frame.payload.size()
        > NearbyFrameCodec::
            maximumRequestBytes) {
        return invalidFrame(
            QStringLiteral(
                "nearby.payload_too_large"),
            QStringLiteral(
                "The nearby frame payload is too large."));
    }
    if (static_cast<quint64>(
            frame.payload.size())
        > std::numeric_limits<
              quint32>::max()) {
        return invalidFrame(
            QStringLiteral(
                "nearby.payload_too_large"),
            QStringLiteral(
                "The nearby frame payload is too large."));
    }

    switch (frame.type) {
    case NearbyFrameType::Request:
        if (!frame.attachmentId.isNull()
            || frame.chunkIndex != 0
            || frame.chunkCount != 0
            || frame.payload.isEmpty()) {
            return invalidFrame(
                QStringLiteral(
                    "nearby.invalid_request_frame"),
                QStringLiteral(
                    "The nearby request frame is invalid."));
        }
        break;
    case NearbyFrameType::AttachmentBegin:
        if (frame.attachmentId.isNull()
            || frame.chunkIndex != 0
            || frame.payload.isEmpty()) {
            return invalidFrame(
                QStringLiteral(
                    "nearby.invalid_begin_frame"),
                QStringLiteral(
                    "The nearby attachment-begin frame is invalid."));
        }
        break;
    case NearbyFrameType::AttachmentChunk:
        if (frame.attachmentId.isNull()
            || frame.chunkCount == 0
            || frame.chunkIndex
                >= frame.chunkCount
            || frame.payload.isEmpty()
            || frame.payload.size()
                > NearbyFrameCodec::
                    maximumChunkBytes) {
            return invalidFrame(
                QStringLiteral(
                    "nearby.invalid_chunk_frame"),
                QStringLiteral(
                    "The nearby attachment chunk is invalid."));
        }
        break;
    case NearbyFrameType::AttachmentCommit:
        if (frame.attachmentId.isNull()
            || frame.chunkIndex != 0
            || !frame.payload.isEmpty()) {
            return invalidFrame(
                QStringLiteral(
                    "nearby.invalid_commit_frame"),
                QStringLiteral(
                    "The nearby attachment commit is invalid."));
        }
        break;
    case NearbyFrameType::TransferCancel:
        if (!frame.attachmentId.isNull()
            || frame.chunkIndex != 0
            || frame.chunkCount != 0
            || !isStableAsciiCode(
                frame.payload)) {
            return invalidFrame(
                QStringLiteral(
                    "nearby.invalid_cancel_frame"),
                QStringLiteral(
                    "The nearby transfer cancellation is invalid."));
        }
        break;
    }
    return Result<void>::success();
}

QByteArray authenticatedBytes(
    QByteArrayView header,
    QByteArrayView payload)
{
    QByteArray result;
    result.reserve(
        header.size() + payload.size());
    result.append(
        header.data(),
        header.size());
    result.append(
        payload.data(),
        payload.size());
    return result;
}

} // namespace

Result<QByteArray> NearbyFrameCodec::encode(
    const NearbyFrame& frame,
    QByteArrayView secret)
{
    const auto validation =
        validateFrame(frame, secret);
    if (!validation.hasValue()) {
        return Result<QByteArray>::failure(
            validation.error());
    }

    QByteArray header;
    header.reserve(kAuthenticatedHeaderBytes);
    header.append("CCN1", 4);
    header.append(
        static_cast<char>(kVersion));
    header.append(
        static_cast<char>(frame.type));
    appendBigEndian16(header, frame.flags);
    header.append(
        frame.transferId.toRfc4122());
    header.append(
        frame.attachmentId.isNull()
            ? QByteArray(16, '\0')
            : frame.attachmentId
                  .toRfc4122());
    appendBigEndian32(
        header,
        frame.chunkIndex);
    appendBigEndian32(
        header,
        frame.chunkCount);
    appendBigEndian32(
        header,
        static_cast<quint32>(
            frame.payload.size()));
    appendBigEndian32(header, 0);
    if (header.size()
        != kAuthenticatedHeaderBytes) {
        return Result<QByteArray>::failure(
            frameError(
                QStringLiteral(
                    "nearby.header_failed"),
                QStringLiteral(
                    "Codex Companion could not construct the nearby frame header.")));
    }

    const QByteArray authenticated =
        authenticatedBytes(
            header,
            frame.payload);
    const auto authentication =
        WindowsCrypto::hmacSha256(
            secret,
            authenticated);
    if (!authentication.hasValue()) {
        return Result<QByteArray>::failure(
            authentication.error());
    }
    if (authentication.value().size()
        != kAuthenticationBytes) {
        return Result<QByteArray>::failure(
            frameError(
                QStringLiteral(
                    "nearby.authentication_failed"),
                QStringLiteral(
                    "Windows cryptography returned an invalid nearby frame authenticator.")));
    }

    QByteArray result;
    result.reserve(
        headerBytes + frame.payload.size());
    result.append(header);
    result.append(authentication.value());
    result.append(frame.payload);
    return Result<QByteArray>::success(
        std::move(result));
}

Result<NearbyFrame> NearbyFrameCodec::decode(
    QByteArrayView bytes,
    QByteArrayView secret)
{
    if (bytes.size() < headerBytes) {
        return Result<NearbyFrame>::failure(
            frameError(
                QStringLiteral(
                    "nearby.frame_truncated"),
                QStringLiteral(
                    "The nearby frame is truncated.")));
    }
    const QByteArrayView header =
        bytes.first(kAuthenticatedHeaderBytes);
    if (header.first(4)
            != QByteArrayView("CCN1", 4)
        || static_cast<quint8>(
               header.at(4))
            != kVersion) {
        return Result<NearbyFrame>::failure(
            frameError(
                QStringLiteral(
                    "nearby.invalid_header"),
                QStringLiteral(
                    "The nearby frame header is invalid.")));
    }
    const auto type =
        frameType(
            static_cast<quint8>(
                header.at(5)));
    if (!type.has_value()) {
        return Result<NearbyFrame>::failure(
            frameError(
                QStringLiteral(
                    "nearby.invalid_type"),
                QStringLiteral(
                    "The nearby frame type is unsupported.")));
    }

    const quint32 payloadLength =
        readBigEndian32(header, 48);
    if (readBigEndian32(header, 52) != 0
        || static_cast<quint64>(
               payloadLength)
            > static_cast<quint64>(
                std::numeric_limits<
                    qsizetype>::max())
        || bytes.size()
            != headerBytes
                + static_cast<qsizetype>(
                    payloadLength)) {
        return Result<NearbyFrame>::failure(
            frameError(
                QStringLiteral(
                    "nearby.invalid_length"),
                QStringLiteral(
                    "The nearby frame length is invalid.")));
    }

    const QByteArrayView payload =
        bytes.sliced(
            headerBytes,
            static_cast<qsizetype>(
                payloadLength));
    const QByteArray authenticated =
        authenticatedBytes(
            header,
            payload);
    const auto expected =
        WindowsCrypto::hmacSha256(
            secret,
            authenticated);
    if (!expected.hasValue()) {
        return Result<NearbyFrame>::failure(
            expected.error());
    }
    const QByteArrayView supplied =
        bytes.sliced(
            kAuthenticatedHeaderBytes,
            kAuthenticationBytes);
    if (!WindowsCrypto::constantTimeEquals(
            supplied,
            expected.value())) {
        return Result<NearbyFrame>::failure(
            frameError(
                QStringLiteral(
                    "nearby.authentication_failed"),
                QStringLiteral(
                    "The nearby frame authenticator is invalid.")));
    }

    NearbyFrame frame{
        *type,
        readBigEndian16(header, 6),
        QUuid::fromRfc4122(
            header.sliced(8, 16)
                .toByteArray()),
        QUuid::fromRfc4122(
            header.sliced(24, 16)
                .toByteArray()),
        readBigEndian32(header, 40),
        readBigEndian32(header, 44),
        payload.toByteArray(),
    };
    const auto validation =
        validateFrame(frame, secret);
    if (!validation.hasValue()) {
        return Result<NearbyFrame>::failure(
            validation.error());
    }
    return Result<NearbyFrame>::success(
        std::move(frame));
}

} // namespace companion
