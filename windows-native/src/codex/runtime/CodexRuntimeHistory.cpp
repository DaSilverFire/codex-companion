#include "codex/runtime/CodexRuntime.h"

#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/CodexRuntimeOperationState.h"
#include "codex/runtime/RuntimeContinuationHost.h"

#include <QMetaObject>
#include <QMetaType>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace {

CompanionError runtimeUnavailableError()
{
    return {
        QStringLiteral("codex.runtime_unavailable"),
        QStringLiteral(
            "Codex runtime is unavailable."),
        false,
        {},
    };
}

CompanionError invalidArgumentsError()
{
    return {
        QStringLiteral(
            "codex.command_invalid_arguments"),
        QStringLiteral(
            "Invalid Codex command arguments."),
        false,
        {},
    };
}

CompanionError historyFailure()
{
    return {
        QStringLiteral(
            "codex.history_load_failed"),
        QStringLiteral(
            "Could not load Codex task history."),
        true,
        {},
    };
}

Result<HistorySnapshot> sanitizedHistoryResult(
    Result<HistorySnapshot> result)
{
    if (result.hasValue()) {
        return result;
    }
    return Result<HistorySnapshot>::failure(
        historyFailure());
}

bool isAllowedHistoryKey(const QString& key)
{
    return key == QStringLiteral("threadId")
        || key == QStringLiteral("cursor")
        || key == QStringLiteral("limit");
}

std::optional<int> parseHistoryLimit(
    const QVariant& value)
{
    qint64 signedValue = 0;
    quint64 unsignedValue = 0;
    bool isUnsigned = false;

    switch (value.metaType().id()) {
    case QMetaType::Int:
        signedValue = value.toInt();
        break;
    case QMetaType::UInt:
        unsignedValue = value.toUInt();
        isUnsigned = true;
        break;
    case QMetaType::LongLong:
        signedValue = value.toLongLong();
        break;
    case QMetaType::ULongLong:
        unsignedValue = value.toULongLong();
        isUnsigned = true;
        break;
    case QMetaType::Float:
    case QMetaType::Double: {
        const double number = value.toDouble();
        if (!std::isfinite(number)
            || std::trunc(number) != number
            || number < 1.0
            || number
                > static_cast<double>(
                    std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        signedValue = static_cast<qint64>(number);
        break;
    }
    default:
        return std::nullopt;
    }

    if (isUnsigned) {
        if (unsignedValue == 0
            || unsignedValue
                > static_cast<quint64>(
                    std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        signedValue =
            static_cast<qint64>(unsignedValue);
    }
    if (signedValue < 1
        || signedValue
            > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }

    return std::min(
        static_cast<int>(signedValue),
        static_cast<int>(kMaximumPageSize));
}

Result<HistoryKey> parseHistoryArguments(
    const QVariantMap& arguments)
{
    for (auto argument = arguments.cbegin();
         argument != arguments.cend();
         ++argument) {
        if (!isAllowedHistoryKey(
                argument.key())) {
            return Result<HistoryKey>::failure(
                invalidArgumentsError());
        }
    }

    const auto threadIdArgument =
        arguments.constFind(
            QStringLiteral("threadId"));
    if (threadIdArgument == arguments.constEnd()
        || threadIdArgument->metaType().id()
            != QMetaType::QString) {
        return Result<HistoryKey>::failure(
            invalidArgumentsError());
    }
    const QString threadId =
        threadIdArgument->toString().trimmed();
    if (threadId.isEmpty()) {
        return Result<HistoryKey>::failure(
            invalidArgumentsError());
    }

    std::optional<QString> cursor;
    const auto cursorArgument =
        arguments.constFind(
            QStringLiteral("cursor"));
    if (cursorArgument != arguments.constEnd()) {
        if (cursorArgument->metaType().id()
            != QMetaType::QString) {
            return Result<HistoryKey>::failure(
                invalidArgumentsError());
        }
        cursor = cursorArgument->toString();
    }

    int limit =
        static_cast<int>(
            kDefaultMessagePageSize);
    const auto limitArgument =
        arguments.constFind(
            QStringLiteral("limit"));
    if (limitArgument != arguments.constEnd()) {
        const std::optional<int> parsed =
            parseHistoryLimit(*limitArgument);
        if (!parsed.has_value()) {
            return Result<HistoryKey>::failure(
                invalidArgumentsError());
        }
        limit = *parsed;
    }

    return Result<HistoryKey>::success({
        threadId,
        std::move(cursor),
        limit,
    });
}

std::uint64_t nextGeneration(
    std::uint64_t& generation)
{
    ++generation;
    if (generation == 0) {
        ++generation;
    }
    return generation;
}

} // namespace

struct CodexRuntimeHistoryWaiter final {
    std::uint64_t selectionGeneration = 0;
    std::shared_ptr<
        CodexRuntimeOperationState>
        operation;
};

struct CodexRuntimeHistoryEntry final {
    HistoryKey key;
    std::uint64_t runtimeGeneration = 0;
    std::mutex mutex;
    QVector<CodexRuntimeHistoryWaiter> waiters;
    std::optional<Result<HistorySnapshot>>
        terminalResult;
    HistoryLoadHandle loadHandle;
};

void CodexRuntime::dispatchHistoryCommand(
    QVariantMap arguments,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation)
{
    if (invocation == nullptr) {
        return;
    }

    const auto delivery =
        invocation->deliveryState.lock();
    if (delivery == nullptr) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }

    QPointer<CodexRuntime> owner;
    QThread* ownerThread = nullptr;
    bool unavailable = false;
    bool queued = false;
    {
        const std::scoped_lock lock(
            delivery->mutex);
        if (delivery->destroying
            || delivery->runtime.isNull()) {
            unavailable = true;
        } else {
            owner = delivery->runtime;
            ownerThread = owner->thread();
            if (QThread::currentThread()
                != ownerThread) {
                queued =
                    QMetaObject::invokeMethod(
                        owner.data(),
                        [arguments =
                             std::move(arguments),
                         invocation]() mutable {
                            dispatchHistoryCommand(
                                std::move(arguments),
                                invocation);
                        },
                        Qt::QueuedConnection);
            }
        }
    }

    if (unavailable) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }
    if (QThread::currentThread() != ownerThread) {
        if (!queued) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
        }
        return;
    }
    if (owner.isNull()
        || !invocation->claimInvocation()) {
        if (owner.isNull()) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
        }
        return;
    }

    try {
        const TransitionGuard transition(*owner);
        owner->requestHistoryOnOwnerThread(
            arguments,
            invocation);
    } catch (...) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    }
}

void CodexRuntime::requestHistoryOnOwnerThread(
    const QVariantMap& arguments,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation)
{
    if (!running_
        || deferredStop_
        || !historyCommandsEnabled_
        || !historyLoader_
        || historyCoordinator_ == nullptr
        || continuationHost_ == nullptr
        || operationRegistry_ == nullptr) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }

    const Result<HistoryKey> parsed =
        parseHistoryArguments(arguments);
    if (!parsed.hasValue()) {
        invocation->finish(
            Result<void>::failure(
                invalidArgumentsError()));
        return;
    }

    const CollaboratorStatus status =
        collaboratorStatus();
    if (status != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(status);
        stopForRuntimeFailure(error);
        invocation->finish(
            Result<void>::failure(error));
        return;
    }

    const std::uint64_t operationGeneration =
        nextGeneration(
            nextOperationGeneration_);
    const auto operation =
        CodexRuntimeOperationState::createRead(
            [invocation](Result<void> result) {
                invocation->finish(
                    std::move(result));
            },
            generation_,
            operationGeneration,
            [] {
            });
    if (operation == nullptr
        || operationRegistry_->registerOperation(
               operation)
            == 0) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }

    const HistoryKey historyKey =
        parsed.value();
    const std::uint64_t selectionGeneration =
        nextGeneration(
            historySelectionGeneration_);
    selectedHistoryThreadId_ =
        historyKey.threadId;
    historyLoading_ = true;
    historyErrorCode_.clear();
    historyErrorMessage_.clear();
    emit historyChanged();
    if (deferredStop_
        || !running_
        || operation->terminal()) {
        return;
    }

    const CodexRuntimeHistoryWaiter waiter{
        selectionGeneration,
        operation,
    };
    const auto existing =
        historyEntries_.find(historyKey);
    if (existing != historyEntries_.end()) {
        const auto entry = existing.value();
        {
            const std::scoped_lock lock(
                entry->mutex);
            entry->waiters.append(waiter);
        }
        return;
    }

    const auto entry =
        std::make_shared<
            CodexRuntimeHistoryEntry>();
    entry->key = historyKey;
    entry->runtimeGeneration = generation_;
    entry->waiters.append(waiter);
    historyEntries_.insert(
        historyKey,
        entry);

    QSet<QString> pendingApprovalThreadIds;
    pendingApprovalThreadIds.reserve(
        processSnapshot_.pendingApprovals.size());
    for (auto approval =
             processSnapshot_
                 .pendingApprovals.cbegin();
         approval
         != processSnapshot_
                .pendingApprovals.cend();
         ++approval) {
        pendingApprovalThreadIds.insert(
            approval.key());
    }

    QDateTime now;
    try {
        now = nowProvider_();
        if (now.isValid()) {
            now = now.toUTC();
        }
    } catch (...) {
    }

    try {
        const RuntimeHistoryLoader loader =
            historyLoader_;
        entry->loadHandle =
            historyCoordinator_->loadCancellable(
                historyKey,
                [loader,
                 historyKey,
                 pendingApprovalThreadIds =
                     std::move(
                         pendingApprovalThreadIds),
                 now](
                    std::stop_token stopToken) {
                    if (!now.isValid()) {
                        return Result<
                            HistorySnapshot>::failure(
                            historyFailure());
                    }
                    try {
                        return sanitizedHistoryResult(
                            loader(
                                historyKey,
                                pendingApprovalThreadIds,
                                now,
                                stopToken));
                    } catch (...) {
                        return Result<
                            HistorySnapshot>::failure(
                            historyFailure());
                    }
                });
    } catch (...) {
        {
            const std::scoped_lock lock(
                entry->mutex);
            entry->terminalResult =
                Result<HistorySnapshot>::failure(
                    historyFailure());
        }
        postHistoryResult(
            std::weak_ptr<
                CodexRuntimeDeliveryState>(
                deliveryState_),
            generation_,
            entry);
        return;
    }

    QFuture<Result<HistorySnapshot>> future =
        entry->loadHandle.future;
    const auto weakDelivery =
        std::weak_ptr<
            CodexRuntimeDeliveryState>(
            deliveryState_);
    const Result<void> submitted =
        continuationHost_->submit(
            [future,
             entry,
             weakDelivery]() mutable {
                Result<HistorySnapshot> result =
                    Result<HistorySnapshot>::failure(
                        historyFailure());
                try {
                    if (future.isValid()) {
                        future.waitForFinished();
                        if (!future.isCanceled()
                            && future.resultCount()
                                == 1) {
                            result =
                                sanitizedHistoryResult(
                                    future.result());
                        }
                    }
                } catch (...) {
                    result =
                        Result<HistorySnapshot>::
                            failure(
                                historyFailure());
                }
                {
                    const std::scoped_lock lock(
                        entry->mutex);
                    if (!entry->terminalResult
                             .has_value()) {
                        entry->terminalResult =
                            std::move(result);
                    }
                }
                CodexRuntime::postHistoryResult(
                    weakDelivery,
                    entry->runtimeGeneration,
                    entry);
            });
    if (!submitted.hasValue()) {
        if (entry->loadHandle
                .cancellationLease
            != nullptr) {
            entry->loadHandle
                .cancellationLease
                ->requestStop();
        }
        {
            const std::scoped_lock lock(
                entry->mutex);
            if (!entry->terminalResult
                     .has_value()) {
                entry->terminalResult =
                    Result<HistorySnapshot>::failure(
                        historyFailure());
            }
        }
        postHistoryResult(
            weakDelivery,
            entry->runtimeGeneration,
            entry);
    }
}

void CodexRuntime::applyHistoryResult(
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeHistoryEntry>& entry)
{
    if (entry == nullptr
        || !running_
        || runtimeGeneration != generation_) {
        return;
    }
    const TransitionGuard transition(*this);
    const CollaboratorStatus completionStatus =
        collaboratorStatus();
    if (completionStatus
        != CollaboratorStatus::Ready) {
        stopForRuntimeFailure(
            collaboratorError(completionStatus));
        return;
    }

    const auto current =
        historyEntries_.find(entry->key);
    if (current == historyEntries_.end()
        || current.value() != entry) {
        return;
    }
    historyEntries_.erase(current);

    QVector<CodexRuntimeHistoryWaiter> waiters;
    Result<HistorySnapshot> result =
        Result<HistorySnapshot>::failure(
            historyFailure());
    {
        const std::scoped_lock lock(
            entry->mutex);
        waiters = std::move(entry->waiters);
        if (entry->terminalResult.has_value()) {
            result =
                sanitizedHistoryResult(
                    std::move(
                        *entry->terminalResult));
        }
    }

    bool publishesSelectedGeneration = false;
    for (const auto& waiter : waiters) {
        if (waiter.selectionGeneration
                == historySelectionGeneration_
            && selectedHistoryThreadId_
                == entry->key.threadId) {
            publishesSelectedGeneration = true;
            break;
        }
    }

    if (publishesSelectedGeneration) {
        historyLoading_ = false;
        if (result.hasValue()) {
            historyPublication_ =
                CodexHistoryPublication{
                    entry->key.threadId,
                    entry->key.cursor,
                    result.value(),
                };
            historyErrorCode_.clear();
            historyErrorMessage_.clear();
        } else {
            const CompanionError error =
                historyFailure();
            historyErrorCode_ = error.code;
            historyErrorMessage_ =
                error.message;
        }
    }

    for (const auto& waiter : waiters) {
        if (waiter.operation == nullptr) {
            continue;
        }
        waiter.operation->finish(
            result.hasValue()
            ? Result<void>::success()
            : Result<void>::failure(
                  historyFailure()));
    }
    if (publishesSelectedGeneration) {
        emit historyChanged();
    }
}

void CodexRuntime::postHistoryResult(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeHistoryEntry>& entry)
{
    const auto state = weakState.lock();
    if (state == nullptr || entry == nullptr) {
        return;
    }

    const std::scoped_lock lock(state->mutex);
    if (state->destroying
        || state->runtime.isNull()) {
        return;
    }
    QMetaObject::invokeMethod(
        state->runtime.data(),
        [weakState,
         runtimeGeneration,
         entry] {
            const auto delivery =
                weakState.lock();
            if (delivery == nullptr) {
                return;
            }
            QPointer<CodexRuntime> runtime;
            {
                const std::scoped_lock deliveryLock(
                    delivery->mutex);
                if (delivery->destroying) {
                    return;
                }
                runtime = delivery->runtime;
            }
            if (runtime.isNull()) {
                return;
            }
            runtime->applyHistoryResult(
                runtimeGeneration,
                entry);
        },
        Qt::QueuedConnection);
}

void CodexRuntime::requestHistoryStops() noexcept
{
    try {
        for (auto entry =
                 historyEntries_.cbegin();
             entry != historyEntries_.cend();
             ++entry) {
            if (entry.value() != nullptr
                && entry.value()
                       ->loadHandle
                       .cancellationLease
                    != nullptr) {
                entry.value()
                    ->loadHandle
                    .cancellationLease
                    ->requestStop();
            }
        }
    } catch (...) {
    }
}

void CodexRuntime::stopHistoryOperations()
{
    requestHistoryStops();
    historyEntries_.clear();
    nextGeneration(
        historySelectionGeneration_);
    if (historyLoading_) {
        historyLoading_ = false;
        emit historyChanged();
    }
}

} // namespace companion
