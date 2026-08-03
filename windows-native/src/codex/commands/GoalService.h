#pragma once

#include "codex/appserver/AppServerRpcClient.h"
#include "codex/commands/CommitAwareMutation.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/models/BridgeModels.h"
#include "core/Result.h"

#include <QFuture>
#include <QHash>
#include <QProcessEnvironment>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <stop_token>

namespace companion {

namespace detail {
struct GoalServiceTestAccess;
}

struct GoalSetRequest final {
    QString threadId;
    std::optional<QString> objective;
    std::optional<GoalStatus> status;
    std::optional<qint64> tokenBudget;
};

struct GoalUpdateRequest final {
    QString threadId;
    QString objective;
    std::optional<qint64> tokenBudget;
};

using GoalRpcPerformer =
    std::function<Result<QHash<int, RpcResponse>>(
        const QVector<RpcRequest>&)>;
using CancellableGoalRpcPerformer =
    std::function<Result<QHash<int, RpcResponse>>(
        const QVector<RpcRequest>&,
        std::stop_token)>;
using GoalCommitProbe = std::function<void(QString)>;

class GoalService final {
public:
    explicit GoalService(
        const CodexEnvironment& environment,
        QProcessEnvironment processEnvironment =
            QProcessEnvironment::systemEnvironment(),
        int timeoutMilliseconds =
            AppServerRpcClient::kDefaultTimeoutMilliseconds);

    explicit GoalService(
        GoalRpcPerformer performer,
        GoalCommitProbe mutationCommitProbe = {});

    GoalService(
        CancellableGoalRpcPerformer performer,
        GoalCommitProbe mutationCommitProbe = {});

    Result<QHash<QString, std::optional<BridgeGoal>>> readSync(
        const QVector<QString>& threadIds,
        std::stop_token stopToken = {}) const;

    QFuture<Result<QHash<QString, std::optional<BridgeGoal>>>>
    read(const QVector<QString>& threadIds) const;

    CommitAwareMutationHandle<BridgeGoal> createMutation(
        const QString& threadId,
        const QString& objective,
        std::optional<qint64> tokenBudget =
            std::nullopt) const;

    CommitAwareMutationHandle<BridgeGoal> setMutation(
        const GoalSetRequest& request) const;

    CommitAwareMutationHandle<BridgeGoal> pauseMutation(
        const QString& threadId) const;

    CommitAwareMutationHandle<BridgeGoal> resumeMutation(
        const QString& threadId) const;

    CommitAwareMutationHandle<BridgeGoal> updateMutation(
        const GoalUpdateRequest& request) const;

    QFuture<Result<BridgeGoal>> create(
        const QString& threadId,
        const QString& objective,
        std::optional<qint64> tokenBudget =
            std::nullopt) const;

    QFuture<Result<BridgeGoal>> set(
        const GoalSetRequest& request) const;

    QFuture<Result<BridgeGoal>> pause(
        const QString& threadId) const;

    QFuture<Result<BridgeGoal>> resume(
        const QString& threadId) const;

    QFuture<Result<BridgeGoal>> update(
        const GoalUpdateRequest& request) const;

private:
    enum class ReadPhase {
        AfterNormalizeItem,
        AfterRequestBuilt,
        AfterResponseParsed,
    };

    using ReadPhaseProbe =
        std::function<void(ReadPhase, qsizetype)>;

    GoalService(
        CancellableGoalRpcPerformer performer,
        GoalCommitProbe mutationCommitProbe,
        ReadPhaseProbe readPhaseProbe);

    static void probeReadPhase(
        const ReadPhaseProbe& probe,
        ReadPhase phase,
        qsizetype index) noexcept;

    static Result<
        QHash<QString, std::optional<BridgeGoal>>>
    readGoalsSync(
        const CancellableGoalRpcPerformer& performer,
        const ReadPhaseProbe& readPhaseProbe,
        const QVector<QString>& threadIds,
        std::stop_token stopToken);

    friend struct detail::GoalServiceTestAccess;

    CancellableGoalRpcPerformer performer_;
    GoalCommitProbe mutationCommitProbe_;
    ReadPhaseProbe readPhaseProbe_;
};

} // namespace companion
