#include "UpdateManifestSigner.h"

#include "update/MonocypherEd25519Verifier.h"
#include "update/ReleaseVersion.h"
#include "update/UpdateManifestValidator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

#include "monocypher-ed25519.h"
#include "monocypher.h"

namespace companion {
namespace {

CompanionError signerError(
    QString code,
    QString message)
{
    return {
        std::move(code),
        std::move(message),
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
                static_cast<std::size_t>(
                    bytes_.size()));
        }
    }

    WipedByteArray(const WipedByteArray&) = delete;
    WipedByteArray& operator=(
        const WipedByteArray&) = delete;

    QByteArray& bytes() noexcept
    {
        return bytes_;
    }

private:
    QByteArray bytes_;
};

class WipedSecretKey final {
public:
    ~WipedSecretKey()
    {
        crypto_wipe(
            bytes.data(),
            bytes.size());
    }

    std::array<std::uint8_t, 64> bytes{};
};

QByteArray::FromBase64Result decodeBase64(
    QByteArrayView value)
{
    return QByteArray::fromBase64Encoding(
        value.toByteArray(),
        QByteArray::AbortOnBase64DecodingErrors);
}

bool isStrictWindowsReleaseVersion(
    QStringView value)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)"));
    const QRegularExpressionMatch match =
        pattern.matchView(value);
    if (!match.hasMatch()
        || !ReleaseVersion::parse(value).has_value()) {
        return false;
    }

    for (int index = 1; index <= 3; ++index) {
        bool ok = false;
        const qulonglong component =
            match.capturedView(index)
                .toULongLong(&ok);
        if (!ok || component > 65'535ULL) {
            return false;
        }
    }
    return true;
}

QByteArray quotedJsonString(
    QStringView value)
{
    QJsonArray array;
    array.append(value.toString());
    const QByteArray encoded =
        QJsonDocument(array).toJson(
            QJsonDocument::Compact);
    return encoded.mid(1, encoded.size() - 2);
}

QByteArray encodeManifest(
    const UpdateManifest& manifest)
{
    QByteArray json;
    json.reserve(512);
    json.append("{\n");
    json.append("  \"build\": ");
    json.append(QByteArray::number(manifest.build));
    json.append(",\n");
    json.append("  \"downloadURL\": ");
    json.append(quotedJsonString(
        manifest.downloadUrl));
    json.append(",\n");
    json.append("  \"minimumSystemVersion\": ");
    json.append(quotedJsonString(
        manifest.minimumSystemVersion));
    json.append(",\n");
    json.append("  \"publishedAt\": ");
    json.append(quotedJsonString(
        manifest.publishedAt));
    json.append(",\n");
    json.append("  \"schemaVersion\": ");
    json.append(QByteArray::number(
        manifest.schemaVersion));
    json.append(",\n");
    json.append("  \"sha256\": ");
    json.append(quotedJsonString(
        manifest.sha256));
    json.append(",\n");
    json.append("  \"signature\": ");
    json.append(quotedJsonString(
        manifest.signature));
    json.append(",\n");
    json.append("  \"size\": ");
    json.append(QByteArray::number(manifest.size));
    json.append(",\n");
    json.append("  \"version\": ");
    json.append(quotedJsonString(
        manifest.version));
    json.append("\n}\n");
    return json;
}

Result<UpdateManifest> validatedManifest(
    const UpdateManifestSigningRequest& request)
{
    if (!isStrictWindowsReleaseVersion(
            request.version)) {
        return Result<UpdateManifest>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.invalid_version"),
                QStringLiteral(
                    "The Windows release version is invalid.")));
    }
    if (request.build <= 0
        || request.build > 65'535) {
        return Result<UpdateManifest>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.invalid_build"),
                QStringLiteral(
                    "The Windows release build is invalid.")));
    }

    UpdateManifest candidate;
    candidate.schemaVersion = 1;
    candidate.version = request.version;
    candidate.build = request.build;
    candidate.minimumSystemVersion =
        request.minimumSystemVersion;
    candidate.publishedAt = request.publishedAt;
    candidate.downloadUrl = request.downloadUrl;
    candidate.sha256 =
        request.sha256.toLower();
    candidate.size = request.size;
    candidate.signature =
        QString::fromLatin1(
            QByteArray(64, '\0').toBase64());

    return UpdateManifest::decode(
        encodeManifest(candidate));
}

} // namespace

Result<SignedUpdateManifest> signUpdateManifest(
    UpdateManifestSigningRequest request,
    QByteArray privateSeedBase64)
{
    WipedByteArray encodedSeed(
        std::move(privateSeedBase64));
    auto decodedSeedResult =
        decodeBase64(encodedSeed.bytes());
    const bool seedEncodingIsValid =
        static_cast<bool>(decodedSeedResult);
    WipedByteArray seed(
        std::move(decodedSeedResult.decoded));
    if (!seedEncodingIsValid
        || seed.bytes().size() != 32) {
        return Result<SignedUpdateManifest>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.invalid_private_key"),
                QStringLiteral(
                    "The Windows update private seed is invalid.")));
    }

    WipedSecretKey secretKey;
    std::array<std::uint8_t, 32> publicKey{};
    crypto_ed25519_key_pair(
        secretKey.bytes.data(),
        publicKey.data(),
        reinterpret_cast<std::uint8_t*>(
            seed.bytes().data()));

    const QString publicKeyBase64 =
        QString::fromLatin1(
            QByteArray(
                reinterpret_cast<const char*>(
                    publicKey.data()),
                static_cast<qsizetype>(
                    publicKey.size()))
                .toBase64());
    if (!request.expectedPublicKeyBase64.isEmpty()) {
        const auto expectedResult =
            decodeBase64(
                request.expectedPublicKeyBase64
                    .toLatin1());
        if (!expectedResult
            || expectedResult.decoded.size() != 32
            || expectedResult.decoded
                != QByteArray(
                    reinterpret_cast<const char*>(
                        publicKey.data()),
                    static_cast<qsizetype>(
                        publicKey.size()))) {
            return Result<SignedUpdateManifest>::failure(
                signerError(
                    QStringLiteral(
                        "update.signer.public_key_mismatch"),
                    QStringLiteral(
                        "The expected Windows update public key does not match the private seed.")));
        }
    }

    Result<UpdateManifest> validated =
        validatedManifest(request);
    if (!validated.hasValue()) {
        return Result<SignedUpdateManifest>::failure(
            validated.error());
    }
    UpdateManifest manifest =
        std::move(validated.value());

    const QByteArray canonical =
        manifest.canonicalPayload();
    std::array<std::uint8_t, 64> signature{};
    crypto_ed25519_sign(
        signature.data(),
        secretKey.bytes.data(),
        reinterpret_cast<const std::uint8_t*>(
            canonical.constData()),
        static_cast<std::size_t>(
            canonical.size()));
    manifest.signature =
        QString::fromLatin1(
            QByteArray(
                reinterpret_cast<const char*>(
                    signature.data()),
                static_cast<qsizetype>(
                    signature.size()))
                .toBase64());

    const QByteArray json =
        encodeManifest(manifest);
    const MonocypherEd25519Verifier verifier;
    const UpdateManifestValidator validator(verifier);
    const auto selfVerified =
        validator.validate(
            json,
            publicKeyBase64);
    if (!selfVerified.hasValue()) {
        return Result<SignedUpdateManifest>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.self_verification_failed"),
                QStringLiteral(
                    "The generated Windows update manifest did not pass production verification.")));
    }

    return Result<SignedUpdateManifest>::success(
        {
            std::move(manifest),
            json,
            publicKeyBase64,
        });
}

Result<void> writeSignedUpdateManifest(
    QStringView outputPath,
    QByteArrayView manifestJson)
{
    if (outputPath.isEmpty()) {
        return Result<void>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.invalid_output"),
                QStringLiteral(
                    "The manifest output path is invalid.")));
    }

    QSaveFile file(outputPath.toString());
    if (!file.open(QIODevice::WriteOnly)
        || file.write(
               manifestJson.data(),
               manifestJson.size())
            != manifestJson.size()
        || !file.commit()) {
        return Result<void>::failure(
            signerError(
                QStringLiteral(
                    "update.signer.write_failed"),
                QStringLiteral(
                    "The signed Windows update manifest could not be written.")));
    }
    return Result<void>::success();
}

} // namespace companion
