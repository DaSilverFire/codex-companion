#include "codex/runtime/CodexRuntime.h"

#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/CodexRuntimeOperationState.h"
#include "codex/runtime/RuntimeContinuationHost.h"

#include <QMetaObject>
#include <QMetaType>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

namespace {

enum class ReadDispatchGateState : int {
    Unclaimed = 0,
    Accepted = 1,
    Rejected = 2,
};

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

CompanionError capabilityFailure()
{
    return {
        QStringLiteral(
            "codex.capabilities_load_failed"),
        QStringLiteral(
            "Could not load Codex capabilities."),
        true,
        {},
    };
}

CompanionError usageFailure()
{
    return {
        QStringLiteral(
            "codex.usage_load_failed"),
        QStringLiteral(
            "Could not load Codex usage."),
        true,
        {},
    };
}

CompanionError supersededError()
{
    return {
        QStringLiteral(
            "codex.operation_superseded"),
        QStringLiteral(
            "A newer Codex request replaced this one."),
        false,
        {},
    };
}

Result<QString> parseCapabilityArguments(
    const QVariantMap& arguments)
{
    for (auto argument = arguments.cbegin();
         argument != arguments.cend();
         ++argument) {
        if (argument.key()
            != QStringLiteral("cwd")) {
            return Result<QString>::failure(
                invalidArgumentsError());
        }
    }

    const auto cwd =
        arguments.constFind(
            QStringLiteral("cwd"));
    if (cwd == arguments.constEnd()) {
        return Result<QString>::success(
            QString());
    }
    if (cwd->metaType().id()
        != QMetaType::QString) {
        return Result<QString>::failure(
            invalidArgumentsError());
    }
    return Result<QString>::success(
        cwd->toString().trimmed());
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

Result<BridgeCapabilities>
sanitizedCapabilityResult(
    Result<BridgeCapabilities> result)
{
    return result.hasValue()
        ? std::move(result)
        : Result<BridgeCapabilities>::failure(
              capabilityFailure());
}

Result<BridgeUsageSnapshot>
sanitizedUsageResult(
    Result<BridgeUsageSnapshot> result)
{
    return result.hasValue()
        ? std::move(result)
        : Result<BridgeUsageSnapshot>::failure(
              usageFailure());
}

} // namespace

struct CodexRuntimeCapabilityEntry final {
    QString cwd;
    std::uint64_t runtimeGeneration = 0;
    std::uint64_t entryGeneration = 0;
    std::uint64_t requiredRevision = 0;
    std::shared_ptr<std::stop_source>
        stopSource =
            std::make_shared<std::stop_source>();
    bool publicationEligible = true;
    bool forceFresh = false;
    std::mutex mutex;
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        waiters;
    std::optional<
        Result<BridgeCapabilities>>
        terminalResult;
};

struct CodexRuntimeCapabilityStopLink final {
    void bind(
        const std::shared_ptr<std::stop_source>&
            requestedSource)
    {
        std::shared_ptr<std::stop_source> source;
        {
            const std::scoped_lock lock(mutex);
            stopSource = requestedSource;
            if (stopRequested) {
                source = requestedSource;
            }
        }
        if (source != nullptr) {
            source->request_stop();
        }
    }

    void requestStop() noexcept
    {
        try {
            std::shared_ptr<std::stop_source> source;
            {
                const std::scoped_lock lock(mutex);
                stopRequested = true;
                source = stopSource.lock();
            }
            if (source != nullptr) {
                source->request_stop();
            }
        } catch (...) {
        }
    }

    std::mutex mutex;
    std::weak_ptr<std::stop_source> stopSource;
    bool stopRequested = false;
};

struct CodexRuntimeUsageEntry final {
    std::uint64_t runtimeGeneration = 0;
    std::uint64_t entryGeneration = 0;
    std::mutex mutex;
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        waiters;
    std::optional<
        Result<BridgeUsageSnapshot>>
        terminalResult;
};

namespace {

template <typename Entry, typename Value>
bool storeTerminalResult(
    const std::shared_ptr<Entry>& entry,
    Result<Value> result)
{
    if (entry == nullptr) {
        return false;
    }
    const std::scoped_lock lock(entry->mutex);
    if (entry->terminalResult.has_value()) {
        return false;
    }
    entry->terminalResult = std::move(result);
    return true;
}

void appendCapabilityWaiter(
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry,
    const std::shared_ptr<
        CodexRuntimeOperationState>& operation,
    const std::shared_ptr<
        CodexRuntimeCapabilityStopLink>& stopLink)
{
    if (entry == nullptr || operation == nullptr) {
        return;
    }
    if (stopLink != nullptr) {
        stopLink->bind(entry->stopSource);
    }
    const std::scoped_lock lock(entry->mutex);
    entry->waiters.append(operation);
}

QVector<
    std::shared_ptr<
        CodexRuntimeOperationState>>
takeCapabilityWaiters(
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry)
{
    if (entry == nullptr) {
        return {};
    }
    const std::scoped_lock lock(entry->mutex);
    return std::exchange(entry->waiters, {});
}

void finishCapabilityWaiters(
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry,
    const CompanionError& error)
{
    const auto waiters =
        takeCapabilityWaiters(entry);
    for (const auto& waiter : waiters) {
        if (waiter != nullptr) {
            waiter->finish(
                Result<void>::failure(error));
        }
    }
}

void finishWaiters(
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        waiters,
    const Result<void>& result)
{
    for (const auto& waiter : waiters) {
        if (waiter != nullptr) {
            waiter->finish(result);
        }
    }
}

} // namespace

void CodexRuntime::dispatchCapabilitiesCommand(
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
                            dispatchCapabilitiesCommand(
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
        owner->requestCapabilitiesOnOwnerThread(
            arguments,
            invocation);
    } catch (...) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    }
}

void CodexRuntime::dispatchUsageCommand(
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
                            dispatchUsageCommand(
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
        owner->requestUsageOnOwnerThread(
            arguments,
            invocation);
    } catch (...) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    }
}

void CodexRuntime::requestCapabilitiesOnOwnerThread(
    const QVariantMap& arguments,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation)
{
    if (!running_
        || deferredStop_
        || !readCommandsEnabled_
        || !capabilityLoader_
        || operationRegistry_ == nullptr) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }

    const Result<QString> parsed =
        parseCapabilityArguments(arguments);
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

    const auto stopLink =
        std::make_shared<
            CodexRuntimeCapabilityStopLink>();
    const auto operation =
        CodexRuntimeOperationState::createRead(
            [invocation](Result<void> result) {
                invocation->finish(
                    std::move(result));
            },
            generation_,
            nextGeneration(
                nextOperationGeneration_),
            [stopLink] {
                stopLink->requestStop();
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

    const QString cwd = parsed.value();
    latestCapabilityCwd_ = cwd;
    const bool cached =
        activeCapabilityEntry_ == nullptr
        && queuedCapabilityEntry_ == nullptr
        && capabilities_.has_value()
        && chatCapabilityValid_
        && publishedCapabilityCwd_
               == latestCapabilityCwd_
        && publishedCapabilityRevision_
               == std::optional<std::uint64_t>(
                      capabilityRevision_);
    if (cached) {
        const bool posted =
            QMetaObject::invokeMethod(
                this,
                [operation] {
                    operation->finish(
                        Result<void>::success());
                },
                Qt::QueuedConnection);
        if (!posted) {
            operation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
        }
        return;
    }

    enqueueCapabilityRequest(
        cwd,
        operation,
        stopLink,
        false);
}

void CodexRuntime::requestUsageOnOwnerThread(
    const QVariantMap& arguments,
    const std::shared_ptr<
        CodexRuntimeCommandInvocationState>&
        invocation)
{
    if (!running_
        || deferredStop_
        || !readCommandsEnabled_
        || !usageReadStarter_
        || continuationHost_ == nullptr
        || operationRegistry_ == nullptr) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
        return;
    }
    if (!arguments.isEmpty()) {
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

    const auto operation =
        CodexRuntimeOperationState::createRead(
            [invocation](Result<void> result) {
                invocation->finish(
                    std::move(result));
            },
            generation_,
            nextGeneration(
                nextOperationGeneration_),
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

    const bool statusChanged =
        !usageLoading_
        || !usageErrorCode_.isEmpty()
        || !usageErrorMessage_.isEmpty();
    usageLoading_ = true;
    usageErrorCode_.clear();
    usageErrorMessage_.clear();

    if (activeUsageEntry_ != nullptr) {
        queuedUsageWaiters_.append(operation);
        if (statusChanged) {
            emit usageChanged();
        }
        return;
    }

    const auto entry =
        std::make_shared<
            CodexRuntimeUsageEntry>();
    entry->runtimeGeneration = generation_;
    entry->entryGeneration =
        nextGeneration(
            nextReadEntryGeneration_);
    entry->waiters.append(operation);
    activeUsageEntry_ = entry;
    startUsageEntry(entry);
    if (statusChanged) {
        emit usageChanged();
    }
}

void CodexRuntime::scheduleUsageRefreshAfterMutation()
{
    if (!running_
        || deferredStop_
        || !readCommandsEnabled_
        || !usageReadStarter_
        || continuationHost_ == nullptr) {
        return;
    }

    const bool statusChanged =
        !usageLoading_
        || !usageErrorCode_.isEmpty()
        || !usageErrorMessage_.isEmpty();
    usageLoading_ = true;
    usageErrorCode_.clear();
    usageErrorMessage_.clear();

    if (activeUsageEntry_ != nullptr) {
        usageFollowUpRequested_ = true;
        if (statusChanged) {
            emit usageChanged();
        }
        return;
    }

    const auto entry =
        std::make_shared<
            CodexRuntimeUsageEntry>();
    entry->runtimeGeneration = generation_;
    entry->entryGeneration =
        nextGeneration(
            nextReadEntryGeneration_);
    activeUsageEntry_ = entry;
    startUsageEntry(entry);
    if (statusChanged) {
        emit usageChanged();
    }
}

void CodexRuntime::enqueueCapabilityRequest(
    QString cwd,
    std::shared_ptr<
        CodexRuntimeOperationState> operation,
    std::shared_ptr<
        CodexRuntimeCapabilityStopLink> stopLink,
    bool forceFresh)
{
    const auto makeEntry =
        [this](
            QString requestedCwd,
            std::shared_ptr<
                CodexRuntimeOperationState>
                requestedOperation,
            std::shared_ptr<
                CodexRuntimeCapabilityStopLink>
                requestedStopLink,
            bool requestedForceFresh) {
            auto entry =
                std::make_shared<
                    CodexRuntimeCapabilityEntry>();
            entry->cwd =
                std::move(requestedCwd);
            entry->runtimeGeneration =
                generation_;
            entry->entryGeneration =
                nextGeneration(
                    nextReadEntryGeneration_);
            entry->requiredRevision =
                capabilityRevision_;
            entry->forceFresh =
                requestedForceFresh;
            appendCapabilityWaiter(
                entry,
                requestedOperation,
                requestedStopLink);
            return entry;
        };

    const bool statusChanged =
        !capabilitiesLoading_
        || !capabilitiesErrorCode_.isEmpty()
        || !capabilitiesErrorMessage_.isEmpty();
    capabilitiesLoading_ = true;
    capabilitiesErrorCode_.clear();
    capabilitiesErrorMessage_.clear();

    if (activeCapabilityEntry_ == nullptr) {
        activeCapabilityEntry_ =
            makeEntry(
                std::move(cwd),
                std::move(operation),
                std::move(stopLink),
                forceFresh);
        startCapabilityEntry(
            activeCapabilityEntry_);
        if (statusChanged) {
            emit capabilitiesChanged();
        }
        return;
    }

    if (queuedCapabilityEntry_ != nullptr) {
        if (queuedCapabilityEntry_->cwd == cwd) {
            queuedCapabilityEntry_
                ->requiredRevision =
                std::max(
                    queuedCapabilityEntry_
                        ->requiredRevision,
                    capabilityRevision_);
            queuedCapabilityEntry_->forceFresh =
                queuedCapabilityEntry_
                    ->forceFresh
                || forceFresh;
            appendCapabilityWaiter(
                queuedCapabilityEntry_,
                operation,
                stopLink);
        } else {
            finishCapabilityWaiters(
                queuedCapabilityEntry_,
                supersededError());
            queuedCapabilityEntry_ =
                makeEntry(
                    std::move(cwd),
                    std::move(operation),
                    std::move(stopLink),
                    forceFresh);
        }
        if (statusChanged) {
            emit capabilitiesChanged();
        }
        return;
    }

    if (activeCapabilityEntry_->cwd == cwd
        && activeCapabilityEntry_
               ->publicationEligible
        && activeCapabilityEntry_
               ->requiredRevision
            == capabilityRevision_
        && !forceFresh) {
        appendCapabilityWaiter(
            activeCapabilityEntry_,
            operation,
            stopLink);
        if (statusChanged) {
            emit capabilitiesChanged();
        }
        return;
    }

    activeCapabilityEntry_
        ->publicationEligible = false;
    finishCapabilityWaiters(
        activeCapabilityEntry_,
        supersededError());
    activeCapabilityEntry_
        ->stopSource->request_stop();
    queuedCapabilityEntry_ =
        makeEntry(
            std::move(cwd),
            std::move(operation),
            std::move(stopLink),
            forceFresh);
    if (statusChanged) {
        emit capabilitiesChanged();
    }
}

void CodexRuntime::startCapabilityEntry(
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry)
{
    if (entry == nullptr) {
        return;
    }
    const RuntimeCapabilityLoader loader =
        capabilityLoader_;
    const RuntimeExecutor executor = executor_;
    const std::stop_token stopToken =
        entry->stopSource->get_token();
    const auto weakDelivery =
        std::weak_ptr<
            CodexRuntimeDeliveryState>(
            deliveryState_);
    QThread* const ownerThread = thread();
    const auto dispatchGate =
        std::make_shared<std::atomic_int>(
            static_cast<int>(
                ReadDispatchGateState::
                    Unclaimed));

    const auto complete =
        [entry,
         weakDelivery](
            Result<BridgeCapabilities> result) {
            if (!storeTerminalResult(
                    entry,
                    sanitizedCapabilityResult(
                        std::move(result)))) {
                return;
            }
            CodexRuntime::postCapabilityResult(
                weakDelivery,
                entry->runtimeGeneration,
                entry);
        };

    const auto worker =
        [loader,
         cwd = entry->cwd,
         stopToken,
         ownerThread,
         dispatchGate,
         complete] {
            int expected = static_cast<int>(
                ReadDispatchGateState::
                    Unclaimed);
            if (QThread::currentThread()
                == ownerThread) {
                if (dispatchGate
                        ->compare_exchange_strong(
                            expected,
                            static_cast<int>(
                                ReadDispatchGateState::
                                    Rejected))) {
                    complete(
                        Result<
                            BridgeCapabilities>::
                            failure(
                                capabilityFailure()));
                }
                return;
            }

            expected = static_cast<int>(
                ReadDispatchGateState::
                    Unclaimed);
            if (!dispatchGate
                     ->compare_exchange_strong(
                         expected,
                         static_cast<int>(
                             ReadDispatchGateState::
                                 Accepted))) {
                return;
            }

            Result<BridgeCapabilities> result =
                Result<BridgeCapabilities>::failure(
                    capabilityFailure());
            try {
                result = loader(cwd, stopToken);
            } catch (...) {
                result =
                    Result<BridgeCapabilities>::
                        failure(
                            capabilityFailure());
            }
            complete(std::move(result));
        };

    try {
        executor(worker);
    } catch (...) {
        int expected = static_cast<int>(
            ReadDispatchGateState::Unclaimed);
        if (dispatchGate
                ->compare_exchange_strong(
                    expected,
                    static_cast<int>(
                        ReadDispatchGateState::
                            Rejected))) {
            complete(
                Result<BridgeCapabilities>::
                    failure(
                        capabilityFailure()));
        }
    }
}

void CodexRuntime::applyCapabilityResult(
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry)
{
    if (entry == nullptr
        || !running_
        || runtimeGeneration != generation_
        || activeCapabilityEntry_ != entry) {
        return;
    }
    const TransitionGuard transition(*this);
    const CollaboratorStatus status =
        collaboratorStatus();
    if (status != CollaboratorStatus::Ready) {
        stopForRuntimeFailure(
            collaboratorError(status));
        return;
    }

    Result<BridgeCapabilities> result =
        Result<BridgeCapabilities>::failure(
            capabilityFailure());
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        waiters;
    {
        const std::scoped_lock lock(
            entry->mutex);
        waiters = std::exchange(
            entry->waiters,
            {});
        if (entry->terminalResult.has_value()) {
            result = sanitizedCapabilityResult(
                std::move(
                    *entry->terminalResult));
        }
    }

    const bool eligible =
        entry->publicationEligible
        && entry->requiredRevision
            == capabilityRevision_;
    activeCapabilityEntry_.reset();
    if (eligible) {
        if (result.hasValue()) {
            capabilities_ =
                result.value();
            publishedCapabilityCwd_ =
                entry->cwd;
            publishedCapabilityRevision_ =
                entry->requiredRevision;
            chatCapabilityValid_ = true;
            capabilitiesErrorCode_.clear();
            capabilitiesErrorMessage_.clear();
        } else {
            const CompanionError error =
                capabilityFailure();
            capabilitiesErrorCode_ =
                error.code;
            capabilitiesErrorMessage_ =
                error.message;
        }
    }

    finishWaiters(
        std::move(waiters),
        eligible && result.hasValue()
        ? Result<void>::success()
        : Result<void>::failure(
              eligible
              ? capabilityFailure()
              : supersededError()));

    if (queuedCapabilityEntry_ != nullptr) {
        activeCapabilityEntry_ =
            std::exchange(
                queuedCapabilityEntry_,
                {});
        startCapabilityEntry(
            activeCapabilityEntry_);
        capabilitiesLoading_ = true;
    } else {
        capabilitiesLoading_ = false;
    }
    emit capabilitiesChanged();
}

void CodexRuntime::postCapabilityResult(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeCapabilityEntry>& entry)
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
            if (!runtime.isNull()) {
                runtime->applyCapabilityResult(
                    runtimeGeneration,
                    entry);
            }
        },
        Qt::QueuedConnection);
}

void CodexRuntime::startUsageEntry(
    const std::shared_ptr<
        CodexRuntimeUsageEntry>& entry)
{
    if (entry == nullptr) {
        return;
    }
    const RuntimeUsageReadStarter starter =
        usageReadStarter_;
    const auto weakDelivery =
        std::weak_ptr<
            CodexRuntimeDeliveryState>(
            deliveryState_);
    const auto complete =
        [entry,
         weakDelivery](
            Result<BridgeUsageSnapshot> result) {
            if (!storeTerminalResult(
                    entry,
                    sanitizedUsageResult(
                        std::move(result)))) {
                return;
            }
            CodexRuntime::postUsageResult(
                weakDelivery,
                entry->runtimeGeneration,
                entry);
        };

    QFuture<Result<BridgeUsageSnapshot>> future;
    try {
        future = starter();
    } catch (...) {
        complete(
            Result<BridgeUsageSnapshot>::failure(
                usageFailure()));
        return;
    }
    if (!future.isValid()) {
        complete(
            Result<BridgeUsageSnapshot>::failure(
                usageFailure()));
        return;
    }

    const Result<void> submitted =
        continuationHost_->submit(
            [future,
             complete]() mutable {
                Result<BridgeUsageSnapshot> result =
                    Result<BridgeUsageSnapshot>::
                        failure(
                            usageFailure());
                try {
                    future.waitForFinished();
                    if (!future.isCanceled()
                        && future.resultCount() == 1) {
                        result =
                            sanitizedUsageResult(
                                future.result());
                    }
                } catch (...) {
                    result =
                        Result<
                            BridgeUsageSnapshot>::
                            failure(
                                usageFailure());
                }
                complete(std::move(result));
            });
    if (!submitted.hasValue()) {
        complete(
            Result<BridgeUsageSnapshot>::failure(
                usageFailure()));
    }
}

void CodexRuntime::applyUsageResult(
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeUsageEntry>& entry)
{
    if (entry == nullptr
        || !running_
        || runtimeGeneration != generation_
        || activeUsageEntry_ != entry) {
        return;
    }
    const TransitionGuard transition(*this);
    const CollaboratorStatus status =
        collaboratorStatus();
    if (status != CollaboratorStatus::Ready) {
        stopForRuntimeFailure(
            collaboratorError(status));
        return;
    }

    Result<BridgeUsageSnapshot> result =
        Result<BridgeUsageSnapshot>::failure(
            usageFailure());
    QVector<
        std::shared_ptr<
            CodexRuntimeOperationState>>
        waiters;
    {
        const std::scoped_lock lock(
            entry->mutex);
        waiters = std::exchange(
            entry->waiters,
            {});
        if (entry->terminalResult.has_value()) {
            result = sanitizedUsageResult(
                std::move(
                    *entry->terminalResult));
        }
    }

    activeUsageEntry_.reset();
    if (result.hasValue()) {
        usageSnapshot_ = result.value();
        usageErrorCode_.clear();
        usageErrorMessage_.clear();
    } else {
        const CompanionError error =
            usageFailure();
        usageErrorCode_ = error.code;
        usageErrorMessage_ = error.message;
    }
    finishWaiters(
        std::move(waiters),
        result.hasValue()
        ? Result<void>::success()
        : Result<void>::failure(
              usageFailure()));

    const bool followUpRequested =
        std::exchange(
            usageFollowUpRequested_,
            false);
    if (followUpRequested
        || !queuedUsageWaiters_.isEmpty()) {
        const auto followUp =
            std::make_shared<
                CodexRuntimeUsageEntry>();
        followUp->runtimeGeneration =
            generation_;
        followUp->entryGeneration =
            nextGeneration(
                nextReadEntryGeneration_);
        followUp->waiters =
            std::exchange(
                queuedUsageWaiters_,
                {});
        activeUsageEntry_ = followUp;
        usageLoading_ = true;
        startUsageEntry(followUp);
    } else {
        usageLoading_ = false;
    }
    emit usageChanged();
}

void CodexRuntime::postUsageResult(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    std::uint64_t runtimeGeneration,
    const std::shared_ptr<
        CodexRuntimeUsageEntry>& entry)
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
            if (!runtime.isNull()) {
                runtime->applyUsageResult(
                    runtimeGeneration,
                    entry);
            }
        },
        Qt::QueuedConnection);
}

void CodexRuntime::invalidateChatCapabilities()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [guard = QPointer<CodexRuntime>(this)] {
                if (!guard.isNull()) {
                    guard
                        ->invalidateChatCapabilities();
                }
            },
            Qt::QueuedConnection);
        return;
    }
    const TransitionGuard transition(*this);
    invalidateChatCapabilitiesOnOwnerThread();
}

void CodexRuntime::
invalidateChatCapabilitiesOnOwnerThread()
{
    nextGeneration(capabilityRevision_);
    chatCapabilityValid_ = false;
    emit capabilitiesChanged();
    if (!running_
        || deferredStop_
        || !readCommandsEnabled_
        || !latestCapabilityCwd_.has_value()) {
        return;
    }
    scheduleInvalidatedCapabilityReload();
}

void CodexRuntime::
scheduleInvalidatedCapabilityReload()
{
    if (!running_
        || deferredStop_
        || !readCommandsEnabled_
        || !latestCapabilityCwd_.has_value()
        || (chatCapabilityValid_
            && publishedCapabilityCwd_
                   == latestCapabilityCwd_
            && publishedCapabilityRevision_
                   == std::optional<
                          std::uint64_t>(
                          capabilityRevision_))) {
        return;
    }

    const QString cwd =
        *latestCapabilityCwd_;
    if (queuedCapabilityEntry_ != nullptr) {
        if (queuedCapabilityEntry_->cwd != cwd) {
            finishCapabilityWaiters(
                queuedCapabilityEntry_,
                supersededError());
            queuedCapabilityEntry_ =
                std::make_shared<
                    CodexRuntimeCapabilityEntry>();
            queuedCapabilityEntry_->cwd = cwd;
            queuedCapabilityEntry_
                ->runtimeGeneration =
                generation_;
            queuedCapabilityEntry_
                ->entryGeneration =
                nextGeneration(
                    nextReadEntryGeneration_);
        }
        queuedCapabilityEntry_
            ->requiredRevision =
            capabilityRevision_;
        queuedCapabilityEntry_->forceFresh =
            true;
        const bool statusChanged =
            !capabilitiesLoading_
            || !capabilitiesErrorCode_.isEmpty()
            || !capabilitiesErrorMessage_.isEmpty();
        capabilitiesLoading_ = true;
        capabilitiesErrorCode_.clear();
        capabilitiesErrorMessage_.clear();
        if (statusChanged) {
            emit capabilitiesChanged();
        }
        return;
    }

    if (activeCapabilityEntry_ != nullptr) {
        activeCapabilityEntry_
            ->publicationEligible = false;
        finishCapabilityWaiters(
            activeCapabilityEntry_,
            supersededError());
        activeCapabilityEntry_
            ->stopSource->request_stop();
        queuedCapabilityEntry_ =
            std::make_shared<
                CodexRuntimeCapabilityEntry>();
        queuedCapabilityEntry_->cwd = cwd;
        queuedCapabilityEntry_
            ->runtimeGeneration =
            generation_;
        queuedCapabilityEntry_
            ->entryGeneration =
            nextGeneration(
                nextReadEntryGeneration_);
        queuedCapabilityEntry_
            ->requiredRevision =
            capabilityRevision_;
        queuedCapabilityEntry_->forceFresh =
            true;
        capabilitiesLoading_ = true;
        capabilitiesErrorCode_.clear();
        capabilitiesErrorMessage_.clear();
        emit capabilitiesChanged();
        return;
    }

    enqueueCapabilityRequest(
        cwd,
        {},
        {},
        true);
}

void CodexRuntime::requestReadStops() noexcept
{
    try {
        if (activeCapabilityEntry_ != nullptr) {
            activeCapabilityEntry_
                ->publicationEligible = false;
            activeCapabilityEntry_
                ->stopSource->request_stop();
        }
        if (queuedCapabilityEntry_ != nullptr) {
            queuedCapabilityEntry_
                ->publicationEligible = false;
            queuedCapabilityEntry_
                ->stopSource->request_stop();
        }
    } catch (...) {
    }
}

void CodexRuntime::stopReadOperations()
{
    requestReadStops();
    activeCapabilityEntry_.reset();
    queuedCapabilityEntry_.reset();
    activeUsageEntry_.reset();
    queuedUsageWaiters_.clear();
    usageFollowUpRequested_ = false;

    if (capabilitiesLoading_) {
        capabilitiesLoading_ = false;
        emit capabilitiesChanged();
    }
    if (usageLoading_) {
        usageLoading_ = false;
        emit usageChanged();
    }
}

bool CodexRuntime::capabilitiesLoading()
    const noexcept
{
    return capabilitiesLoading_;
}

QString CodexRuntime::capabilitiesErrorCode() const
{
    return capabilitiesErrorCode_;
}

QString CodexRuntime::
capabilitiesErrorMessage() const
{
    return capabilitiesErrorMessage_;
}

const std::optional<BridgeCapabilities>&
CodexRuntime::capabilities() const noexcept
{
    return capabilities_;
}

bool CodexRuntime::usageLoading() const noexcept
{
    return usageLoading_;
}

QString CodexRuntime::usageErrorCode() const
{
    return usageErrorCode_;
}

QString CodexRuntime::usageErrorMessage() const
{
    return usageErrorMessage_;
}

const std::optional<BridgeUsageSnapshot>&
CodexRuntime::usageSnapshot() const noexcept
{
    return usageSnapshot_;
}

bool CodexRuntime::chatCapabilitiesValid()
    const noexcept
{
    return chatCapabilityValid_;
}

quint64 CodexRuntime::capabilityRevision()
    const noexcept
{
    return capabilityRevision_;
}

} // namespace companion
