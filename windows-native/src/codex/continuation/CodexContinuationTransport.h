#pragma once

#include "codex/accounts/CodexAccountRouter.h"
#include "codex/continuation/CodexQuotaInterruption.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/BridgeModels.h"
#include "codex/models/ThreadRuntimeStatus.h"
#include "core/Result.h"

#include <QString>
#include <QUuid>

#include <functional>
#include <optional>
#include <stop_token>

namespace companion {

struct CodexContinuationCommands final {
    std::function<
        Result<
            std::optional<
                CodexQuotaInterruption>>(
            const CodexAccountRoute&,
            QString,
            std::stop_token)>
        readQuota;
    std::function<
        Result<BridgeUsageSnapshot>(
            const CodexAccountRoute&,
            std::stop_token)>
        readUsage;
    std::function<
        Result<
            std::optional<BridgeGoal>>(
            const CodexAccountRoute&,
            QString,
            std::stop_token)>
        readGoal;
    std::function<
        Result<BridgeGoal>(
            const CodexAccountRoute&,
            QString,
            std::stop_token)>
        activateGoal;
    std::function<
        Result<void>(
            QString,
            QString,
            ThreadRuntimeStatus,
            QUuid,
            std::stop_token)>
        handoff;
    std::function<
        Result<void>(
            const CodexAccountRoute&,
            QString,
            QString,
            QString,
            std::stop_token)>
        send;
};

CodexContinuationCommands
createProductionCodexContinuationCommands(
    const CodexEnvironment& environment,
    CodexAccountRouter& router);

} // namespace companion
