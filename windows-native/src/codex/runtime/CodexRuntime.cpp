#include "codex/runtime/CodexRuntime.h"

#include "codex/commands/GoalService.h"
#include "codex/runtime/CodexRuntimeOperationRegistry.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "codex/runtime/TaskSnapshotLoader.h"
#include "codex/state/TaskProjector.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QMetaObject>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace companion {

static CompanionError runtimeUnavailableError()
{
    return {
        QStringLiteral("codex.runtime_unavailable"),
        QStringLiteral(
            "Codex runtime is unavailable."),
        false,
        {},
    };
}

CodexRuntimeCommandInvocationState::
CodexRuntimeCommandInvocationState(
    std::weak_ptr<CodexRuntimeDeliveryState>
        requestedDeliveryState,
    CompanionCommandBus::Completion
        requestedCompletion)
    : deliveryState(
          std::move(requestedDeliveryState)),
      completion(
          std::move(requestedCompletion))
{
}

CodexRuntimeCommandInvocationState::
~CodexRuntimeCommandInvocationState()
{
    try {
        finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    } catch (...) {
    }
}

void CodexRuntimeCommandInvocationState::finish(
    Result<void> result) noexcept
{
    bool expected = false;
    if (!finished.compare_exchange_strong(
            expected,
            true)) {
        return;
    }
    try {
        completion(std::move(result));
    } catch (...) {
    }
}

bool CodexRuntimeCommandInvocationState::
claimInvocation() noexcept
{
    bool expected = false;
    return invoked.compare_exchange_strong(
        expected,
        true);
}

namespace {

enum class DispatchGateState : int {
    Unclaimed = 0,
    Accepted = 1,
    Rejected = 2,
};

CompanionError threadMismatchError()
{
    return {
        QStringLiteral(
            "codex.runtime_thread_mismatch"),
        QStringLiteral(
            "Codex runtime components must share one thread."),
        false,
        {},
    };
}

CompanionError taskRefreshError()
{
    return {
        QStringLiteral("codex.refresh_failed"),
        QStringLiteral(
            "Could not refresh Codex tasks."),
        true,
        {},
    };
}

CompanionError goalRefreshError()
{
    return {
        QStringLiteral(
            "codex.goal_refresh_failed"),
        QStringLiteral(
            "Could not refresh Codex goals."),
        true,
        {},
    };
}

CompanionError invalidCommandArgumentsError()
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

std::shared_ptr<RuntimeContinuationHost>
legacyReadOnlyContinuationHost()
{
    static const auto host =
        std::make_shared<
            RuntimeContinuationHost>();
    return host;
}

bool isCanceled(const CompanionError& error)
{
    return error.code
        == QStringLiteral(
            "codex.operation_canceled");
}

void destroyTimer(QTimer*& timer)
{
    if (timer == nullptr) {
        return;
    }
    timer->stop();
    delete timer;
    timer = nullptr;
}

QDateTime normalizedUtc(QDateTime value)
{
    return value.isValid()
        ? value.toUTC()
        : QDateTime();
}

} // namespace

CodexRuntime::TransitionGuard::TransitionGuard(
    CodexRuntime& runtime)
    : runtime_(runtime)
{
    runtime_.beginTransition();
}

CodexRuntime::TransitionGuard::~TransitionGuard()
{
    runtime_.endTransition();
}

CodexRuntime::CodexRuntime(
    CompanionState& state,
    CompanionCommandBus& commandBus,
    const CodexEnvironment& environment,
    QObject* parent)
    : CodexRuntime(
          state,
          commandBus,
          [loader =
               std::make_shared<TaskSnapshotLoader>(
                   environment)](
              const QHash<QString, BridgeGoal>&
                  cachedGoals,
              std::stop_token stopToken) {
              return loader->load(
                  cachedGoals,
                  stopToken);
          },
          [service =
               std::make_shared<GoalService>(
                   environment)](
              const QVector<QString>& threadIds,
              std::stop_token stopToken) {
              return service->readSync(
                  threadIds,
                  stopToken);
          },
          [](std::function<void()> worker) {
              QThreadPool::globalInstance()->start(
                  std::move(worker));
          },
          [] {
              return QDateTime::currentDateTimeUtc();
          },
          {},
          parent)
{
}

CodexRuntime::CodexRuntime(
    CompanionState& state,
    CompanionCommandBus& commandBus,
    RuntimeTaskLoader taskLoader,
    RuntimeGoalLoader goalLoader,
    RuntimeExecutor executor,
    RuntimeNowProvider nowProvider,
    CodexRuntimeCadence cadence,
    QObject* parent)
    : CodexRuntime(
          state,
          commandBus,
          CodexRuntimeDependencies{
              std::move(taskLoader),
              std::move(goalLoader),
              std::move(executor),
              std::move(nowProvider),
          },
          legacyReadOnlyContinuationHost(),
          cadence,
          false,
          false,
          CodexRuntimeMode::ReadOnlyProbe,
          parent)
{
}

CodexRuntime::CodexRuntime(
    CompanionState& state,
    CompanionCommandBus& commandBus,
    CodexRuntimeDependencies dependencies,
    std::shared_ptr<
        RuntimeContinuationHost>
        continuationHost,
    CodexRuntimeCadence cadence,
    QObject* parent)
    : CodexRuntime(
          state,
          commandBus,
          std::move(dependencies),
          std::move(continuationHost),
          cadence,
          true,
          true,
          CodexRuntimeMode::Interactive,
          parent)
{
}

CodexRuntime::CodexRuntime(
    CompanionState& state,
    CompanionCommandBus& commandBus,
    CodexRuntimeDependencies dependencies,
    std::shared_ptr<
        RuntimeContinuationHost>
        continuationHost,
    CodexRuntimeMode mode,
    CodexRuntimeCadence cadence,
    QObject* parent)
    : CodexRuntime(
          state,
          commandBus,
          std::move(dependencies),
          std::move(continuationHost),
          cadence,
          true,
          true,
          mode,
          parent)
{
}

CodexRuntime::CodexRuntime(
    CompanionState& state,
    CompanionCommandBus& commandBus,
    CodexRuntimeDependencies dependencies,
    std::shared_ptr<
        RuntimeContinuationHost>
        continuationHost,
    CodexRuntimeCadence cadence,
    bool historyCommandsEnabled,
    bool readCommandsEnabled,
    CodexRuntimeMode mode,
    QObject* parent)
    : QObject(parent),
      state_(&state),
      stateAccessState_(state.accessState_),
      commandBusDeliveryState_(
          commandBus.deliveryState_),
      continuationHost_(
          std::move(continuationHost)),
      operationRegistry_(
          CodexRuntimeOperationRegistry::create()),
      taskLoader_(
          std::move(
              dependencies.taskLoader)),
      goalLoader_(
          std::move(
              dependencies.goalLoader)),
      executor_(
          std::move(
              dependencies.executor)),
      nowProvider_(
          std::move(
              dependencies.nowProvider)),
      historyLoader_(
          dependencies.history.has_value()
          ? std::move(
                dependencies.history
                    ->historyLoader)
          : RuntimeHistoryLoader()),
      historyCoordinator_(
          dependencies.history.has_value()
          ? std::move(
                dependencies.history
                    ->historyCoordinator)
          : std::shared_ptr<
                HistoryCoordinator>()),
      capabilityLoader_(
          dependencies.reads.has_value()
          ? std::move(
                dependencies.reads
                    ->capabilityLoader)
          : RuntimeCapabilityLoader()),
      usageReadStarter_(
          dependencies.reads.has_value()
          ? std::move(
                dependencies.reads
                    ->usageReadStarter)
          : RuntimeUsageReadStarter()),
      sendMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->sendMutationStarter)
          : RuntimeSendMutationStarter()),
      approvalMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->approvalMutationStarter)
          : RuntimeApprovalMutationStarter()),
      taskCreateMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->taskCreateMutationStarter)
          : RuntimeTaskCreateMutationStarter()),
      chatMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->chatMutationStarter)
          : RuntimeChatMutationStarter()),
      goalMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->goalMutationStarter)
          : RuntimeGoalMutationStarter()),
      usageResetMutationStarter_(
          dependencies.mutations.has_value()
          ? std::move(
                dependencies.mutations
                    ->usageResetMutationStarter)
          : RuntimeUsageResetMutationStarter()),
      cadence_(cadence),
      mode_(mode),
      deliveryState_(
          std::make_shared<
              CodexRuntimeDeliveryState>())
{
    historyCommandsEnabled_ =
        historyCommandsEnabled;
    readCommandsEnabled_ =
        readCommandsEnabled;
    mutationCommandsEnabled_ =
        dependencies.mutations.has_value();
    deliveryState_->runtime = this;
}

CodexRuntime::~CodexRuntime()
{
    Q_ASSERT(
        QThread::currentThread() == thread());
    requestHistoryStops();
    requestReadStops();
    if (operationRegistry_ != nullptr) {
        operationRegistry_
            ->requestRuntimeStop();
    }
    {
        const std::scoped_lock lock(
            deliveryState_->mutex);
        destroying_ = true;
        deliveryState_->destroying = true;
        deliveryState_->runtime.clear();
    }
    const TransitionGuard transition(*this);
    stopOnOwnerThread();
}

Result<void> CodexRuntime::start()
{
    if (QThread::currentThread() != thread()) {
        return Result<void>::failure(
            threadMismatchError());
    }

    if (transitionDepth_ > 0) {
        return running_
            ? Result<void>::success()
            : Result<void>::failure(
                  runtimeUnavailableError());
    }

    Result<void> result =
        Result<void>::failure(
            runtimeUnavailableError());
    {
        const TransitionGuard transition(*this);
        result = startOnOwnerThread();
    }
    if (result.hasValue() && !running_) {
        return Result<void>::failure(
            runtimeUnavailableError());
    }
    return result;
}

Result<void> CodexRuntime::startOnOwnerThread()
{
    if (!taskLoader_
        || !goalLoader_
        || !executor_
        || !nowProvider_
        || (historyCommandsEnabled_
            && (!historyLoader_
                || historyCoordinator_
                    == nullptr))
        || (readCommandsEnabled_
            && (!capabilityLoader_
                || !usageReadStarter_))
        || (mutationCommandsEnabled_
            && (!sendMutationStarter_
                || !approvalMutationStarter_
                || !taskCreateMutationStarter_
                || !chatMutationStarter_
                || !goalMutationStarter_
                || !usageResetMutationStarter_))
        || continuationHost_ == nullptr
        || !continuationHost_->accepting()
        || operationRegistry_ == nullptr) {
        const CompanionError error =
            runtimeUnavailableError();
        setError(error);
        return Result<void>::failure(error);
    }

    const CollaboratorStatus initialStatus =
        collaboratorStatus();
    if (initialStatus
        != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(initialStatus);
        setError(error);
        return Result<void>::failure(error);
    }

    const Result<void> binding =
        bindRuntimeCommandGroup();
    if (!binding.hasValue()) {
        const CompanionError error =
            runtimeUnavailableError();
        setError(error);
        return Result<void>::failure(error);
    }

    if (running_) {
        return Result<void>::success();
    }

    try {
        if (!normalizedUtc(nowProvider_()).isValid()) {
            const CompanionError error =
                runtimeUnavailableError();
            setError(error);
            return Result<void>::failure(error);
        }
    } catch (...) {
        const CompanionError error =
            runtimeUnavailableError();
        setError(error);
        return Result<void>::failure(error);
    }

    ++generation_;
    running_ = true;
    emit runningChanged();
    if (deferredStop_) {
        return Result<void>::success();
    }
    const CollaboratorStatus startedStatus =
        collaboratorStatus();
    if (startedStatus
        != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(startedStatus);
        stopForRuntimeFailure(error);
        return Result<void>::failure(error);
    }

    const Result<void> dispatched =
        startTaskRefresh(true);
    if (!dispatched.hasValue()) {
        if (!running_) {
            return dispatched;
        }
        stopTimers();
        if (taskStopSource_.has_value()) {
            taskStopSource_->request_stop();
        }
        if (goalStopSource_.has_value()) {
            goalStopSource_->request_stop();
        }
        taskStopSource_.reset();
        goalStopSource_.reset();
        taskRefreshActive_ = false;
        goalRefreshActive_ = false;
        taskFollowUpRequested_ = false;
        ++generation_;
        const CompanionError error =
            runtimeUnavailableError();
        publishStoppedFailure(error);
        return Result<void>::failure(error);
    }
    if (deferredStop_) {
        return Result<void>::success();
    }

    scheduleInvalidatedCapabilityReload();
    startPassiveTimer();
    if (processSurfaceVisible_) {
        startActiveTimer();
        scheduleSettleTimer();
    }

    if (processSurfaceVisible_) {
        evaluateGoalStaleness();
    }
    return Result<void>::success();
}

void CodexRuntime::stop()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                stop();
            },
            Qt::QueuedConnection);
        return;
    }
    if (transitionDepth_ > 0) {
        deferredStop_ = true;
        requestActiveStops();
        return;
    }
    const TransitionGuard transition(*this);
    stopOnOwnerThread();
}

void CodexRuntime::refreshNow()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                refreshNow();
            },
            Qt::QueuedConnection);
        return;
    }
    Q_UNUSED(requestRefreshOnOwnerThread());
}

void CodexRuntime::setProcessSurfaceVisible(
    bool visible)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this, visible] {
                setProcessSurfaceVisible(visible);
            },
            Qt::QueuedConnection);
        return;
    }
    if (transitionDepth_ > 0) {
        deferredProcessSurfaceVisible_ =
            visible;
        return;
    }
    const TransitionGuard transition(*this);
    setProcessSurfaceVisibleOnOwnerThread(visible);
}

void CodexRuntime::requestOperationStop(
    QString operationKey)
{
    operationKey = operationKey.trimmed();
    if (operationKey.isEmpty()) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this,
            [this,
             operationKey =
                 std::move(operationKey)] {
                requestOperationStop(
                    operationKey);
            },
            Qt::QueuedConnection);
        return;
    }
    if (operationRegistry_ != nullptr) {
        operationRegistry_->requestOperationStop(
            operationKey);
    }
}

bool CodexRuntime::running() const noexcept
{
    return running_;
}

bool CodexRuntime::loading() const noexcept
{
    return loading_;
}

QString CodexRuntime::errorCode() const
{
    return errorCode_;
}

QString CodexRuntime::errorMessage() const
{
    return errorMessage_;
}

const CodexProcessSnapshot&
CodexRuntime::processSnapshot() const noexcept
{
    return processSnapshot_;
}

bool CodexRuntime::historyLoading() const noexcept
{
    return historyLoading_;
}

QString CodexRuntime::historyErrorCode() const
{
    return historyErrorCode_;
}

QString CodexRuntime::historyErrorMessage() const
{
    return historyErrorMessage_;
}

QString CodexRuntime::selectedHistoryThreadId() const
{
    return selectedHistoryThreadId_;
}

const std::optional<CodexHistoryPublication>&
CodexRuntime::historyPublication() const noexcept
{
    return historyPublication_;
}

Result<void> CodexRuntime::startTaskRefresh(
    bool startup)
{
    if (!running_
        || taskRefreshActive_
        || deferredStop_) {
        return Result<void>::success();
    }

    taskRefreshActive_ = true;
    taskStopSource_.emplace();

    const std::uint64_t generation =
        generation_;
    const std::stop_token stopToken =
        taskStopSource_->get_token();
    const RuntimeTaskLoader loader =
        taskLoader_;
    const QHash<QString, BridgeGoal>
        cachedGoals = cachedGoals_;
    const RuntimeExecutor executor = executor_;
    const auto deliveryState =
        std::weak_ptr<CodexRuntimeDeliveryState>(
            deliveryState_);
    QThread* const ownerThread = thread();
    const auto dispatchGate =
        std::make_shared<std::atomic_int>(
            static_cast<int>(
                DispatchGateState::Unclaimed));

    publishTaskRefreshStarted();
    if (deferredStop_) {
        return Result<void>::success();
    }
    const CollaboratorStatus dispatchStatus =
        collaboratorStatus();
    if (dispatchStatus
        != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(dispatchStatus);
        stopForRuntimeFailure(error);
        return Result<void>::failure(error);
    }
    const auto callback =
        [loader,
         cachedGoals,
         stopToken,
         generation,
         deliveryState,
         ownerThread,
         dispatchGate] {
            int expected = static_cast<int>(
                DispatchGateState::Unclaimed);
            if (QThread::currentThread()
                == ownerThread) {
                if (dispatchGate
                        ->compare_exchange_strong(
                            expected,
                            static_cast<int>(
                                DispatchGateState::
                                    Rejected))) {
                    CodexRuntime::postTaskResult(
                        deliveryState,
                        generation,
                        Result<CodexProcessSnapshot>::
                            failure(
                                taskRefreshError()));
                }
                return;
            }

            expected = static_cast<int>(
                DispatchGateState::Unclaimed);
            if (!dispatchGate
                     ->compare_exchange_strong(
                         expected,
                         static_cast<int>(
                             DispatchGateState::
                                 Accepted))) {
                return;
            }

            Result<CodexProcessSnapshot> result =
                Result<CodexProcessSnapshot>::
                    failure(taskRefreshError());
            try {
                result = loader(
                    cachedGoals,
                    stopToken);
            } catch (...) {
                result =
                    Result<CodexProcessSnapshot>::
                        failure(taskRefreshError());
            }
            CodexRuntime::postTaskResult(
                deliveryState,
                generation,
                std::move(result));
        };

    try {
        executor(callback);
    } catch (...) {
        int expected = static_cast<int>(
            DispatchGateState::Unclaimed);
        if (dispatchGate->compare_exchange_strong(
                expected,
                static_cast<int>(
                    DispatchGateState::Rejected))) {
            return Result<void>::failure(
                startup
                    ? runtimeUnavailableError()
                    : taskRefreshError());
        }
    }

    if (dispatchGate->load()
        == static_cast<int>(
            DispatchGateState::Rejected)) {
        return Result<void>::failure(
            startup
                ? runtimeUnavailableError()
                : taskRefreshError());
    }
    return Result<void>::success();
}

Result<void> CodexRuntime::startGoalRefresh()
{
    if (!running_
        || goalRefreshActive_
        || deferredStop_) {
        return Result<void>::success();
    }
    const CollaboratorStatus dispatchStatus =
        collaboratorStatus();
    if (dispatchStatus
        != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(dispatchStatus);
        stopForRuntimeFailure(error);
        return Result<void>::failure(error);
    }

    QSet<QString> uniqueIds;
    for (const BridgeTask& task :
         processSnapshot_.tasks) {
        const QString id = task.id.trimmed();
        if (!id.isEmpty()) {
            uniqueIds.insert(id);
        }
    }
    for (const QString& candidateId :
         processSnapshot_.goalCandidateThreadIds) {
        const QString id = candidateId.trimmed();
        if (!id.isEmpty()) {
            uniqueIds.insert(id);
        }
    }
    QVector<QString> threadIds(
        uniqueIds.cbegin(),
        uniqueIds.cend());
    std::sort(
        threadIds.begin(),
        threadIds.end());
    if (threadIds.isEmpty()) {
        return Result<void>::success();
    }

    goalRefreshActive_ = true;
    goalStopSource_.emplace();

    const std::uint64_t generation =
        generation_;
    const std::uint64_t goalRevision =
        goalRevision_;
    const std::stop_token stopToken =
        goalStopSource_->get_token();
    const RuntimeGoalLoader loader =
        goalLoader_;
    const RuntimeExecutor executor = executor_;
    const auto deliveryState =
        std::weak_ptr<CodexRuntimeDeliveryState>(
            deliveryState_);
    QThread* const ownerThread = thread();
    const auto dispatchGate =
        std::make_shared<std::atomic_int>(
            static_cast<int>(
                DispatchGateState::Unclaimed));

    const auto callback =
        [loader,
         threadIds,
         stopToken,
         generation,
         goalRevision,
         deliveryState,
         ownerThread,
         dispatchGate] {
            int expected = static_cast<int>(
                DispatchGateState::Unclaimed);
            if (QThread::currentThread()
                == ownerThread) {
                if (dispatchGate
                        ->compare_exchange_strong(
                            expected,
                            static_cast<int>(
                                DispatchGateState::
                                    Rejected))) {
                    CodexRuntime::postGoalResult(
                        deliveryState,
                        generation,
                        goalRevision,
                        Result<GoalResultMap>::
                            failure(
                                goalRefreshError()));
                }
                return;
            }

            expected = static_cast<int>(
                DispatchGateState::Unclaimed);
            if (!dispatchGate
                     ->compare_exchange_strong(
                         expected,
                         static_cast<int>(
                             DispatchGateState::
                                 Accepted))) {
                return;
            }

            Result<GoalResultMap> result =
                Result<GoalResultMap>::failure(
                    goalRefreshError());
            try {
                result = loader(
                    threadIds,
                    stopToken);
            } catch (...) {
                result =
                    Result<GoalResultMap>::failure(
                        goalRefreshError());
            }
            CodexRuntime::postGoalResult(
                deliveryState,
                generation,
                goalRevision,
                std::move(result));
        };

    try {
        executor(callback);
    } catch (...) {
        int expected = static_cast<int>(
            DispatchGateState::Unclaimed);
        if (dispatchGate->compare_exchange_strong(
                expected,
                static_cast<int>(
                    DispatchGateState::Rejected))) {
            return Result<void>::failure(
                goalRefreshError());
        }
    }

    if (dispatchGate->load()
        == static_cast<int>(
            DispatchGateState::Rejected)) {
        return Result<void>::failure(
            goalRefreshError());
    }
    return Result<void>::success();
}

void CodexRuntime::applyTaskResult(
    std::uint64_t generation,
    Result<CodexProcessSnapshot> result)
{
    if (!running_
        || generation != generation_
        || !taskRefreshActive_) {
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

    taskRefreshActive_ = false;
    taskStopSource_.reset();
    bool shouldRefreshAgain =
        taskFollowUpRequested_;
    taskFollowUpRequested_ = false;

    if (!result.hasValue()
        && isCanceled(result.error())) {
        setLoading(false);
        return;
    }

    QDateTime completionTime;
    try {
        completionTime =
            normalizedUtc(nowProvider_());
    } catch (...) {
    }
    if (!completionTime.isValid()) {
        result =
            Result<CodexProcessSnapshot>::
                failure(taskRefreshError());
    }

    if (result.hasValue()) {
        CodexProcessSnapshot refreshed =
            std::move(result.value());
        QSet<QString> currentIds;
        currentIds.reserve(
            refreshed.tasks.size());
        for (const BridgeTask& task :
             refreshed.tasks) {
            const QString id = task.id.trimmed();
            if (!id.isEmpty()) {
                currentIds.insert(id);
            }
        }
        for (const QString& candidateId :
             refreshed.goalCandidateThreadIds) {
            const QString id =
                candidateId.trimmed();
            if (!id.isEmpty()) {
                currentIds.insert(id);
            }
        }

        auto cached = cachedGoals_.begin();
        while (cached != cachedGoals_.end()) {
            if (!currentIds.contains(
                    cached.key())) {
                cached =
                    cachedGoals_.erase(cached);
            } else {
                ++cached;
            }
        }

        for (BridgeTask& task :
             refreshed.tasks) {
            const QString id = task.id.trimmed();
            const auto goal =
                cachedGoals_.constFind(id);
            if (!id.isEmpty()
                && goal
                    != cachedGoals_.constEnd()) {
                task.goal = goal.value();
            } else if (!id.isEmpty()
                       && task.goal.has_value()) {
                cachedGoals_.insert(
                    id,
                    *task.goal);
            }
        }

        processSnapshot_ =
            std::move(refreshed);
        state_->tasks()->setSnapshot(
            processSnapshot_.tasks);
        clearError();
        emit processSnapshotChanged();
    } else {
        setError(taskRefreshError());
    }

    if (completionTime.isValid()) {
        lastTaskRefreshFinishedAt_ =
            completionTime;
    }
    setLoading(false);
    if (completionTime.isValid()) {
        evaluateGoalStaleness(
            completionTime);
    }
    if (deferredRefresh_) {
        shouldRefreshAgain = true;
        deferredRefresh_ = false;
    }
    if (shouldRefreshAgain
        && running_
        && !deferredStop_) {
        const Result<void> dispatched =
            startTaskRefresh(false);
        if (!dispatched.hasValue()) {
            applyTaskResult(
                generation_,
                Result<CodexProcessSnapshot>::
                    failure(
                        taskRefreshError()));
        }
    }
}

void CodexRuntime::applyGoalResult(
    std::uint64_t generation,
    std::uint64_t goalRevision,
    Result<GoalResultMap> result)
{
    if (!running_
        || generation != generation_
        || !goalRefreshActive_) {
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

    goalRefreshActive_ = false;
    goalStopSource_.reset();

    if (!result.hasValue()
        && isCanceled(result.error())) {
        return;
    }

    QDateTime completionTime;
    try {
        completionTime =
            normalizedUtc(nowProvider_());
    } catch (...) {
    }
    if (!completionTime.isValid()) {
        result =
            Result<GoalResultMap>::failure(
                goalRefreshError());
    }
    if (goalRevision != goalRevision_) {
        if (completionTime.isValid()) {
            lastGoalRefreshFinishedAt_ =
                completionTime;
        }
        return;
    }

    bool hiddenGoalNeedsProjection = false;
    if (result.hasValue()) {
        bool changed = false;
        QSet<QString> knownIds;
        for (const BridgeTask& task :
             processSnapshot_.tasks) {
            const QString id = task.id.trimmed();
            if (!id.isEmpty()) {
                knownIds.insert(id);
            }
        }
        for (const QString& candidateId :
             processSnapshot_
                 .goalCandidateThreadIds) {
            const QString id =
                candidateId.trimmed();
            if (!id.isEmpty()) {
                knownIds.insert(id);
            }
        }
        for (auto goalEntry =
                 result.value().cbegin();
             goalEntry
             != result.value().cend();
             ++goalEntry) {
            const QString id =
                goalEntry.key().trimmed();
            if (id.isEmpty()) {
                continue;
            }
            if (!knownIds.contains(id)) {
                continue;
            }

            bool isCurrent = false;
            for (BridgeTask& task :
                 processSnapshot_.tasks) {
                if (task.id.trimmed() != id) {
                    continue;
                }
                isCurrent = true;
                const auto runtime =
                    processSnapshot_
                        .runtimeStatuses
                        .constFind(id);
                const std::optional<
                    ThreadRuntimeStatus>
                    runtimeStatus =
                        runtime
                            != processSnapshot_
                                .runtimeStatuses
                                .constEnd()
                        ? std::optional<
                              ThreadRuntimeStatus>(
                              runtime.value())
                        : std::nullopt;
                BridgeTask updated =
                    TaskProjector::applyingGoal(
                        task,
                        goalEntry.value(),
                        runtimeStatus,
                        completionTime);
                if (task != updated) {
                    task = std::move(updated);
                    changed = true;
                }
            }
            if (goalEntry.value().has_value()) {
                const auto cached =
                    cachedGoals_.constFind(id);
                const bool cacheChanged =
                    cached
                        == cachedGoals_.constEnd()
                    || cached.value()
                        != *goalEntry.value();
                cachedGoals_.insert(
                    id,
                    *goalEntry.value());
                hiddenGoalNeedsProjection =
                    hiddenGoalNeedsProjection
                    || (!isCurrent
                        && cacheChanged);
            } else {
                cachedGoals_.remove(id);
            }
        }

        if (changed) {
            state_->tasks()->setSnapshot(
                processSnapshot_.tasks);
            emit processSnapshotChanged();
        }
        clearError();
    } else {
        setError(goalRefreshError());
    }

    if (completionTime.isValid()) {
        lastGoalRefreshFinishedAt_ =
            completionTime;
    }
    if (result.hasValue()
        && hiddenGoalNeedsProjection) {
        Q_UNUSED(
            requestRefreshOnOwnerThread());
    }
}

void CodexRuntime::evaluateGoalStaleness()
{
    if (!running_
        || goalRefreshActive_) {
        return;
    }

    QDateTime now;
    try {
        now = normalizedUtc(nowProvider_());
    } catch (...) {
    }
    if (!now.isValid()) {
        setError(goalRefreshError());
        return;
    }
    evaluateGoalStaleness(now);
}

void CodexRuntime::evaluateGoalStaleness(
    const QDateTime& now)
{
    if (!running_
        || goalRefreshActive_) {
        return;
    }

    const bool stale =
        !lastGoalRefreshFinishedAt_.has_value()
        || lastGoalRefreshFinishedAt_->msecsTo(now)
            >= cadence_.goalStaleMilliseconds;
    if (!stale) {
        return;
    }

    const Result<void> dispatched =
        startGoalRefresh();
    if (!dispatched.hasValue()
        && goalRefreshActive_) {
        applyGoalResult(
            generation_,
            goalRevision_,
            Result<GoalResultMap>::failure(
                goalRefreshError()));
    }
}

void CodexRuntime::dispatchRefreshCommand(
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
                        [invocation] {
                            dispatchRefreshCommand(
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
        if (!owner->running_) {
            invocation->finish(
                Result<void>::failure(
                    runtimeUnavailableError()));
            return;
        }
        invocation->finish(
            owner->requestRefreshOnOwnerThread());
    } catch (...) {
        invocation->finish(
            Result<void>::failure(
                runtimeUnavailableError()));
    }
}

void CodexRuntime::startPassiveTimer()
{
    destroyTimer(passiveTimer_);
    passiveTimer_ = new QTimer(this);
    passiveTimer_->setTimerType(Qt::PreciseTimer);
    passiveTimer_->setInterval(
        std::max(
            1,
            cadence_.passiveRefreshMilliseconds));
    connect(
        passiveTimer_,
        &QTimer::timeout,
        this,
        [this] {
            handlePassiveTimer();
        });
    passiveTimer_->start();
}

void CodexRuntime::startActiveTimer()
{
    destroyTimer(activeTimer_);
    if (!running_
        || !processSurfaceVisible_) {
        return;
    }
    activeTimer_ = new QTimer(this);
    activeTimer_->setTimerType(Qt::PreciseTimer);
    activeTimer_->setInterval(
        std::max(
            1,
            cadence_.activeRefreshMilliseconds));
    connect(
        activeTimer_,
        &QTimer::timeout,
        this,
        [this] {
            handleActiveTimer();
        });
    activeTimer_->start();
}

void CodexRuntime::scheduleSettleTimer()
{
    destroyTimer(settleTimer_);
    if (!running_
        || !processSurfaceVisible_) {
        return;
    }
    settleTimer_ = new QTimer(this);
    settleTimer_->setSingleShot(true);
    settleTimer_->setTimerType(Qt::PreciseTimer);
    settleTimer_->setInterval(
        std::max(
            0,
            cadence_.settleRefreshMilliseconds));
    connect(
        settleTimer_,
        &QTimer::timeout,
        this,
        [this] {
            handleSettleTimer();
        });
    settleTimer_->start();
}

void CodexRuntime::stopTimers()
{
    stopActiveAndSettleTimers();
    destroyTimer(passiveTimer_);
}

void CodexRuntime::stopActiveAndSettleTimers()
{
    destroyTimer(activeTimer_);
    destroyTimer(settleTimer_);
}

void CodexRuntime::handlePassiveTimer()
{
    const TransitionGuard transition(*this);
    if (running_
        && !processSurfaceVisible_) {
        refreshNowOnOwnerThread();
    }
}

void CodexRuntime::handleActiveTimer()
{
    const TransitionGuard transition(*this);
    if (!running_
        || !processSurfaceVisible_) {
        return;
    }
    refreshNowOnOwnerThread();
    evaluateGoalStaleness();
}

void CodexRuntime::handleSettleTimer()
{
    const TransitionGuard transition(*this);
    QTimer* const completedTimer =
        settleTimer_;
    settleTimer_ = nullptr;
    if (completedTimer != nullptr) {
        completedTimer->deleteLater();
    }
    if (!running_
        || !processSurfaceVisible_) {
        return;
    }

    QDateTime now;
    try {
        now = normalizedUtc(nowProvider_());
    } catch (...) {
    }
    if (!now.isValid()) {
        setError(taskRefreshError());
        return;
    }

    const bool taskStale =
        processSnapshot_.tasks.isEmpty()
        || !lastTaskRefreshFinishedAt_.has_value()
        || lastTaskRefreshFinishedAt_->msecsTo(now)
            >= cadence_.taskStaleMilliseconds;
    if (taskStale) {
        refreshNowOnOwnerThread();
    }
    evaluateGoalStaleness(now);
}

void CodexRuntime::stopOnOwnerThread()
{
    requestHistoryStops();
    requestReadStops();
    if (operationRegistry_ != nullptr) {
        operationRegistry_
            ->requestRuntimeStop();
    }
    stopHistoryOperations();
    stopReadOperations();
    if (!running_) {
        return;
    }

    stopTimers();
    ++generation_;
    if (taskStopSource_.has_value()) {
        taskStopSource_->request_stop();
    }
    if (goalStopSource_.has_value()) {
        goalStopSource_->request_stop();
    }
    taskStopSource_.reset();
    goalStopSource_.reset();
    taskRefreshActive_ = false;
    goalRefreshActive_ = false;
    taskFollowUpRequested_ = false;
    running_ = false;
    setLoading(false);
    emit runningChanged();
}

Result<void> CodexRuntime::requestRefreshOnOwnerThread()
{
    if (!running_ || deferredStop_) {
        return Result<void>::failure(
            runtimeUnavailableError());
    }
    const CollaboratorStatus status =
        collaboratorStatus();
    if (status != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(status);
        stopForRuntimeFailure(error);
        return Result<void>::failure(error);
    }
    if (transitionDepth_ > 0) {
        deferredRefresh_ = true;
        return Result<void>::success();
    }
    const TransitionGuard transition(*this);
    return refreshNowOnOwnerThread();
}

Result<void> CodexRuntime::refreshNowOnOwnerThread()
{
    if (!running_ || deferredStop_) {
        return Result<void>::failure(
            runtimeUnavailableError());
    }
    const CollaboratorStatus status =
        collaboratorStatus();
    if (status != CollaboratorStatus::Ready) {
        const CompanionError error =
            collaboratorError(status);
        stopForRuntimeFailure(error);
        return Result<void>::failure(error);
    }
    if (taskRefreshActive_) {
        taskFollowUpRequested_ = true;
        return Result<void>::success();
    }

    const Result<void> dispatched =
        startTaskRefresh(false);
    if (!dispatched.hasValue()
        && taskRefreshActive_) {
        applyTaskResult(
            generation_,
            Result<CodexProcessSnapshot>::
                failure(taskRefreshError()));
    }
    return dispatched;
}

void CodexRuntime::
setProcessSurfaceVisibleOnOwnerThread(bool visible)
{
    if (processSurfaceVisible_ == visible) {
        return;
    }
    processSurfaceVisible_ = visible;
    if (!running_) {
        return;
    }

    if (!visible) {
        stopActiveAndSettleTimers();
        return;
    }

    evaluateGoalStaleness();
    scheduleSettleTimer();
    startActiveTimer();
}

void CodexRuntime::beginTransition()
{
    ++transitionDepth_;
}

void CodexRuntime::endTransition()
{
    Q_ASSERT(transitionDepth_ > 0);
    --transitionDepth_;
    if (transitionDepth_ == 0
        && !drainingDeferredActions_) {
        drainDeferredOwnerActions();
    }
}

void CodexRuntime::drainDeferredOwnerActions()
{
    if (destroying_) {
        deferredStop_ = false;
        deferredRefresh_ = false;
        deferredProcessSurfaceVisible_.reset();
        return;
    }

    drainingDeferredActions_ = true;
    while (deferredStop_
           || deferredRefresh_
           || deferredProcessSurfaceVisible_
                  .has_value()) {
        const std::optional<bool> visible =
            deferredProcessSurfaceVisible_;
        const bool shouldStop = deferredStop_;
        const bool shouldRefresh =
            deferredRefresh_;
        deferredProcessSurfaceVisible_.reset();
        deferredStop_ = false;
        deferredRefresh_ = false;

        ++transitionDepth_;
        if (shouldStop) {
            stopOnOwnerThread();
            if (visible.has_value()) {
                setProcessSurfaceVisibleOnOwnerThread(
                    *visible);
            }
        } else {
            if (visible.has_value()) {
                setProcessSurfaceVisibleOnOwnerThread(
                    *visible);
            }
            if (shouldRefresh
                && !deferredStop_) {
                refreshNowOnOwnerThread();
            }
        }
        --transitionDepth_;
    }
    drainingDeferredActions_ = false;
}

void CodexRuntime::requestActiveStops() noexcept
{
    requestHistoryStops();
    requestReadStops();
    if (operationRegistry_ != nullptr) {
        operationRegistry_
            ->requestRuntimeStop();
    }
    if (taskStopSource_.has_value()) {
        taskStopSource_->request_stop();
    }
    if (goalStopSource_.has_value()) {
        goalStopSource_->request_stop();
    }
}

CodexRuntime::CollaboratorStatus
CodexRuntime::collaboratorStatus() const noexcept
{
    if (QThread::currentThread() != thread()) {
        return CollaboratorStatus::ThreadMismatch;
    }

    QThread* stateThread = nullptr;
    QThread* commandBusThread = nullptr;
    if (!CompanionState::tryGetOwnerThread(
            stateAccessState_,
            stateThread)
        || !CompanionCommandBus::tryGetOwnerThread(
            commandBusDeliveryState_,
            commandBusThread)) {
        return CollaboratorStatus::Unavailable;
    }
    if (stateThread != thread()
        || commandBusThread != thread()) {
        return CollaboratorStatus::ThreadMismatch;
    }
    return CollaboratorStatus::Ready;
}

CompanionError CodexRuntime::collaboratorError(
    CollaboratorStatus status)
{
    return status
            == CollaboratorStatus::ThreadMismatch
        ? threadMismatchError()
        : runtimeUnavailableError();
}

void CodexRuntime::stopForRuntimeFailure(
    const CompanionError& error)
{
    requestHistoryStops();
    requestReadStops();
    if (operationRegistry_ != nullptr) {
        operationRegistry_
            ->requestRuntimeStop();
    }
    stopHistoryOperations();
    stopReadOperations();
    stopTimers();
    ++generation_;
    if (taskStopSource_.has_value()) {
        taskStopSource_->request_stop();
    }
    if (goalStopSource_.has_value()) {
        goalStopSource_->request_stop();
    }
    taskStopSource_.reset();
    goalStopSource_.reset();
    taskRefreshActive_ = false;
    goalRefreshActive_ = false;
    taskFollowUpRequested_ = false;
    publishStoppedFailure(error);
}

void CodexRuntime::publishTaskRefreshStarted()
{
    const bool loadingDidChange = !loading_;
    const bool statusDidChange =
        !errorCode_.isEmpty()
        || !errorMessage_.isEmpty();
    loading_ = true;
    errorCode_.clear();
    errorMessage_.clear();
    if (loadingDidChange) {
        emit loadingChanged();
    }
    if (statusDidChange) {
        emit statusChanged();
    }
}

void CodexRuntime::publishStoppedFailure(
    const CompanionError& error)
{
    const bool runningDidChange = running_;
    const bool loadingDidChange = loading_;
    const bool statusDidChange =
        errorCode_ != error.code
        || errorMessage_ != error.message;
    running_ = false;
    loading_ = false;
    errorCode_ = error.code;
    errorMessage_ = error.message;
    if (loadingDidChange) {
        emit loadingChanged();
    }
    if (runningDidChange) {
        emit runningChanged();
    }
    if (statusDidChange) {
        emit statusChanged();
    }
}

void CodexRuntime::setLoading(bool loading)
{
    if (loading_ == loading) {
        return;
    }
    loading_ = loading;
    emit loadingChanged();
}

void CodexRuntime::setError(
    const CompanionError& error)
{
    if (errorCode_ == error.code
        && errorMessage_ == error.message) {
        return;
    }
    errorCode_ = error.code;
    errorMessage_ = error.message;
    emit statusChanged();
}

void CodexRuntime::clearError()
{
    if (errorCode_.isEmpty()
        && errorMessage_.isEmpty()) {
        return;
    }
    errorCode_.clear();
    errorMessage_.clear();
    emit statusChanged();
}

void CodexRuntime::postTaskResult(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    std::uint64_t generation,
    Result<CodexProcessSnapshot> result)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }
    const auto sharedResult =
        std::make_shared<
            Result<CodexProcessSnapshot>>(
            std::move(result));

    const std::scoped_lock lock(state->mutex);
    if (state->destroying
        || state->runtime.isNull()) {
        return;
    }
    QMetaObject::invokeMethod(
        state->runtime.data(),
        [weakState,
         generation,
         sharedResult] {
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
            runtime->applyTaskResult(
                generation,
                std::move(*sharedResult));
        },
        Qt::QueuedConnection);
}

void CodexRuntime::postGoalResult(
    const std::weak_ptr<
        CodexRuntimeDeliveryState>& weakState,
    std::uint64_t generation,
    std::uint64_t goalRevision,
    Result<GoalResultMap> result)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }
    const auto sharedResult =
        std::make_shared<Result<GoalResultMap>>(
            std::move(result));

    const std::scoped_lock lock(state->mutex);
    if (state->destroying
        || state->runtime.isNull()) {
        return;
    }
    QMetaObject::invokeMethod(
        state->runtime.data(),
        [weakState,
         generation,
         goalRevision,
         sharedResult] {
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
            runtime->applyGoalResult(
                generation,
                goalRevision,
                std::move(*sharedResult));
        },
        Qt::QueuedConnection);
}

} // namespace companion
