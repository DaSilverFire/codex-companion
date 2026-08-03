#pragma once

#include "codex/accounts/CodexAccountRouter.h"
#include "codex/runtime/CodexRuntimeOperations.h"

#include <QFuture>
#include <QHash>

#include <functional>
#include <optional>
#include <stop_token>

namespace companion {

struct ProfiledCodexControlCommands final {
    std::function<
        QFuture<Result<BridgeUsageSnapshot>>(
            const CodexAccountRoute&)>
        readUsage;
    std::function<
        CommitAwareMutationHandle<
            UsageResetOutcome>(
            const CodexAccountRoute&,
            QString,
            QUuid)>
        consumeUsageReset;
    std::function<
        Result<
            QHash<
                QString,
                std::optional<BridgeGoal>>>(
            const CodexAccountRoute&,
            const QVector<QString>&,
            std::stop_token)>
        readGoals;
    std::function<
        CommitAwareMutationHandle<
            BridgeGoal>(
            const CodexAccountRoute&,
            RuntimeGoalMutationRequest)>
        mutateGoal;
};

class ProfiledCodexControlService final {
public:
    ProfiledCodexControlService(
        const CodexEnvironment& environment,
        CodexAccountRouter& router);
    ProfiledCodexControlService(
        CodexAccountRouter& router,
        ProfiledCodexControlCommands commands);

    QFuture<Result<BridgeUsageSnapshot>>
    readUsage() const;
    CommitAwareMutationHandle<
        UsageResetOutcome>
    consumeUsageReset(
        QString creditId,
        QUuid idempotencyKey) const;
    Result<
        QHash<
            QString,
            std::optional<BridgeGoal>>>
    readGoalsSync(
        const QVector<QString>& threadIds,
        std::stop_token stopToken = {}) const;
    CommitAwareMutationHandle<BridgeGoal>
    mutateGoal(
        RuntimeGoalMutationRequest request) const;

private:
    CodexAccountRouter* router_ = nullptr;
    ProfiledCodexControlCommands commands_;
};

} // namespace companion
