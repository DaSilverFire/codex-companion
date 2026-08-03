#include "core/ChatCredentialService.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cctype>
#include <utility>

namespace {

class SensitiveBytes final {
public:
    explicit SensitiveBytes(
        QByteArray bytes)
        : bytes_(std::move(bytes))
    {
    }

    ~SensitiveBytes()
    {
        if (!bytes_.isEmpty()) {
            SecureZeroMemory(
                const_cast<char*>(
                    bytes_.constData()),
                static_cast<SIZE_T>(
                    bytes_.size()));
            bytes_.clear();
        }
    }

    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(
        const SensitiveBytes&) = delete;

    const QByteArray& bytes() const noexcept
    {
        return bytes_;
    }

private:
    QByteArray bytes_;
};

bool hasNonWhitespaceByte(
    const QByteArray& bytes) noexcept
{
    for (const char byte : bytes) {
        if (!std::isspace(
                static_cast<unsigned char>(
                    byte))) {
            return true;
        }
    }
    return false;
}

companion::CompanionError
credentialServiceError(
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

} // namespace

namespace companion {

QString ChatCredentialService::serviceName(
    ChatCredentialKind kind)
{
    switch (kind) {
    case ChatCredentialKind::OpenAI:
        return QStringLiteral(
            "companion.openai-api-key");
    case ChatCredentialKind::Lumo:
        return QStringLiteral(
            "companion.lumo-api-key");
    }
    return {};
}

bool ChatCredentialService::hasUsableCredential(
    const CredentialStore& store,
    ChatCredentialKind kind) noexcept
{
    const QString service = serviceName(kind);
    if (service.isEmpty()) {
        return false;
    }

    try {
        auto loaded = store.read(service);
        if (!loaded.hasValue()) {
            return false;
        }
        SensitiveBytes secret(
            std::move(loaded.value()));
        return hasNonWhitespaceByte(
            secret.bytes());
    } catch (...) {
        return false;
    }
}

Result<void> ChatCredentialService::save(
    CredentialStore& store,
    ChatCredentialKind kind,
    const QString& secret)
{
    const QString service = serviceName(kind);
    if (service.isEmpty()) {
        return Result<void>::failure(
            credentialServiceError(
                QStringLiteral(
                    "credential.provider_invalid"),
                QStringLiteral(
                    "The chat credential provider is invalid.")));
    }

    QString normalized = secret.trimmed();
    if (normalized.isEmpty()) {
        return Result<void>::failure(
            credentialServiceError(
                QStringLiteral(
                    "credential.empty_secret"),
                QStringLiteral(
                    "Enter an API key before saving.")));
    }

    SensitiveBytes bytes(normalized.toUtf8());
    normalized.fill(QChar(u'\0'));
    normalized.clear();
    return store.write(
        service,
        bytes.bytes());
}

Result<void> ChatCredentialService::remove(
    CredentialStore& store,
    ChatCredentialKind kind)
{
    const QString service = serviceName(kind);
    if (service.isEmpty()) {
        return Result<void>::failure(
            credentialServiceError(
                QStringLiteral(
                    "credential.provider_invalid"),
                QStringLiteral(
                    "The chat credential provider is invalid.")));
    }
    return store.remove(service);
}

} // namespace companion
