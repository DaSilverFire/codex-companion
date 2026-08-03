#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "codex/chat/ChatCatalog.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <functional>
#include <stop_token>

namespace companion {

using CapabilityRpcPerformer =
    std::function<Result<QHash<int, RpcResponse>>(
        const QVector<RpcRequest>&,
        std::stop_token)>;
using CapabilityCredentialProbe =
    std::function<bool(ChatProvider)>;
using CapabilityChatAvailabilityProvider =
    std::function<ChatCatalogAvailability()>;

class CapabilityService final {
public:
    explicit CapabilityService(
        const CodexEnvironment& environment,
        CapabilityChatAvailabilityProvider
            availabilityProvider);

    CapabilityService(
        CapabilityRpcPerformer performer,
        CapabilityChatAvailabilityProvider
            availabilityProvider);

    CapabilityService(
        const CodexEnvironment& environment,
        CapabilityCredentialProbe credentialProbe);

    CapabilityService(
        CapabilityRpcPerformer performer,
        CapabilityCredentialProbe credentialProbe);

    Result<BridgeCapabilities> load(
        const QString& cwd,
        std::stop_token stopToken = {}) const;

private:
    CapabilityRpcPerformer performer_;
    CapabilityChatAvailabilityProvider
        availabilityProvider_;
};

} // namespace companion
