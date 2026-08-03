#include "update/MonocypherEd25519Verifier.h"

#include <cstddef>
#include <cstdint>
#include <utility>

#include <QByteArray>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace companion {
namespace {

CompanionError invalidPublicKey()
{
    return {
        QStringLiteral("update.invalid_public_key"),
        QStringLiteral("The update signing key is invalid."),
    };
}

CompanionError invalidSignature()
{
    return {
        QStringLiteral("update.invalid_signature"),
        QStringLiteral("The update manifest signature is invalid."),
    };
}

class WipedByteArray final {
public:
    explicit WipedByteArray(QByteArray bytes)
        : bytes_(std::move(bytes))
    {
    }

    ~WipedByteArray()
    {
        if (!bytes_.isEmpty()) {
            crypto_wipe(
                bytes_.data(),
                static_cast<std::size_t>(bytes_.size()));
        }
    }

    WipedByteArray(const WipedByteArray&) = delete;
    WipedByteArray& operator=(const WipedByteArray&) = delete;

    QByteArray& bytes() noexcept { return bytes_; }
    const QByteArray& bytes() const noexcept { return bytes_; }

private:
    QByteArray bytes_;
};

QByteArray::FromBase64Result decodeBase64(QStringView value)
{
    return QByteArray::fromBase64Encoding(
        value.toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
}

} // namespace

Result<void> MonocypherEd25519Verifier::verify(
    QByteArrayView message,
    QStringView signatureBase64,
    QStringView publicKeyBase64) const
{
    auto decodedPublicKey = decodeBase64(publicKeyBase64);
    const bool publicKeyEncodingIsValid =
        static_cast<bool>(decodedPublicKey);
    WipedByteArray publicKey(
        std::move(decodedPublicKey.decoded));
    if (!publicKeyEncodingIsValid
        || publicKey.bytes().size() != 32) {
        return Result<void>::failure(invalidPublicKey());
    }

    auto decodedSignature = decodeBase64(signatureBase64);
    const bool signatureEncodingIsValid =
        static_cast<bool>(decodedSignature);
    WipedByteArray signature(
        std::move(decodedSignature.decoded));
    if (!signatureEncodingIsValid
        || signature.bytes().size() != 64) {
        return Result<void>::failure(invalidSignature());
    }
    WipedByteArray canonicalPayload(
        QByteArray(message.data(), message.size()));

    const int status = crypto_ed25519_check(
        reinterpret_cast<const std::uint8_t*>(
            signature.bytes().constData()),
        reinterpret_cast<const std::uint8_t*>(
            publicKey.bytes().constData()),
        reinterpret_cast<const std::uint8_t*>(
            canonicalPayload.bytes().constData()),
        static_cast<std::size_t>(
            canonicalPayload.bytes().size()));
    if (status != 0) {
        return Result<void>::failure(invalidSignature());
    }
    return Result<void>::success();
}

} // namespace companion
