#include "platform/windows/security/WindowsCrypto.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <limits>
#include <optional>
#include <utility>

namespace companion {
namespace {

class AlgorithmHandle final {
public:
    ~AlgorithmHandle()
    {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(
                handle_,
                0);
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) =
        delete;
    AlgorithmHandle& operator=(
        const AlgorithmHandle&) = delete;

    AlgorithmHandle() = default;

    BCRYPT_ALG_HANDLE* put() noexcept
    {
        return &handle_;
    }

    BCRYPT_ALG_HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle final {
public:
    ~HashHandle()
    {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(
        const HashHandle&) = delete;

    HashHandle() = default;

    BCRYPT_HASH_HANDLE* put() noexcept
    {
        return &handle_;
    }

    BCRYPT_HASH_HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

class KeyHandle final {
public:
    ~KeyHandle()
    {
        if (handle_ != nullptr) {
            BCryptDestroyKey(handle_);
        }
    }

    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(
        const KeyHandle&) = delete;

    KeyHandle() = default;

    BCRYPT_KEY_HANDLE* put() noexcept
    {
        return &handle_;
    }

    BCRYPT_KEY_HANDLE get() const noexcept
    {
        return handle_;
    }

private:
    BCRYPT_KEY_HANDLE handle_ = nullptr;
};

class SecureBuffer final {
public:
    explicit SecureBuffer(qsizetype size)
        : bytes_(size, '\0')
    {
    }

    explicit SecureBuffer(QByteArrayView bytes)
        : bytes_(bytes.data(), bytes.size())
    {
    }

    ~SecureBuffer()
    {
        WindowsCrypto::secureZero(bytes_);
    }

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(
        const SecureBuffer&) = delete;

    QByteArray& bytes() noexcept
    {
        return bytes_;
    }

private:
    QByteArray bytes_;
};

CompanionError cryptoError(
    QString code,
    QString message,
    QString operation,
    std::optional<NTSTATUS> status =
        std::nullopt)
{
    QVariantMap context{
        {QStringLiteral("operation"),
         std::move(operation)},
    };
    if (status.has_value()) {
        context.insert(
            QStringLiteral("ntStatus"),
            static_cast<qulonglong>(
                static_cast<ULONG>(
                    *status)));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

std::optional<ULONG> byteCount(
    qsizetype count)
{
    if (count < 0
        || static_cast<quint64>(count)
            > std::numeric_limits<ULONG>::max()) {
        return std::nullopt;
    }
    return static_cast<ULONG>(count);
}

PUCHAR mutableBytes(
    QByteArrayView bytes) noexcept
{
    if (bytes.isEmpty()) {
        return nullptr;
    }
    return reinterpret_cast<PUCHAR>(
        const_cast<char*>(bytes.data()));
}

PUCHAR mutableBytes(
    QByteArray& bytes) noexcept
{
    if (bytes.isEmpty()) {
        return nullptr;
    }
    return reinterpret_cast<PUCHAR>(
        bytes.data());
}

Result<ULONG> unsignedProperty(
    BCRYPT_HANDLE handle,
    LPCWSTR property,
    const QString& operation)
{
    ULONG value = 0;
    ULONG written = 0;
    const NTSTATUS status = BCryptGetProperty(
        handle,
        property,
        reinterpret_cast<PUCHAR>(&value),
        sizeof(value),
        &written,
        0);
    if (status < 0 || written != sizeof(value)) {
        return Result<ULONG>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.property_failed"),
                QStringLiteral(
                    "Windows cryptography did not provide a required property."),
                operation,
                status));
    }
    return Result<ULONG>::success(value);
}

Result<void> validateTagLength(
    BCRYPT_ALG_HANDLE algorithm)
{
    BCRYPT_AUTH_TAG_LENGTHS_STRUCT lengths{};
    ULONG written = 0;
    const NTSTATUS status = BCryptGetProperty(
        algorithm,
        BCRYPT_AUTH_TAG_LENGTH,
        reinterpret_cast<PUCHAR>(&lengths),
        sizeof(lengths),
        &written,
        0);
    if (status < 0
        || written != sizeof(lengths)
        || lengths.dwMinLength > 16
        || lengths.dwMaxLength < 16
        || (lengths.dwIncrement != 0
            && (16 - lengths.dwMinLength)
                % lengths.dwIncrement != 0)) {
        return Result<void>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.tag_length_unsupported"),
                QStringLiteral(
                    "Windows cryptography does not support the required authentication tag."),
                QStringLiteral(
                    "BCryptGetProperty"),
                status));
    }
    return Result<void>::success();
}

Result<void> makeChaChaKey(
    AlgorithmHandle& algorithm,
    KeyHandle& key,
    SecureBuffer& keyObject,
    QByteArrayView secret)
{
    const auto objectLength =
        unsignedProperty(
            algorithm.get(),
            BCRYPT_OBJECT_LENGTH,
            QStringLiteral(
                "BCryptGetProperty"));
    if (!objectLength.hasValue()) {
        return Result<void>::failure(
            objectLength.error());
    }

    keyObject.bytes().resize(
        static_cast<qsizetype>(
            objectLength.value()));
    const auto secretLength =
        byteCount(secret.size());
    if (!secretLength.has_value()) {
        return Result<void>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.input_too_large"),
                QStringLiteral(
                    "The cryptographic input is too large."),
                QStringLiteral(
                    "BCryptGenerateSymmetricKey")));
    }

    const auto objectBytes =
        byteCount(keyObject.bytes().size());
    if (!objectBytes.has_value()) {
        return Result<void>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.input_too_large"),
                QStringLiteral(
                    "The cryptographic key object is too large."),
                QStringLiteral(
                    "BCryptGenerateSymmetricKey")));
    }

    const NTSTATUS status =
        BCryptGenerateSymmetricKey(
            algorithm.get(),
            key.put(),
            mutableBytes(keyObject.bytes()),
            *objectBytes,
            mutableBytes(secret),
            *secretLength,
            0);
    if (status < 0) {
        return Result<void>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.key_failed"),
                QStringLiteral(
                    "Windows cryptography could not create the relay key."),
                QStringLiteral(
                    "BCryptGenerateSymmetricKey"),
                status));
    }
    return Result<void>::success();
}

} // namespace

Result<QByteArray> WindowsCrypto::randomBytes(
    qsizetype count)
{
    const auto nativeCount = byteCount(count);
    if (!nativeCount.has_value()) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.invalid_size"),
                QStringLiteral(
                    "The requested random byte count is invalid."),
                QStringLiteral(
                    "BCryptGenRandom")));
    }

    QByteArray bytes(count, '\0');
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        mutableBytes(bytes),
        *nativeCount,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        secureZero(bytes);
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.random_failed"),
                QStringLiteral(
                    "Windows could not generate secure random data."),
                QStringLiteral(
                    "BCryptGenRandom"),
                status));
    }
    return Result<QByteArray>::success(
        std::move(bytes));
}

Result<QByteArray> WindowsCrypto::hmacSha256(
    QByteArrayView secret,
    QByteArrayView data)
{
    const auto secretLength =
        byteCount(secret.size());
    const auto dataLength =
        byteCount(data.size());
    if (!secretLength.has_value()
        || !dataLength.has_value()) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.input_too_large"),
                QStringLiteral(
                    "The cryptographic input is too large."),
                QStringLiteral(
                    "BCryptHashData")));
    }

    AlgorithmHandle algorithm;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(
            algorithm.put(),
            BCRYPT_SHA256_ALGORITHM,
            nullptr,
            BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.algorithm_unavailable"),
                QStringLiteral(
                    "Windows SHA-256 HMAC is unavailable."),
                QStringLiteral(
                    "BCryptOpenAlgorithmProvider"),
                status));
    }

    const auto objectLength =
        unsignedProperty(
            algorithm.get(),
            BCRYPT_OBJECT_LENGTH,
            QStringLiteral(
                "BCryptGetProperty"));
    if (!objectLength.hasValue()) {
        return Result<QByteArray>::failure(
            objectLength.error());
    }
    const auto hashLength =
        unsignedProperty(
            algorithm.get(),
            BCRYPT_HASH_LENGTH,
            QStringLiteral(
                "BCryptGetProperty"));
    if (!hashLength.hasValue()) {
        return Result<QByteArray>::failure(
            hashLength.error());
    }

    SecureBuffer hashObject(
        static_cast<qsizetype>(
            objectLength.value()));
    HashHandle hash;
    status = BCryptCreateHash(
        algorithm.get(),
        hash.put(),
        mutableBytes(hashObject.bytes()),
        objectLength.value(),
        mutableBytes(secret),
        *secretLength,
        0);
    if (status < 0) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.hmac_failed"),
                QStringLiteral(
                    "Windows could not initialize the invitation authenticator."),
                QStringLiteral(
                    "BCryptCreateHash"),
                status));
    }

    status = BCryptHashData(
        hash.get(),
        mutableBytes(data),
        *dataLength,
        0);
    if (status < 0) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.hmac_failed"),
                QStringLiteral(
                    "Windows could not authenticate the Companion data."),
                QStringLiteral(
                    "BCryptHashData"),
                status));
    }

    QByteArray digest(
        static_cast<qsizetype>(
            hashLength.value()),
        '\0');
    status = BCryptFinishHash(
        hash.get(),
        mutableBytes(digest),
        hashLength.value(),
        0);
    if (status < 0) {
        secureZero(digest);
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.hmac_failed"),
                QStringLiteral(
                    "Windows could not finish the Companion authenticator."),
                QStringLiteral(
                    "BCryptFinishHash"),
                status));
    }
    return Result<QByteArray>::success(
        std::move(digest));
}

Result<AuthenticatedCiphertext>
WindowsCrypto::chacha20Poly1305Seal(
    QByteArrayView plaintext,
    QByteArrayView secret,
    QByteArrayView nonce,
    QByteArrayView authenticatedData)
{
    const auto plaintextLength =
        byteCount(plaintext.size());
    const auto nonceLength =
        byteCount(nonce.size());
    const auto authenticatedLength =
        byteCount(authenticatedData.size());
    if (!plaintextLength.has_value()
        || !nonceLength.has_value()
        || !authenticatedLength.has_value()) {
        return Result<AuthenticatedCiphertext>::
            failure(
                cryptoError(
                    QStringLiteral(
                        "mobile.crypto.input_too_large"),
                    QStringLiteral(
                        "The relay encryption input is too large."),
                    QStringLiteral(
                        "BCryptEncrypt")));
    }

    AlgorithmHandle algorithm;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(
            algorithm.put(),
            BCRYPT_CHACHA20_POLY1305_ALGORITHM,
            nullptr,
            0);
    if (status < 0) {
        return Result<AuthenticatedCiphertext>::
            failure(
                cryptoError(
                    QStringLiteral(
                        "mobile.crypto.algorithm_unavailable"),
                    QStringLiteral(
                        "Windows ChaCha20-Poly1305 is unavailable."),
                    QStringLiteral(
                        "BCryptOpenAlgorithmProvider"),
                    status));
    }
    const auto tagSupport =
        validateTagLength(algorithm.get());
    if (!tagSupport.hasValue()) {
        return Result<AuthenticatedCiphertext>::
            failure(tagSupport.error());
    }

    SecureBuffer keyObject(0);
    KeyHandle key;
    const auto keyResult =
        makeChaChaKey(
            algorithm,
            key,
            keyObject,
            secret);
    if (!keyResult.hasValue()) {
        return Result<AuthenticatedCiphertext>::
            failure(keyResult.error());
    }

    AuthenticatedCiphertext result{
        QByteArray(plaintext.size(), '\0'),
        QByteArray(16, '\0'),
    };
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mutableBytes(nonce);
    info.cbNonce = *nonceLength;
    info.pbAuthData =
        mutableBytes(authenticatedData);
    info.cbAuthData = *authenticatedLength;
    info.pbTag = mutableBytes(result.tag);
    info.cbTag = 16;

    ULONG written = 0;
    status = BCryptEncrypt(
        key.get(),
        mutableBytes(plaintext),
        *plaintextLength,
        &info,
        nullptr,
        0,
        mutableBytes(result.ciphertext),
        *plaintextLength,
        &written,
        0);
    if (status < 0
        || written != *plaintextLength) {
        secureZero(result.ciphertext);
        secureZero(result.tag);
        return Result<AuthenticatedCiphertext>::
            failure(
                cryptoError(
                    QStringLiteral(
                        "mobile.crypto.encrypt_failed"),
                    QStringLiteral(
                        "Windows could not encrypt the Companion relay payload."),
                    QStringLiteral(
                        "BCryptEncrypt"),
                    status));
    }
    return Result<AuthenticatedCiphertext>::
        success(std::move(result));
}

Result<QByteArray>
WindowsCrypto::chacha20Poly1305Open(
    QByteArrayView ciphertext,
    QByteArrayView tag,
    QByteArrayView secret,
    QByteArrayView nonce,
    QByteArrayView authenticatedData)
{
    const auto ciphertextLength =
        byteCount(ciphertext.size());
    const auto nonceLength =
        byteCount(nonce.size());
    const auto authenticatedLength =
        byteCount(authenticatedData.size());
    if (!ciphertextLength.has_value()
        || !nonceLength.has_value()
        || !authenticatedLength.has_value()) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.input_too_large"),
                QStringLiteral(
                    "The relay decryption input is too large."),
                QStringLiteral(
                    "BCryptDecrypt")));
    }

    AlgorithmHandle algorithm;
    NTSTATUS status =
        BCryptOpenAlgorithmProvider(
            algorithm.put(),
            BCRYPT_CHACHA20_POLY1305_ALGORITHM,
            nullptr,
            0);
    if (status < 0) {
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.algorithm_unavailable"),
                QStringLiteral(
                    "Windows ChaCha20-Poly1305 is unavailable."),
                QStringLiteral(
                    "BCryptOpenAlgorithmProvider"),
                status));
    }
    const auto tagSupport =
        validateTagLength(algorithm.get());
    if (!tagSupport.hasValue()) {
        return Result<QByteArray>::failure(
            tagSupport.error());
    }

    SecureBuffer keyObject(0);
    KeyHandle key;
    const auto keyResult =
        makeChaChaKey(
            algorithm,
            key,
            keyObject,
            secret);
    if (!keyResult.hasValue()) {
        return Result<QByteArray>::failure(
            keyResult.error());
    }

    QByteArray plaintext(
        ciphertext.size(),
        '\0');
    SecureBuffer tagCopy(tag);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = mutableBytes(nonce);
    info.cbNonce = *nonceLength;
    info.pbAuthData =
        mutableBytes(authenticatedData);
    info.cbAuthData = *authenticatedLength;
    info.pbTag =
        mutableBytes(tagCopy.bytes());
    info.cbTag = static_cast<ULONG>(
        tagCopy.bytes().size());

    ULONG written = 0;
    status = BCryptDecrypt(
        key.get(),
        mutableBytes(ciphertext),
        *ciphertextLength,
        &info,
        nullptr,
        0,
        mutableBytes(plaintext),
        *ciphertextLength,
        &written,
        0);
    if (status < 0
        || written != *ciphertextLength) {
        secureZero(plaintext);
        return Result<QByteArray>::failure(
            cryptoError(
                QStringLiteral(
                    "mobile.crypto.authentication_failed"),
                QStringLiteral(
                    "The Companion relay payload could not be authenticated."),
                QStringLiteral(
                    "BCryptDecrypt"),
                status));
    }
    return Result<QByteArray>::success(
        std::move(plaintext));
}

bool WindowsCrypto::constantTimeEquals(
    QByteArrayView left,
    QByteArrayView right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }

    volatile unsigned char difference = 0;
    for (qsizetype index = 0;
         index < left.size();
         ++index) {
        difference = static_cast<unsigned char>(
            difference
            | (static_cast<unsigned char>(
                   left.at(index))
               ^ static_cast<unsigned char>(
                   right.at(index))));
    }
    return difference == 0;
}

void WindowsCrypto::secureZero(
    QByteArray& bytes) noexcept
{
    if (!bytes.isEmpty()) {
        SecureZeroMemory(
            bytes.data(),
            static_cast<SIZE_T>(
                bytes.size()));
    }
    bytes.clear();
}

} // namespace companion
