#include "app/CompanionMobileRuntimeAdapter.h"

#include "codex/commands/CommitAwareMutation.h"

#include <QDateTime>
#include <QPromise>
#include <QSet>
#include <QtConcurrentRun>

#include <algorithm>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace companion {
namespace {

struct AdapterState final {
    CodexRuntimeDependencies
        dependencies;
};

CompanionError dependenciesUnavailable()
{
    return {
        QStringLiteral(
            "mobile.runtime_dependencies_unavailable"),
        QStringLiteral(
            "The production Codex runtime dependencies are unavailable."),
        false,
        {},
    };
}

CompanionError approvalUnavailable()
{
    return {
        QStringLiteral(
            "approval.request_not_found"),
        QStringLiteral(
            "That approval request is no longer active."),
        false,
        {},
    };
}

bool valid(
    const CodexRuntimeDependencies&
        dependencies)
{
    return dependencies.taskLoader
        && dependencies.goalLoader
        && dependencies.nowProvider
        && dependencies.history
               .has_value()
        && dependencies.history
               ->historyLoader
        && dependencies.reads
               .has_value()
        && dependencies.reads
               ->capabilityLoader
        && dependencies.reads
               ->usageReadStarter
        && dependencies.mutations
               .has_value()
        && dependencies.mutations
               ->sendMutationStarter
        && dependencies.mutations
               ->approvalMutationStarter
        && dependencies.mutations
               ->taskCreateMutationStarter
        && dependencies.mutations
               ->chatMutationStarter
        && dependencies.mutations
               ->goalMutationStarter
        && dependencies.mutations
               ->usageResetMutationStarter;
}

template <typename T>
QFuture<Result<T>> readyFuture(
    Result<T> result)
{
    QPromise<Result<T>> promise;
    promise.start();
    QFuture<Result<T>> future =
        promise.future();
    promise.addResult(
        std::move(result));
    promise.finish();
    return future;
}

qint64 pageOffset(
    const std::optional<QString>& cursor)
{
    if (!cursor.has_value()) {
        return 0;
    }
    bool validNumber = false;
    const qint64 parsed =
        cursor->trimmed().toLongLong(
            &validNumber);
    return validNumber && parsed > 0
        ? parsed
        : 0;
}

QSet<QString> pendingApprovalThreads(
    const std::shared_ptr<
        AdapterState>& state)
{
    const auto snapshot =
        state->dependencies
            .taskLoader(
                {},
                std::stop_token{});
    if (!snapshot.hasValue()) {
        return {};
    }
    QSet<QString> result;
    result.reserve(
        snapshot.value()
            .pendingApprovals.size());
    for (auto iterator =
             snapshot.value()
                 .pendingApprovals
                 .cbegin();
         iterator
         != snapshot.value()
                .pendingApprovals
                .cend();
         ++iterator) {
        result.insert(
            iterator.key());
    }
    return result;
}

std::optional<PendingApproval>
pendingApproval(
    const std::shared_ptr<
        AdapterState>& state,
    const QString& threadId)
{
    const auto snapshot =
        state->dependencies
            .taskLoader(
                {},
                std::stop_token{});
    if (!snapshot.hasValue()) {
        return std::nullopt;
    }
    const auto found =
        snapshot.value()
            .pendingApprovals
            .constFind(threadId);
    return found
            == snapshot.value()
                   .pendingApprovals
                   .cend()
        ? std::nullopt
        : std::optional<
              PendingApproval>(
              found.value());
}

} // namespace

Result<
    CompanionMobileRuntimeBindings>
CompanionMobileRuntimeAdapter::create(
    CodexRuntimeDependencies dependencies)
{
    if (!valid(dependencies)) {
        return Result<
            CompanionMobileRuntimeBindings>::
            failure(
                dependenciesUnavailable());
    }

    const auto state =
        std::make_shared<AdapterState>(
            AdapterState{
                std::move(dependencies),
            });

    CompanionMobileRuntimeBindings
        bindings;
    bindings.reads.taskPageLoader =
        [state](
            std::optional<QString> cursor,
            qint64 requestedLimit) {
            return QtConcurrent::run(
                [state,
                 cursor =
                     std::move(cursor),
                 requestedLimit]() {
                    const auto snapshot =
                        state->dependencies
                            .taskLoader(
                                {},
                                std::stop_token{});
                    if (!snapshot
                             .hasValue()) {
                        return Result<
                            MobileTaskPage>::
                            failure(
                                snapshot.error());
                    }

                    const QVector<BridgeTask>&
                        tasks =
                            snapshot.value()
                                .tasks;
                    const qint64 offset =
                        std::min(
                            pageOffset(
                                cursor),
                            qint64(
                                tasks.size()));
                    const qint64 limit =
                        std::clamp(
                            requestedLimit,
                            qint64(1),
                            kMaximumPageSize);
                    const qint64 end =
                        std::min(
                            qint64(
                                tasks.size()),
                            offset + limit);

                    MobileTaskPage page;
                    page.tasks.reserve(
                        qsizetype(
                            end - offset));
                    for (qint64 index =
                             offset;
                         index < end;
                         ++index) {
                        page.tasks.append(
                            tasks.at(
                                qsizetype(
                                    index)));
                    }
                    if (end
                        < tasks.size()) {
                        page.nextCursor =
                            QString::number(
                                end);
                    }
                    return Result<
                        MobileTaskPage>::
                        success(
                            std::move(
                                page));
                });
        };
    bindings.reads.goalLoader =
        [state](
            QVector<QString> threadIds) {
            return QtConcurrent::run(
                [state,
                 threadIds =
                     std::move(
                         threadIds)] {
                    return state
                        ->dependencies
                        .goalLoader(
                            threadIds,
                            std::stop_token{});
                });
        };
    bindings.reads.historyLoader =
        [state](
            MobileHistoryKey key) {
            return QtConcurrent::run(
                [state,
                 key =
                     std::move(key)] {
                    return state
                        ->dependencies
                        .history
                        ->historyLoader(
                            HistoryKey{
                                key.threadId,
                                key.cursor,
                                key.limit,
                            },
                            pendingApprovalThreads(
                                state),
                            state
                                ->dependencies
                                .nowProvider(),
                            std::stop_token{});
                });
        };
    bindings.reads.capabilityLoader =
        [state](QString cwd) {
            return QtConcurrent::run(
                [state,
                 cwd = std::move(cwd)] {
                    return state
                        ->dependencies
                        .reads
                        ->capabilityLoader(
                            cwd,
                            std::stop_token{});
                });
        };
    bindings.reads.usageLoader =
        [state] {
            return state
                ->dependencies
                .reads
                ->usageReadStarter();
        };

    bindings.mutations.sendMessage =
        [state](SendRequest request) {
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->sendMutationStarter(
                        std::move(request))
                    .terminalFuture);
        };
    bindings.mutations.respondToApproval =
        [state](
            QString threadId,
            ApprovalDecision decision) {
            const auto approval =
                pendingApproval(
                    state,
                    threadId);
            if (!approval.has_value()) {
                return readyFuture<void>(
                    Result<void>::failure(
                        approvalUnavailable()));
            }
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->approvalMutationStarter(
                        *approval,
                        decision)
                    .terminalFuture);
        };
    bindings.mutations.createTask =
        [state](
            RuntimeTaskCreateRequest request) {
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->taskCreateMutationStarter(
                        std::move(request))
                    .terminalFuture);
        };
    bindings.mutations.sendCasualChat =
        [state](ChatRequest request) {
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->chatMutationStarter(
                        std::move(request))
                    .terminalFuture);
        };
    bindings.mutations.consumeUsageReset =
        [state](
            QString creditId,
            QUuid idempotencyKey) {
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->usageResetMutationStarter(
                        std::move(creditId),
                        idempotencyKey)
                    .terminalFuture);
        };
    bindings.mutations.mutateGoal =
        [state](
            RuntimeGoalMutationRequest
                request) {
            return cancellationDetachedMutationFuture(
                state->dependencies
                    .mutations
                    ->goalMutationStarter(
                        std::move(request))
                    .terminalFuture);
        };

    return Result<
        CompanionMobileRuntimeBindings>::
        success(
            std::move(bindings));
}

} // namespace companion
