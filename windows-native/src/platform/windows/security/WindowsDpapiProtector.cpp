#include "platform/windows/security/WindowsDpapiProtector.h"

#include "platform/windows/security/WindowsCrypto.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dpapi.h>

#include <limits>
#include <optional>
#include <utility>

namespace companion {
namespace {

class LocalBlob final {
public:
    explicit LocalBlob(DATA_BLOB blob = {})
        : blob_(blob)
    {
    }

    ~LocalBlob()
    {
        if (blob_.pbData != nullptr) {
            SecureZeroMemory(
                blob_.pbData,
                blob_.cbData);
            LocalFree(blob_.pbData);
        }
    }

    LocalBlob(const LocalBlob&) = delete;
    LocalBlob& operator=(
        const LocalBlob&) = delete;

    const DATA_BLOB& get() const noexcept
    {
        return blob_;
    }

private:
    DATA_BLOB blob_{};
};

class LocalDescription final {
public:
    ~LocalDescription()
    {
        if (value_ != nullptr) {
            LocalFree(value_);
        }
    }

    LocalDescription(const LocalDescription&) =
        delete;
    LocalDescription& operator=(
        const LocalDescription&) = delete;

    LocalDescription() = default;

    LPWSTR* put() noexcept
    {
        return &value_;
    }

private:
    LPWSTR value_ = nullptr;
};

CompanionError dpapiError(
    QString code,
    QString message,
    QString operation,
    DWORD windowsError = ERROR_SUCCESS)
{
    QVariantMap context{
        {QStringLiteral("operation"),
         std::move(operation)},
    };
    if (windowsError != ERROR_SUCCESS) {
        context.insert(
            QStringLiteral("windowsError"),
            static_cast<qulonglong>(
                windowsError));
    }
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

std::optional<DWORD> byteCount(
    qsizetype size)
{
    if (size < 0
        || static_cast<quint64>(size)
            > std::numeric_limits<DWORD>::max()) {
        return std::nullopt;
    }
    return static_cast<DWORD>(size);
}

DATA_BLOB dataBlob(
    QByteArray& bytes)
{
    return {
        static_cast<DWORD>(bytes.size()),
        bytes.isEmpty()
            ? nullptr
            : reinterpret_cast<BYTE*>(
                  bytes.data()),
    };
}

Result<QByteArray> transform(
    QByteArrayView input,
    QByteArrayView entropy,
    bool protect)
{
    const auto inputLength =
        byteCount(input.size());
    const auto entropyLength =
        byteCount(entropy.size());
    if (!inputLength.has_value()
        || !entropyLength.has_value()) {
        return Result<QByteArray>::failure(
            dpapiError(
                QStringLiteral(
                    "mobile.dpapi.input_too_large"),
                QStringLiteral(
                    "The pairing secret is too large for Windows protection."),
                protect
                    ? QStringLiteral(
                          "CryptProtectData")
                    : QStringLiteral(
                          "CryptUnprotectData")));
    }

    QByteArray inputCopy(
        input.data(),
        input.size());
    QByteArray entropyCopy(
        entropy.data(),
        entropy.size());
    DATA_BLOB inputBlob =
        dataBlob(inputCopy);
    DATA_BLOB entropyBlob =
        dataBlob(entropyCopy);
    DATA_BLOB outputBlob{};
    LocalDescription description;

    const BOOL succeeded = protect
        ? CryptProtectData(
              &inputBlob,
              L"Codex Companion paired-device secret",
              entropy.isEmpty()
                  ? nullptr
                  : &entropyBlob,
              nullptr,
              nullptr,
              CRYPTPROTECT_UI_FORBIDDEN,
              &outputBlob)
        : CryptUnprotectData(
              &inputBlob,
              description.put(),
              entropy.isEmpty()
                  ? nullptr
                  : &entropyBlob,
              nullptr,
              nullptr,
              CRYPTPROTECT_UI_FORBIDDEN,
              &outputBlob);
    const DWORD windowsError =
        succeeded ? ERROR_SUCCESS : GetLastError();
    WindowsCrypto::secureZero(inputCopy);
    WindowsCrypto::secureZero(entropyCopy);

    if (!succeeded) {
        return Result<QByteArray>::failure(
            dpapiError(
                protect
                    ? QStringLiteral(
                          "mobile.dpapi.protect_failed")
                    : QStringLiteral(
                          "mobile.dpapi.unprotect_failed"),
                protect
                    ? QStringLiteral(
                          "Windows could not protect the paired-device secret.")
                    : QStringLiteral(
                          "Windows could not unlock the paired-device secret."),
                protect
                    ? QStringLiteral(
                          "CryptProtectData")
                    : QStringLiteral(
                          "CryptUnprotectData"),
                windowsError));
    }

    LocalBlob output(outputBlob);
    QByteArray result(
        reinterpret_cast<const char*>(
            output.get().pbData),
        static_cast<qsizetype>(
            output.get().cbData));
    return Result<QByteArray>::success(
        std::move(result));
}

} // namespace

Result<QByteArray>
WindowsDpapiProtector::protect(
    QByteArrayView plaintext,
    QByteArrayView entropy) const
{
    return transform(
        plaintext,
        entropy,
        true);
}

Result<QByteArray>
WindowsDpapiProtector::unprotect(
    QByteArrayView protectedData,
    QByteArrayView entropy) const
{
    return transform(
        protectedData,
        entropy,
        false);
}

} // namespace companion
