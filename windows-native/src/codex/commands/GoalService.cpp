#include "codex/commands/GoalService.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QCborValue>
#include <QPromise>
#include <QThreadPool>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace {

template <typename T>
Result<T> goalFailure(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return Result<T>::failure({
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    });
}

template <typename T>
Result<T> goalUnavailable(
    QVariantMap context = {})
{
    return goalFailure<T>(
        QStringLiteral("codex.goal_unavailable"),
        QStringLiteral("Codex goal is unavailable."),
        std::move(context));
}

template <typename T>
Result<T> goalCanceled()
{
    return Result<T>::failure({
        QStringLiteral("codex.operation_canceled"),
        QStringLiteral("The Codex operation was canceled."),
        false,
        {},
    });
}

template <typename T>
struct GoalCompletion final {
    explicit GoalCompletion(
        std::shared_ptr<QPromise<Result<T>>> requestedPromise)
        : promise(std::move(requestedPromise))
    {
    }

    void finish(Result<T> result)
    {
        const std::scoped_lock lock(mutex);
        if (finished) {
            return;
        }
        promise->addResult(std::move(result));
        promise->finish();
        finished = true;
    }

    void cancel()
    {
        const std::scoped_lock lock(mutex);
        if (finished) {
            return;
        }
        promise->finish();
        finished = true;
    }

    std::shared_ptr<QPromise<Result<T>>> promise;
    std::mutex mutex;
    bool finished = false;
};

void probeGoalCommit(
    const GoalCommitProbe& probe,
    const QString& phase) noexcept
{
    if (!probe) {
        return;
    }
    try {
        probe(phase);
    } catch (...) {
    }
}

std::optional<qint64> integerValue(
    const QJsonValue& value)
{
    const QCborValue cbor =
        QCborValue::fromJsonValue(value);
    if (!cbor.isInteger()) {
        return std::nullopt;
    }
    return cbor.toInteger();
}

bool isMissingOrNull(const QJsonValue& value)
{
    return value.isUndefined() || value.isNull();
}

bool requiredString(
    const QJsonObject& object,
    const QString& key,
    QString& out)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return false;
    }
    out = value.toString();
    return true;
}

bool requiredInteger(
    const QJsonObject& object,
    const QString& key,
    qint64& out)
{
    const std::optional<qint64> value =
        integerValue(object.value(key));
    if (!value.has_value()) {
        return false;
    }
    out = *value;
    return true;
}

bool optionalInteger(
    const QJsonObject& object,
    const QString& key,
    std::optional<qint64>& out)
{
    const QJsonValue value = object.value(key);
    if (isMissingOrNull(value)) {
        out = std::nullopt;
        return true;
    }
    const std::optional<qint64> parsed =
        integerValue(value);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

std::optional<GoalStatus> goalStatusFrom(
    const QString& value)
{
    if (value == QStringLiteral("active")) {
        return GoalStatus::Active;
    }
    if (value == QStringLiteral("paused")) {
        return GoalStatus::Paused;
    }
    if (value == QStringLiteral("blocked")) {
        return GoalStatus::Blocked;
    }
    if (value == QStringLiteral("usageLimited")) {
        return GoalStatus::UsageLimited;
    }
    if (value == QStringLiteral("budgetLimited")) {
        return GoalStatus::BudgetLimited;
    }
    if (value == QStringLiteral("complete")) {
        return GoalStatus::Complete;
    }
    return std::nullopt;
}

QString goalStatusText(GoalStatus status)
{
    switch (status) {
    case GoalStatus::Active:
        return QStringLiteral("active");
    case GoalStatus::Paused:
        return QStringLiteral("paused");
    case GoalStatus::Blocked:
        return QStringLiteral("blocked");
    case GoalStatus::UsageLimited:
        return QStringLiteral("usageLimited");
    case GoalStatus::BudgetLimited:
        return QStringLiteral("budgetLimited");
    case GoalStatus::Complete:
        return QStringLiteral("complete");
    }
    return QStringLiteral("active");
}

Result<BridgeGoal> parseGoal(
    const QJsonValue& value)
{
    if (!value.isObject()) {
        return goalFailure<BridgeGoal>(
            QStringLiteral("codex.goal_unavailable"),
            QStringLiteral(
                "Codex app-server returned an unreadable goal."));
    }
    const QJsonObject object = value.toObject();
    QString threadId;
    QString objective;
    QString rawStatus;
    qint64 tokensUsed = 0;
    qint64 elapsedSeconds = 0;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    std::optional<qint64> tokenBudget;
    const bool validShape =
        requiredString(
            object,
            QStringLiteral("threadId"),
            threadId)
        && requiredString(
            object,
            QStringLiteral("objective"),
            objective)
        && requiredString(
            object,
            QStringLiteral("status"),
            rawStatus)
        && requiredInteger(
            object,
            QStringLiteral("tokensUsed"),
            tokensUsed)
        && requiredInteger(
            object,
            QStringLiteral("timeUsedSeconds"),
            elapsedSeconds)
        && requiredInteger(
            object,
            QStringLiteral("createdAt"),
            createdAt)
        && requiredInteger(
            object,
            QStringLiteral("updatedAt"),
            updatedAt)
        && optionalInteger(
            object,
            QStringLiteral("tokenBudget"),
            tokenBudget);
    const std::optional<GoalStatus> status =
        goalStatusFrom(rawStatus);
    if (!validShape || !status.has_value()) {
        return goalFailure<BridgeGoal>(
            QStringLiteral("codex.goal_unavailable"),
            QStringLiteral(
                "Codex app-server returned an unreadable goal."));
    }
    BridgeGoal goal;
    goal.threadId = threadId;
    goal.objective = objective;
    goal.status = *status;
    goal.tokenBudget = tokenBudget;
    goal.tokensUsed = tokensUsed;
    goal.elapsedSeconds = elapsedSeconds;
    goal.createdAt = createdAt;
    goal.updatedAt = updatedAt;
    return Result<BridgeGoal>::success(std::move(goal));
}

template <typename T>
Result<T> appServerResponseFailure(
    int id)
{
    return goalUnavailable<T>(
        {{QStringLiteral("id"), id}});
}

Result<QJsonValue> resultFor(
    int id,
    const QHash<int, RpcResponse>& responses)
{
    const auto iterator = responses.constFind(id);
    if (iterator == responses.constEnd()) {
        return appServerResponseFailure<QJsonValue>(
            id);
    }
    if (iterator.value().isError) {
        return appServerResponseFailure<QJsonValue>(
            id);
    }
    return Result<QJsonValue>::success(
        iterator.value().result);
}

Result<BridgeGoal> parseMutationResponse(
    int id,
    const QHash<int, RpcResponse>& responses)
{
    const Result<QJsonValue> raw =
        resultFor(id, responses);
    if (!raw.hasValue()) {
        return Result<BridgeGoal>::failure(raw.error());
    }
    if (!raw.value().isObject()) {
        return goalFailure<BridgeGoal>(
            QStringLiteral("codex.goal_unavailable"),
            QStringLiteral(
                "Codex app-server returned an unreadable goal."));
    }
    return parseGoal(
        raw.value()
            .toObject()
            .value(QStringLiteral("goal")));
}

Result<void> validateThreadId(
    const QString& threadId)
{
    if (threadId.trimmed().isEmpty()) {
        return Result<void>::failure({
            QStringLiteral("codex.goal_thread_empty"),
            QStringLiteral("Choose a Codex task first."),
            false,
            {},
        });
    }
    return Result<void>::success();
}

Result<void> validateObjective(
    const QString& objective)
{
    if (objective.trimmed().isEmpty()) {
        return Result<void>::failure({
            QStringLiteral("codex.goal_objective_empty"),
            QStringLiteral("Enter a goal objective first."),
            false,
            {},
        });
    }
    return Result<void>::success();
}

Result<void> validateBudget(
    std::optional<qint64> budget)
{
    if (budget.has_value() && *budget <= 0) {
        return Result<void>::failure({
            QStringLiteral(
                "codex.goal_token_budget_invalid"),
            QStringLiteral(
                "Goal token budget must be positive."),
            false,
            {},
        });
    }
    return Result<void>::success();
}

RpcRequest goalSetRequest(
    int id,
    const GoalSetRequest& request)
{
    QJsonObject params{
        {
            QStringLiteral("threadId"),
            request.threadId.trimmed(),
        },
    };
    if (request.objective.has_value()) {
        params.insert(
            QStringLiteral("objective"),
            request.objective->trimmed());
    }
    if (request.status.has_value()) {
        params.insert(
            QStringLiteral("status"),
            goalStatusText(*request.status));
    }
    if (request.tokenBudget.has_value()) {
        params.insert(
            QStringLiteral("tokenBudget"),
            *request.tokenBudget);
    }
    return {
        id,
        QStringLiteral("thread/goal/set"),
        params,
    };
}

CancellableGoalRpcPerformer cancellablePerformer(
    GoalRpcPerformer performer)
{
    if (!performer) {
        return {};
    }
    return [performer = std::move(performer)](
               const QVector<RpcRequest>& requests,
               std::stop_token) {
        return performer(requests);
    };
}

using GoalMap =
    QHash<QString, std::optional<BridgeGoal>>;

} // namespace

void GoalService::probeReadPhase(
    const ReadPhaseProbe& probe,
    ReadPhase phase,
    qsizetype index) noexcept
{
    if (!probe) {
        return;
    }
    try {
        probe(phase, index);
    } catch (...) {
    }
}

Result<GoalMap> GoalService::readGoalsSync(
    const CancellableGoalRpcPerformer& performer,
    const ReadPhaseProbe& readPhaseProbe,
    const QVector<QString>& threadIds,
    std::stop_token stopToken)
{
    try {
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }

        QSet<QString> seen;
        QVector<QString> normalized;
        normalized.reserve(threadIds.size());
        for (qsizetype index = 0;
             index < threadIds.size();
             ++index) {
            const QString threadId =
                threadIds.at(index).trimmed();
            if (!threadId.isEmpty()
                && !seen.contains(threadId)) {
                seen.insert(threadId);
                normalized.append(threadId);
            }
            probeReadPhase(
                readPhaseProbe,
                ReadPhase::AfterNormalizeItem,
                index);
            if (stopToken.stop_requested()) {
                return goalCanceled<GoalMap>();
            }
        }
        std::sort(
            normalized.begin(),
            normalized.end());
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }
        if (normalized.isEmpty()) {
            return Result<GoalMap>::success({});
        }

        QVector<RpcRequest> requests;
        requests.reserve(normalized.size());
        for (qsizetype index = 0;
             index < normalized.size();
             ++index) {
            requests.append({
                static_cast<int>(index + 2),
                QStringLiteral("thread/goal/get"),
                {
                    {
                        QStringLiteral("threadId"),
                        normalized.at(index),
                    },
                },
            });
            probeReadPhase(
                readPhaseProbe,
                ReadPhase::AfterRequestBuilt,
                index);
            if (stopToken.stop_requested()) {
                return goalCanceled<GoalMap>();
            }
        }
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }
        if (!performer) {
            return goalUnavailable<GoalMap>(
                {{QStringLiteral("method"),
                  QStringLiteral("thread/goal/get")}});
        }

        const Result<QHash<int, RpcResponse>> responses =
            performer(requests, stopToken);
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }
        if (!responses.hasValue()) {
            if (stopToken.stop_requested()) {
                return goalCanceled<GoalMap>();
            }
            if (responses.error().code
                == QStringLiteral(
                    "codex.operation_canceled")) {
                return Result<GoalMap>::failure(
                    responses.error());
            }
            return goalUnavailable<GoalMap>(
                {{QStringLiteral("method"),
                  QStringLiteral("thread/goal/get")}});
        }
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }

        GoalMap goals;
        for (qsizetype index = 0;
             index < normalized.size();
             ++index) {
            if (stopToken.stop_requested()) {
                return goalCanceled<GoalMap>();
            }
            const int id =
                static_cast<int>(index + 2);
            const Result<QJsonValue> raw =
                resultFor(id, responses.value());
            if (!raw.hasValue()) {
                return Result<GoalMap>::failure(
                    raw.error());
            }
            if (!raw.value().isObject()) {
                return goalFailure<GoalMap>(
                    QStringLiteral(
                        "codex.goal_unavailable"),
                    QStringLiteral(
                        "Codex app-server returned an unreadable goal."));
            }
            const QJsonValue goalValue =
                raw.value()
                    .toObject()
                    .value(QStringLiteral("goal"));
            if (goalValue.isNull()
                || goalValue.isUndefined()) {
                goals.insert(
                    normalized.at(index),
                    std::nullopt);
            } else {
                const Result<BridgeGoal> goal =
                    parseGoal(goalValue);
                if (!goal.hasValue()) {
                    return Result<GoalMap>::failure(
                        goal.error());
                }
                goals.insert(
                    normalized.at(index),
                    goal.value());
            }
            probeReadPhase(
                readPhaseProbe,
                ReadPhase::AfterResponseParsed,
                index);
            if (stopToken.stop_requested()) {
                return goalCanceled<GoalMap>();
            }
        }
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }
        return Result<GoalMap>::success(
            std::move(goals));
    } catch (...) {
        if (stopToken.stop_requested()) {
            return goalCanceled<GoalMap>();
        }
        return goalUnavailable<GoalMap>(
            {{QStringLiteral("method"),
              QStringLiteral("thread/goal/get")}});
    }
}

GoalService::GoalService(
    const CodexEnvironment& environment,
    QProcessEnvironment processEnvironment,
    int timeoutMilliseconds)
    : GoalService(
          [client = std::make_shared<AppServerRpcClient>(
               environment,
               std::move(processEnvironment),
               timeoutMilliseconds)](
              const QVector<RpcRequest>& requests,
              std::stop_token stopToken) {
              return client->perform(
                  requests, stopToken);
          })
{
}

GoalService::GoalService(
    GoalRpcPerformer performer,
    GoalCommitProbe mutationCommitProbe)
    : GoalService(
          cancellablePerformer(
              std::move(performer)),
          std::move(mutationCommitProbe))
{
}

GoalService::GoalService(
    CancellableGoalRpcPerformer performer,
    GoalCommitProbe mutationCommitProbe)
    : GoalService(
          std::move(performer),
          std::move(mutationCommitProbe),
          {})
{
}

GoalService::GoalService(
    CancellableGoalRpcPerformer performer,
    GoalCommitProbe mutationCommitProbe,
    ReadPhaseProbe readPhaseProbe)
    : performer_(std::move(performer)),
      mutationCommitProbe_(std::move(mutationCommitProbe)),
      readPhaseProbe_(std::move(readPhaseProbe))
{
}

Result<QHash<QString, std::optional<BridgeGoal>>>
GoalService::readSync(
    const QVector<QString>& threadIds,
    std::stop_token stopToken) const
{
    return readGoalsSync(
        performer_,
        readPhaseProbe_,
        threadIds,
        stopToken);
}

QFuture<Result<QHash<QString, std::optional<BridgeGoal>>>>
GoalService::read(
    const QVector<QString>& threadIds) const
{
    auto promise = std::make_shared<
        QPromise<Result<QHash<QString, std::optional<BridgeGoal>>>>>();
    promise->start();
    QFuture<Result<QHash<QString, std::optional<BridgeGoal>>>> future =
        promise->future();
    QThreadPool::globalInstance()->start(
        [performer = performer_,
         readPhaseProbe = readPhaseProbe_,
         threadIds,
         promise] {
            GoalCompletion<QHash<QString, std::optional<BridgeGoal>>>
                completion(promise);
            try {
                if (promise->isCanceled()) {
                    completion.cancel();
                    return;
                }
                Result<GoalMap> result =
                    readGoalsSync(
                        performer,
                        readPhaseProbe,
                        threadIds,
                        {});
                if (promise->isCanceled()) {
                    completion.cancel();
                    return;
                }
                completion.finish(
                    std::move(result));
            } catch (...) {
                completion.finish(
                    goalUnavailable<QHash<QString, std::optional<BridgeGoal>>>(
                        {{QStringLiteral("method"),
                          QStringLiteral("thread/goal/get")}}));
            }
        });
    return future;
}

CommitAwareMutationHandle<BridgeGoal>
GoalService::createMutation(
    const QString& threadId,
    const QString& objective,
    std::optional<qint64> tokenBudget) const
{
    return setMutation({
        threadId,
        objective,
        std::nullopt,
        tokenBudget,
    });
}

CommitAwareMutationHandle<BridgeGoal>
GoalService::setMutation(
    const GoalSetRequest& request) const
{
    auto mutation =
        CommitAwareMutation<BridgeGoal>::create();
    CommitAwareMutationHandle<BridgeGoal> handle =
        mutation->handle();
    try {
        QThreadPool::globalInstance()->start(
        [performer = performer_,
         mutationCommitProbe = mutationCommitProbe_,
         request,
         mutation] {
            const QString threadId = request.threadId.trimmed();
            try {
            const Result<void> validThread =
                validateThreadId(request.threadId);
            if (!validThread.hasValue()) {
                mutation->finish(
                    Result<BridgeGoal>::failure(
                        validThread.error()));
                return;
            }
            if (request.objective.has_value()) {
                const Result<void> validObjective =
                    validateObjective(*request.objective);
                if (!validObjective.hasValue()) {
                    mutation->finish(
                        Result<BridgeGoal>::failure(
                            validObjective.error()));
                    return;
                }
            }
            const Result<void> validBudget =
                validateBudget(request.tokenBudget);
            if (!validBudget.hasValue()) {
                mutation->finish(
                    Result<BridgeGoal>::failure(
                        validBudget.error()));
                return;
            }
            if (!performer) {
                mutation->finish(
                    goalUnavailable<BridgeGoal>(
                        {
                            {QStringLiteral("threadId"), threadId},
                            {QStringLiteral("method"),
                             QStringLiteral("thread/goal/set")},
                        }));
                return;
            }
            probeGoalCommit(
                mutationCommitProbe,
                QStringLiteral("mutation.commitPending"));
            probeGoalCommit(
                mutationCommitProbe,
                QStringLiteral("mutation.claimEstablished"));
            if (!mutation->tryCommit()) {
                return;
            }
            probeGoalCommit(
                mutationCommitProbe,
                QStringLiteral("mutation.committed"));
            const RpcRequest rpc =
                goalSetRequest(2, request);
            const Result<QHash<int, RpcResponse>> responses =
                performer({rpc}, {});
            if (!responses.hasValue()) {
                mutation->finish(
                    goalUnavailable<BridgeGoal>(
                        {
                            {QStringLiteral("threadId"), threadId},
                            {QStringLiteral("method"),
                             QStringLiteral("thread/goal/set")},
                        }));
                return;
            }
            mutation->finish(
                parseMutationResponse(
                    2,
                    responses.value()));
            } catch (...) {
                mutation->finish(
                    goalUnavailable<BridgeGoal>(
                        {
                            {QStringLiteral("threadId"), threadId},
                            {QStringLiteral("method"),
                             QStringLiteral("thread/goal/set")},
                        }));
            }
        });
    } catch (...) {
        mutation->finish(
            goalUnavailable<BridgeGoal>({
                {
                    QStringLiteral("threadId"),
                    request.threadId.trimmed(),
                },
                {
                    QStringLiteral("method"),
                    QStringLiteral("thread/goal/set"),
                },
            }));
    }
    return handle;
}

CommitAwareMutationHandle<BridgeGoal>
GoalService::pauseMutation(
    const QString& threadId) const
{
    return setMutation({
        threadId,
        std::nullopt,
        GoalStatus::Paused,
        std::nullopt,
    });
}

CommitAwareMutationHandle<BridgeGoal>
GoalService::resumeMutation(
    const QString& threadId) const
{
    return setMutation({
        threadId,
        std::nullopt,
        GoalStatus::Active,
        std::nullopt,
    });
}

CommitAwareMutationHandle<BridgeGoal>
GoalService::updateMutation(
    const GoalUpdateRequest& request) const
{
    return setMutation({
        request.threadId,
        request.objective,
        std::nullopt,
        request.tokenBudget,
    });
}

QFuture<Result<BridgeGoal>> GoalService::create(
    const QString& threadId,
    const QString& objective,
    std::optional<qint64> tokenBudget) const
{
    return cancellationDetachedMutationFuture(
        createMutation(
            threadId,
            objective,
            tokenBudget)
            .terminalFuture);
}

QFuture<Result<BridgeGoal>> GoalService::set(
    const GoalSetRequest& request) const
{
    return cancellationDetachedMutationFuture(
        setMutation(request).terminalFuture);
}

QFuture<Result<BridgeGoal>> GoalService::pause(
    const QString& threadId) const
{
    return cancellationDetachedMutationFuture(
        pauseMutation(threadId).terminalFuture);
}

QFuture<Result<BridgeGoal>> GoalService::resume(
    const QString& threadId) const
{
    return cancellationDetachedMutationFuture(
        resumeMutation(threadId).terminalFuture);
}

QFuture<Result<BridgeGoal>> GoalService::update(
    const GoalUpdateRequest& request) const
{
    return cancellationDetachedMutationFuture(
        updateMutation(request).terminalFuture);
}

} // namespace companion
