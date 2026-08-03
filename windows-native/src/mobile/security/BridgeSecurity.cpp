#include "mobile/security/BridgeSecurity.h"

#include "platform/windows/security/WindowsCrypto.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTextBoundaryFinder>

#include <limits>
#include <utility>

namespace companion {
namespace {

constexpr qsizetype kHmacSecretMinimumBytes = 32;
constexpr qsizetype kRelaySecretBytes = 32;
constexpr qsizetype kInvitationNonceBytes = 16;
constexpr qsizetype kEnvelopeNonceBytes = 12;
constexpr qsizetype kEnvelopeTagBytes = 16;
constexpr qsizetype kEnvelopeMinimumBytes =
    kEnvelopeNonceBytes + kEnvelopeTagBytes;

CompanionError securityError(
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

void appendInt64BigEndian(
    QByteArray& output,
    qint64 value)
{
    const quint64 bits =
        static_cast<quint64>(value);
    for (int shift = 56;
         shift >= 0;
         shift -= 8) {
        output.append(static_cast<char>(
            (bits >> shift) & 0xFFU));
    }
}

Result<void> appendLengthPrefixed(
    QByteArray& output,
    QByteArrayView value)
{
    if (value.size() < 0
        || static_cast<quint64>(value.size())
            > std::numeric_limits<quint32>::max()) {
        return Result<void>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.field_too_large"),
                QStringLiteral(
                    "A Companion authentication field is too large.")));
    }

    const quint32 length =
        static_cast<quint32>(
            value.size());
    for (int shift = 24;
         shift >= 0;
         shift -= 8) {
        output.append(static_cast<char>(
            (length >> shift) & 0xFFU));
    }
    output.append(
        value.data(),
        value.size());
    return Result<void>::success();
}

QByteArray jsonString(
    const QString& value)
{
    const QByteArray array =
        QJsonDocument(
            QJsonArray{value})
            .toJson(QJsonDocument::Compact);
    QByteArray encoded =
        array.mid(1, array.size() - 2);
    encoded.replace("/", "\\/");
    return encoded;
}

quint64 absoluteDistance(
    qint64 left,
    qint64 right) noexcept
{
    return left >= right
        ? static_cast<quint64>(left)
            - static_cast<quint64>(right)
        : static_cast<quint64>(right)
            - static_cast<quint64>(left);
}

Result<void> validateHmacSecret(
    QByteArrayView secret)
{
    if (secret.size()
        < kHmacSecretMinimumBytes) {
        return Result<void>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.invalid_secret"),
                QStringLiteral(
                    "Companion pairing secrets must contain at least 32 bytes.")));
    }
    return Result<void>::success();
}

Result<void> validateRelaySecret(
    QByteArrayView secret)
{
    if (secret.size() != kRelaySecretBytes) {
        return Result<void>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.invalid_secret"),
                QStringLiteral(
                    "Companion relay secrets must contain exactly 32 bytes.")));
    }
    return Result<void>::success();
}

class OptionalSecretGuard final {
public:
    explicit OptionalSecretGuard(
        std::optional<QByteArray>& secret)
        : secret_(&secret)
    {
    }

    ~OptionalSecretGuard()
    {
        if (secret_ != nullptr
            && secret_->has_value()) {
            WindowsCrypto::secureZero(
                **secret_);
        }
    }

    OptionalSecretGuard(
        const OptionalSecretGuard&) = delete;
    OptionalSecretGuard& operator=(
        const OptionalSecretGuard&) = delete;

private:
    std::optional<QByteArray>* secret_ =
        nullptr;
};

char32_t firstScalar(
    QStringView text) noexcept
{
    if (text.isEmpty()) {
        return 0;
    }
    const QChar first = text.front();
    if (first.isHighSurrogate()
        && text.size() > 1
        && text.at(1).isLowSurrogate()) {
        return QChar::surrogateToUcs4(
            first,
            text.at(1));
    }
    return first.unicode();
}

qsizetype graphemeCount(
    const QString& text)
{
    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme,
        text);
    finder.toStart();
    qsizetype count = 0;
    while (finder.toNextBoundary() >= 0) {
        ++count;
    }
    return count;
}

} // namespace

Result<BridgeInvitation>
BridgeSecurity::authenticatedInvitation(
    QString deviceId,
    QString displayName,
    QByteArrayView secret,
    QDateTime now,
    QByteArray nonce)
{
    const auto secretValidation =
        validateHmacSecret(secret);
    if (!secretValidation.hasValue()) {
        return Result<BridgeInvitation>::
            failure(secretValidation.error());
    }
    if (!now.isValid()) {
        return Result<BridgeInvitation>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.invalid_time"),
                QStringLiteral(
                    "The Companion invitation time is invalid.")));
    }

    BridgeInvitation invitation{
        invitationVersion,
        std::move(deviceId),
        std::move(displayName),
        now.toMSecsSinceEpoch(),
        std::move(nonce),
        std::nullopt,
        std::nullopt,
    };
    const auto authenticationData =
        invitationAuthenticationData(
            invitation);
    if (!authenticationData.hasValue()) {
        return Result<BridgeInvitation>::
            failure(authenticationData.error());
    }
    auto authenticator =
        WindowsCrypto::hmacSha256(
            secret,
            authenticationData.value());
    if (!authenticator.hasValue()) {
        return Result<BridgeInvitation>::
            failure(authenticator.error());
    }
    invitation.authenticator =
        std::move(authenticator.value());
    return Result<BridgeInvitation>::success(
        std::move(invitation));
}

InvitationDecision
BridgeSecurity::decideInvitation(
    const BridgeInvitation& invitation,
    std::optional<QByteArray> trustedSecret,
    std::optional<ActivePairing> activePairing,
    QDateTime now)
{
    OptionalSecretGuard secretGuard(
        trustedSecret);
    if (invitation.version
        != invitationVersion) {
        return InvitationDecision::RejectVersion;
    }
    if (!now.isValid()
        || absoluteDistance(
               now.toMSecsSinceEpoch(),
               invitation.issuedAtMilliseconds)
            > static_cast<quint64>(
                invitationClockSkewMilliseconds)) {
        return InvitationDecision::RejectExpired;
    }

    if (trustedSecret.has_value()) {
        if (trustedSecret->size()
                < kHmacSecretMinimumBytes
            || !invitation.authenticator
                    .has_value()) {
            WindowsCrypto::secureZero(
                *trustedSecret);
            return InvitationDecision::
                RejectAuthentication;
        }

        const auto authenticationData =
            invitationAuthenticationData(
                invitation);
        if (!authenticationData.hasValue()) {
            WindowsCrypto::secureZero(
                *trustedSecret);
            return InvitationDecision::
                RejectAuthentication;
        }
        auto expected =
            WindowsCrypto::hmacSha256(
                *trustedSecret,
                authenticationData.value());
        WindowsCrypto::secureZero(
            *trustedSecret);
        if (!expected.hasValue()) {
            return InvitationDecision::
                RejectAuthentication;
        }

        const bool accepted =
            WindowsCrypto::constantTimeEquals(
                expected.value(),
                *invitation.authenticator);
        WindowsCrypto::secureZero(
            expected.value());
        return accepted
            ? InvitationDecision::AcceptTrusted
            : InvitationDecision::
                RejectAuthentication;
    }

    if (activePairing.has_value()
        && activePairing->expiresAt.isValid()
        && activePairing->expiresAt >= now) {
        const auto expected =
            normalizedPairingCode(
                activePairing->code);
        const auto supplied =
            invitation.pairingCode.has_value()
            ? normalizedPairingCode(
                  *invitation.pairingCode)
            : std::nullopt;
        if (expected.has_value()
            && supplied.has_value()
            && graphemeCount(*expected) == 6
            && *expected == *supplied) {
            return InvitationDecision::
                AcceptPairing;
        }
    }

    return InvitationDecision::RejectUnpaired;
}

Result<QString> BridgeSecurity::channelId(
    QByteArrayView secret)
{
    auto digest = WindowsCrypto::hmacSha256(
        secret,
        QByteArrayView(
            "codex-companion-relay-channel-v1"));
    if (!digest.hasValue()) {
        return Result<QString>::failure(
            digest.error());
    }
    const QString result =
        QString::fromLatin1(
            digest.value()
                .first(24)
                .toBase64(
                    QByteArray::Base64UrlEncoding
                    | QByteArray::
                        OmitTrailingEquals));
    WindowsCrypto::secureZero(
        digest.value());
    return Result<QString>::success(result);
}

Result<EncryptedEnvelope> BridgeSecurity::seal(
    QByteArrayView plaintext,
    QByteArrayView secret,
    QString senderId,
    quint64 sequence,
    qint64 sentAtMilliseconds)
{
    const auto secretValidation =
        validateRelaySecret(secret);
    if (!secretValidation.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(secretValidation.error());
    }
    auto nonce = WindowsCrypto::randomBytes(
        kEnvelopeNonceBytes);
    if (!nonce.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(nonce.error());
    }
    return sealWithNonce(
        plaintext,
        secret,
        std::move(senderId),
        sequence,
        sentAtMilliseconds,
        std::move(nonce.value()));
}

Result<EncryptedEnvelope>
BridgeSecurity::sealWithNonce(
    QByteArrayView plaintext,
    QByteArrayView secret,
    QString senderId,
    quint64 sequence,
    qint64 sentAtMilliseconds,
    QByteArray nonce)
{
    const auto secretValidation =
        validateRelaySecret(secret);
    if (!secretValidation.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(secretValidation.error());
    }
    if (nonce.size()
        != kEnvelopeNonceBytes) {
        return Result<EncryptedEnvelope>::
            failure(
                securityError(
                    QStringLiteral(
                        "mobile.security.invalid_nonce"),
                    QStringLiteral(
                        "Companion relay nonces must contain exactly 12 bytes.")));
    }

    const auto channel = channelId(secret);
    if (!channel.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(channel.error());
    }
    EncryptedEnvelope envelope{
        envelopeVersion,
        channel.value(),
        std::move(senderId),
        sequence,
        sentAtMilliseconds,
        {},
    };
    const auto authenticatedData =
        envelopeAuthenticationData(
            envelope);
    if (!authenticatedData.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(authenticatedData.error());
    }
    auto encrypted =
        WindowsCrypto::
            chacha20Poly1305Seal(
                plaintext,
                secret,
                nonce,
                authenticatedData.value());
    if (!encrypted.hasValue()) {
        return Result<EncryptedEnvelope>::
            failure(encrypted.error());
    }

    envelope.sealedPayload.reserve(
        nonce.size()
        + encrypted.value()
              .ciphertext.size()
        + encrypted.value().tag.size());
    envelope.sealedPayload.append(nonce);
    envelope.sealedPayload.append(
        encrypted.value().ciphertext);
    envelope.sealedPayload.append(
        encrypted.value().tag);
    WindowsCrypto::secureZero(
        encrypted.value().ciphertext);
    WindowsCrypto::secureZero(
        encrypted.value().tag);
    return Result<EncryptedEnvelope>::success(
        std::move(envelope));
}

Result<QByteArray> BridgeSecurity::open(
    const EncryptedEnvelope& envelope,
    QByteArrayView secret)
{
    const auto secretValidation =
        validateRelaySecret(secret);
    if (!secretValidation.hasValue()) {
        return Result<QByteArray>::failure(
            secretValidation.error());
    }
    const auto expectedChannel =
        channelId(secret);
    if (!expectedChannel.hasValue()) {
        return Result<QByteArray>::failure(
            expectedChannel.error());
    }
    if (envelope.version != envelopeVersion
        || envelope.channelId
            != expectedChannel.value()) {
        return Result<QByteArray>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.invalid_channel"),
                QStringLiteral(
                    "The Companion relay envelope channel is invalid.")));
    }
    if (envelope.sealedPayload.size()
        < kEnvelopeMinimumBytes) {
        return Result<QByteArray>::failure(
            securityError(
                QStringLiteral(
                    "mobile.security.truncated_envelope"),
                QStringLiteral(
                    "The Companion relay envelope is truncated.")));
    }

    const qsizetype ciphertextLength =
        envelope.sealedPayload.size()
        - kEnvelopeMinimumBytes;
    const QByteArrayView nonce(
        envelope.sealedPayload.constData(),
        kEnvelopeNonceBytes);
    const QByteArrayView ciphertext(
        envelope.sealedPayload.constData()
            + kEnvelopeNonceBytes,
        ciphertextLength);
    const QByteArrayView tag(
        envelope.sealedPayload.constData()
            + kEnvelopeNonceBytes
            + ciphertextLength,
        kEnvelopeTagBytes);
    const auto authenticatedData =
        envelopeAuthenticationData(
            envelope);
    if (!authenticatedData.hasValue()) {
        return Result<QByteArray>::failure(
            authenticatedData.error());
    }
    return WindowsCrypto::
        chacha20Poly1305Open(
            ciphertext,
            tag,
            secret,
            nonce,
            authenticatedData.value());
}

std::optional<QString>
BridgeSecurity::normalizedPairingCode(
    const QString& code)
{
    QString numbers;
    numbers.reserve(code.size());
    QTextBoundaryFinder finder(
        QTextBoundaryFinder::Grapheme,
        code);
    finder.toStart();
    qsizetype start = 0;
    while (true) {
        const qsizetype end =
            finder.toNextBoundary();
        if (end < 0) {
            break;
        }
        const QStringView grapheme =
            QStringView(code).sliced(
                start,
                end - start);
        if (QChar::isNumber(
                firstScalar(grapheme))) {
            numbers.append(grapheme);
        }
        start = end;
    }
    if (numbers.isEmpty()) {
        return std::nullopt;
    }
    return numbers;
}

Result<QString>
BridgeSecurity::randomPairingCode()
{
    constexpr quint32 kCodeCount = 1000000;
    constexpr quint32 kLimit =
        std::numeric_limits<quint32>::max()
        - (std::numeric_limits<quint32>::max()
           % kCodeCount);

    while (true) {
        auto bytes =
            WindowsCrypto::randomBytes(
                sizeof(quint32));
        if (!bytes.hasValue()) {
            return Result<QString>::failure(
                bytes.error());
        }
        const unsigned char* source =
            reinterpret_cast<
                const unsigned char*>(
                bytes.value().constData());
        const quint32 value =
            static_cast<quint32>(source[0])
            | (static_cast<quint32>(
                   source[1])
               << 8)
            | (static_cast<quint32>(
                   source[2])
               << 16)
            | (static_cast<quint32>(
                   source[3])
               << 24);
        WindowsCrypto::secureZero(
            bytes.value());
        if (value < kLimit) {
            return Result<QString>::success(
                QStringLiteral("%1")
                    .arg(
                        value % kCodeCount,
                        6,
                        10,
                        QLatin1Char('0')));
        }
    }
}

Result<QByteArray> BridgeSecurity::randomSecret()
{
    return WindowsCrypto::randomBytes(
        kRelaySecretBytes);
}

Result<QByteArray>
BridgeSecurity::randomInvitationNonce()
{
    return WindowsCrypto::randomBytes(
        kInvitationNonceBytes);
}

Result<QByteArray>
BridgeSecurity::invitationAuthenticationData(
    const BridgeInvitation& invitation)
{
    QByteArray output;
    appendInt64BigEndian(
        output,
        invitation.version);
    const QByteArray deviceId =
        invitation.deviceId.toUtf8();
    const auto deviceResult =
        appendLengthPrefixed(
            output,
            deviceId);
    if (!deviceResult.hasValue()) {
        return Result<QByteArray>::failure(
            deviceResult.error());
    }
    const QByteArray displayName =
        invitation.displayName.toUtf8();
    const auto displayResult =
        appendLengthPrefixed(
            output,
            displayName);
    if (!displayResult.hasValue()) {
        return Result<QByteArray>::failure(
            displayResult.error());
    }
    appendInt64BigEndian(
        output,
        invitation.issuedAtMilliseconds);
    const auto nonceResult =
        appendLengthPrefixed(
            output,
            invitation.nonce);
    if (!nonceResult.hasValue()) {
        return Result<QByteArray>::failure(
            nonceResult.error());
    }
    return Result<QByteArray>::success(
        std::move(output));
}

Result<QByteArray>
BridgeSecurity::envelopeAuthenticationData(
    const EncryptedEnvelope& envelope)
{
    QByteArray output;
    output.reserve(
        96
        + envelope.channelId.size()
        + envelope.senderId.size());
    output.append(
        "{\"channelID\":");
    output.append(
        jsonString(envelope.channelId));
    output.append(
        ",\"senderID\":");
    output.append(
        jsonString(envelope.senderId));
    output.append(
        ",\"sentAtMilliseconds\":");
    output.append(
        QByteArray::number(
            envelope.sentAtMilliseconds));
    output.append(
        ",\"sequence\":");
    output.append(
        QByteArray::number(
            envelope.sequence));
    output.append(
        ",\"version\":");
    output.append(
        QByteArray::number(
            envelope.version));
    output.append('}');
    return Result<QByteArray>::success(
        std::move(output));
}

} // namespace companion
