#pragma once

#include "codex/models/BridgeModels.h"

#include <QVector>

namespace companion {

struct ChatCatalogAvailability final {
    bool onDeviceAvailable = false;
    bool onDeviceSupportsAttachments = false;
    bool hasOpenAIKey = false;
    bool hasLumoKey = false;
};

struct ResolvedChatAgent final {
    BridgeChatAgent agent;
    QString promptInstruction;
};

class ChatCatalog final {
public:
    static QVector<BridgeChatAgent> agents();
    static ResolvedChatAgent resolveAgent(
        const QString& agentId);

    static QVector<BridgeChatModel> capabilities(
        const ChatCatalogAvailability&
            availability);
};

} // namespace companion
