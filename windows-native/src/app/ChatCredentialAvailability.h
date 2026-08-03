#pragma once

#include <QString>

#include <memory>

namespace companion {

class CompanionShellViewModel;
class CredentialStore;

class ChatCredentialAvailability final {
public:
    static bool refresh(
        CompanionShellViewModel& shellViewModel,
        const std::shared_ptr<CredentialStore>&
            credentialStore,
        const QString& modelId);
};

} // namespace companion
