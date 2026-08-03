#pragma once

#include "core/CredentialStore.h"

#include <QString>

namespace companion {

enum class ChatCredentialKind {
    OpenAI,
    Lumo,
};

class ChatCredentialService final {
public:
    static QString serviceName(
        ChatCredentialKind kind);
    static bool hasUsableCredential(
        const CredentialStore& store,
        ChatCredentialKind kind) noexcept;
    static Result<void> save(
        CredentialStore& store,
        ChatCredentialKind kind,
        const QString& secret);
    static Result<void> remove(
        CredentialStore& store,
        ChatCredentialKind kind);
};

} // namespace companion
