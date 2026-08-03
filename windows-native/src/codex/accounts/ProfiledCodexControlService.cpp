#include "codex/accounts/ProfiledCodexControlService.h"

#include "codex/commands/GoalService.h"
#include "codex/commands/UsageService.h"

#include <QMap>
#include <QPromise>
#include <QSet>

#include <utility>

namespace companion {
namespace {

CompanionError profiledControlError(
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

template <typename T>
QFuture<Result<T>> readyFutureFailure(
    CompanionError error)
{
    QPromise<Result<T>> promise;
    promise.start();
    promise.addResult(
        Result<T>::failure(
            std::move(error)));
    promise.finish();
    return promise.future();
}

template <typename T>
CommitAwareMutationHandle<T>
readyMutationFailure(
    CompanionError error)
{
    const auto mutation =
        CommitAwareMutation<T>::create();
    auto handle = mutation->handle();
    mutation->finish(
        Result<T>::failure(
            std::move(error)));
    return handle;
}

CommitAwareMutationHandle<BridgeGoal>
mutateGoal(
    const GoalService& service,
    RuntimeGoalMutationRequest request)
{
    switch (request.kind) {
    case RuntimeGoalMutationKind::Create:
        if (!request.objective.has_value()) {
            break;
        }
        return service.createMutation(
            request.threadId,
            *request.objective,
            request.tokenBudget);
    case RuntimeGoalMutationKind::Update:
        if (!request.objective.has_value()) {
            break;
        }
        return service.updateMutation({
            request.threadId,
            *request.objective,
            request.tokenBudget,
        });
    case RuntimeGoalMutationKind::Pause:
        return service.pauseMutation(
            request.threadId);
    case RuntimeGoalMutationKind::Resume:
        return service.resumeMutation(
            request.threadId);
    }
    return readyMutationFailure<BridgeGoal>(
        profiledControlError(
            QStringLiteral(
                "codex.account_goal_invalid"),
            QStringLiteral(
                "The routed Codex goal request is invalid.")));
}

QString routeKey(
    const CodexAccountRoute& route)
{
    return route.profileId.has_value()
        ? codexAccountProfileIdString(
              *route.profileId)
        : QString();
}

} // namespace

ProfiledCodexControlService::
    ProfiledCodexControlService(
        const CodexEnvironment& environment,
        CodexAccountRouter& router)
    : ProfiledCodexControlService(
          router,
          ProfiledCodexControlCommands{
              [environment](
                  const CodexAccountRoute&
                      route) {
                  UsageService service(
                      environment,
                      route.environment);
                  return service.read();
              },
              [environment](
                  const CodexAccountRoute&
                      route,
                  QString creditId,
                  QUuid idempotencyKey) {
                  UsageService service(
                      environment,
                      route.environment);
                  return service
                      .consumeResetMutation(
                          creditId,
                          idempotencyKey);
              },
              [environment](
                  const CodexAccountRoute&
                      route,
                  const QVector<QString>&
                      threadIds,
                  std::stop_token
                      stopToken) {
                  GoalService service(
                      environment,
                      route.environment);
                  return service.readSync(
                      threadIds,
                      stopToken);
              },
              [environment](
                  const CodexAccountRoute&
                      route,
                  RuntimeGoalMutationRequest
                      request) {
                  GoalService service(
                      environment,
                      route.environment);
                  return companion::
                      mutateGoal(
                          service,
                          std::move(request));
              },
          })
{
}

ProfiledCodexControlService::
    ProfiledCodexControlService(
        CodexAccountRouter& router,
        ProfiledCodexControlCommands
            commands)
    : router_(&router),
      commands_(std::move(commands))
{
}

QFuture<Result<BridgeUsageSnapshot>>
ProfiledCodexControlService::
readUsage() const
{
    if (router_ == nullptr
        || !commands_.readUsage) {
        return readyFutureFailure<
            BridgeUsageSnapshot>(
            profiledControlError(
                QStringLiteral(
                    "codex.account_usage_unavailable"),
                QStringLiteral(
                    "Routed Codex usage is unavailable.")));
    }
    return commands_.readUsage(
        router_->routeNewWork());
}

CommitAwareMutationHandle<
    UsageResetOutcome>
ProfiledCodexControlService::
consumeUsageReset(
    QString creditId,
    QUuid idempotencyKey) const
{
    if (router_ == nullptr
        || !commands_.consumeUsageReset) {
        return readyMutationFailure<
            UsageResetOutcome>(
            profiledControlError(
                QStringLiteral(
                    "codex.account_usage_unavailable"),
                QStringLiteral(
                    "Routed Codex usage is unavailable.")));
    }
    return commands_.consumeUsageReset(
        router_->routeNewWork(),
        std::move(creditId),
        idempotencyKey);
}

Result<
    QHash<
        QString,
        std::optional<BridgeGoal>>>
ProfiledCodexControlService::
readGoalsSync(
    const QVector<QString>& threadIds,
    std::stop_token stopToken) const
{
    using GoalMap =
        QHash<
            QString,
            std::optional<BridgeGoal>>;
    if (router_ == nullptr
        || !commands_.readGoals) {
        return Result<GoalMap>::failure(
            profiledControlError(
                QStringLiteral(
                    "codex.account_goals_unavailable"),
                QStringLiteral(
                    "Routed Codex goals are unavailable.")));
    }
    if (stopToken.stop_requested()) {
        return Result<GoalMap>::failure(
            profiledControlError(
                QStringLiteral(
                    "codex.operation_canceled"),
                QStringLiteral(
                    "The Codex operation was canceled.")));
    }

    QSet<QString> seen;
    QMap<QString, QVector<QString>>
        groups;
    QHash<QString, CodexAccountRoute>
        routes;
    for (const QString& rawThreadId :
         threadIds) {
        const QString threadId =
            rawThreadId.trimmed();
        if (threadId.isEmpty()
            || seen.contains(threadId)) {
            continue;
        }
        seen.insert(threadId);
        const CodexAccountRoute route =
            router_->routeThread(threadId);
        const QString key =
            routeKey(route);
        groups[key].append(threadId);
        routes.insert(key, route);
    }

    GoalMap combined;
    for (auto iterator =
             groups.cbegin();
         iterator != groups.cend();
         ++iterator) {
        if (stopToken.stop_requested()) {
            return Result<GoalMap>::failure(
                profiledControlError(
                    QStringLiteral(
                        "codex.operation_canceled"),
                    QStringLiteral(
                        "The Codex operation was canceled.")));
        }
        const auto loaded =
            commands_.readGoals(
                routes.value(
                    iterator.key()),
                iterator.value(),
                stopToken);
        if (!loaded.hasValue()) {
            return Result<GoalMap>::failure(
                loaded.error());
        }
        for (auto goal =
                 loaded.value().cbegin();
             goal != loaded.value().cend();
             ++goal) {
            combined.insert(
                goal.key(),
                goal.value());
        }
    }
    return Result<GoalMap>::success(
        std::move(combined));
}

CommitAwareMutationHandle<BridgeGoal>
ProfiledCodexControlService::
mutateGoal(
    RuntimeGoalMutationRequest request) const
{
    if (router_ == nullptr
        || !commands_.mutateGoal) {
        return readyMutationFailure<
            BridgeGoal>(
            profiledControlError(
                QStringLiteral(
                    "codex.account_goals_unavailable"),
                QStringLiteral(
                    "Routed Codex goals are unavailable.")));
    }
    const CodexAccountRoute route =
        router_->routeThread(
            request.threadId);
    return commands_.mutateGoal(
        route,
        std::move(request));
}

} // namespace companion
