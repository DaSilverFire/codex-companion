#include "app/ChatCredentialAvailability.h"

#include "codex/chat/ChatService.h"
#include "ui/CompanionShellViewModel.h"

namespace companion {

bool ChatCredentialAvailability::refresh(
    CompanionShellViewModel& shellViewModel,
    const std::shared_ptr<CredentialStore>&
        credentialStore,
    const QString& modelId)
{
    const bool openAI =
        modelId.startsWith(
            QStringLiteral("openai:"));
    const bool lumo =
        modelId.startsWith(
            QStringLiteral("lumo:"));
    if (!openAI && !lumo) {
        return false;
    }

    const ChatProvider provider = openAI
        ? ChatProvider::OpenAIAPI
        : ChatProvider::LumoAPI;
    const bool available =
        credentialStore
        && ChatService::hasUsableCredential(
            *credentialStore,
            provider);
    shellViewModel.setChatStatus(
        true,
        false,
        false,
        shellViewModel.chatResponse(),
        available
            ? openAI
                ? QStringLiteral(
                      "OpenAI API ready")
                : QStringLiteral(
                      "Lumo API ready")
            : openAI
                ? QStringLiteral(
                      "Add an OpenAI API key in Settings")
                : QStringLiteral(
                      "Add a Lumo API key in Settings"),
        shellViewModel.chatResponsePrompt(),
        shellViewModel.chatResponseTitle(),
        shellViewModel.chatResponseUsageSummary());
    return true;
}

} // namespace companion
