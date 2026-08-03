#include "codex/chat/ChatCatalog.h"

#include <algorithm>
#include <utility>

namespace companion {

QVector<BridgeChatAgent> ChatCatalog::agents()
{
    return {
        {
            QStringLiteral("general"),
            QStringLiteral("General"),
            QStringLiteral(
                "Direct answers and everyday help"),
            QStringLiteral("sparkles"),
        },
        {
            QStringLiteral("explain"),
            QStringLiteral("Explain"),
            QStringLiteral(
                "Clear explanations with useful context"),
            QStringLiteral(
                "text.book.closed"),
        },
        {
            QStringLiteral("plan"),
            QStringLiteral("Plan"),
            QStringLiteral(
                "Practical steps and tradeoffs"),
            QStringLiteral("checklist"),
        },
        {
            QStringLiteral("create"),
            QStringLiteral("Create"),
            QStringLiteral(
                "Ideas, drafts, and alternatives"),
            QStringLiteral(
                "wand.and.stars"),
        },
    };
}

ResolvedChatAgent ChatCatalog::resolveAgent(
    const QString& agentId)
{
    const QVector<BridgeChatAgent> builtIns =
        agents();
    const auto match = std::find_if(
        builtIns.cbegin(),
        builtIns.cend(),
        [&agentId](const BridgeChatAgent& agent) {
            return agent.id == agentId;
        });
    const BridgeChatAgent agent =
        match != builtIns.cend()
        ? *match
        : builtIns.front();

    QString promptInstruction;
    if (agent.id == QStringLiteral("explain")) {
        promptInstruction = QStringLiteral(
            "Explain the answer clearly, define unfamiliar terms, and use a short example when useful.");
    } else if (
        agent.id == QStringLiteral("plan")) {
        promptInstruction = QStringLiteral(
            "Turn the request into a practical ordered plan. State important constraints and tradeoffs.");
    } else if (
        agent.id == QStringLiteral("create")) {
        promptInstruction = QStringLiteral(
            "Generate polished ideas or drafts. Offer distinct alternatives when there is more than one good direction.");
    } else {
        promptInstruction = QStringLiteral(
            "Answer directly and concisely.");
    }
    return {
        agent,
        std::move(promptInstruction),
    };
}

QVector<BridgeChatModel>
ChatCatalog::capabilities(
    const ChatCatalogAvailability& availability)
{
    return {
        {
            QStringLiteral("on-device"),
            ChatProvider::OnDevice,
            QStringLiteral("on-device"),
            QStringLiteral("On-device"),
            QStringLiteral(
                "Foundry Local model on this Windows PC"),
            true,
            availability.onDeviceAvailable,
            availability
                .onDeviceSupportsAttachments,
        },
        {
            QStringLiteral("openai:gpt56Luna"),
            ChatProvider::OpenAIAPI,
            QStringLiteral("gpt56Luna"),
            QStringLiteral("5.6 Luna"),
            availability.hasOpenAIKey
                ? QStringLiteral("lowest cost")
                : QStringLiteral(
                      "Add an OpenAI API key on this PC"),
            false,
            availability.hasOpenAIKey,
            false,
        },
        {
            QStringLiteral("openai:gpt56Terra"),
            ChatProvider::OpenAIAPI,
            QStringLiteral("gpt56Terra"),
            QStringLiteral("5.6 Terra"),
            availability.hasOpenAIKey
                ? QStringLiteral("balanced")
                : QStringLiteral(
                      "Add an OpenAI API key on this PC"),
            false,
            availability.hasOpenAIKey,
            false,
        },
        {
            QStringLiteral("openai:gpt56Sol"),
            ChatProvider::OpenAIAPI,
            QStringLiteral("gpt56Sol"),
            QStringLiteral("5.6 Sol"),
            availability.hasOpenAIKey
                ? QStringLiteral(
                      "highest capability")
                : QStringLiteral(
                      "Add an OpenAI API key on this PC"),
            false,
            availability.hasOpenAIKey,
            false,
        },
        {
            QStringLiteral("lumo:automatic"),
            ChatProvider::LumoAPI,
            QStringLiteral("automatic"),
            QStringLiteral("Lumo Auto"),
            availability.hasLumoKey
                ? QStringLiteral(
                      "best available model")
                : QStringLiteral(
                      "Add a Lumo API key on this PC"),
            false,
            availability.hasLumoKey,
            false,
        },
        {
            QStringLiteral("lumo:fast"),
            ChatProvider::LumoAPI,
            QStringLiteral("fast"),
            QStringLiteral("Lumo Fast"),
            availability.hasLumoKey
                ? QStringLiteral("fast responses")
                : QStringLiteral(
                      "Add a Lumo API key on this PC"),
            false,
            availability.hasLumoKey,
            false,
        },
        {
            QStringLiteral("lumo:thinking"),
            ChatProvider::LumoAPI,
            QStringLiteral("thinking"),
            QStringLiteral("Lumo Thinking"),
            availability.hasLumoKey
                ? QStringLiteral(
                      "deeper reasoning")
                : QStringLiteral(
                      "Add a Lumo API key on this PC"),
            false,
            availability.hasLumoKey,
            false,
        },
    };
}

} // namespace companion
