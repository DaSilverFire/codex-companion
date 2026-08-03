#include "codex/runtime/CodexRuntimeLifetime.h"

#include "codex/chat/WindowsOnDeviceChatBackend.h"
#include "codex/runtime/RuntimeContinuationHost.h"
#include "core/CompanionCommandBus.h"
#include "core/CompanionState.h"

#include <QMetaObject>
#include <QPointer>

#include <mutex>
#include <optional>
#include <utility>

namespace companion {

struct CodexRuntimeInvalidationState final {
    std::mutex mutex;
    QPointer<CodexRuntime> runtime;
    quint64 latestBackendRevision = 0;
    bool active = true;
};

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

bool validBaseDependencies(
    const CodexRuntimeDependencies& dependencies)
{
    return dependencies.taskLoader
        && dependencies.goalLoader
        && dependencies.executor
        && dependencies.nowProvider
        && dependencies.history.has_value()
        && dependencies.history->historyLoader
        && dependencies.history
               ->historyCoordinator
            != nullptr
        && dependencies.reads.has_value()
        && dependencies.reads
               ->capabilityLoader
        && dependencies.reads
               ->usageReadStarter;
}

bool validProductionDependencies(
    const CodexRuntimeDependencies& dependencies)
{
    return validBaseDependencies(dependencies)
        && dependencies.mutations.has_value()
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

void queueCapabilityInvalidation(
    const std::weak_ptr<
        CodexRuntimeInvalidationState>&
        weakState,
    std::optional<quint64> backendRevision)
{
    const auto state = weakState.lock();
    if (state == nullptr) {
        return;
    }

    QPointer<CodexRuntime> runtime;
    {
        const std::scoped_lock lock(
            state->mutex);
        if (!state->active
            || state->runtime.isNull()) {
            return;
        }
        if (backendRevision.has_value()) {
            if (*backendRevision
                <= state
                       ->latestBackendRevision) {
                return;
            }
            state->latestBackendRevision =
                *backendRevision;
        }
        runtime = state->runtime;
        QMetaObject::invokeMethod(
            runtime.data(),
            [weakState] {
                const auto queuedState =
                    weakState.lock();
                if (queuedState == nullptr) {
                    return;
                }
                QPointer<CodexRuntime>
                    queuedRuntime;
                {
                    const std::scoped_lock lock(
                        queuedState->mutex);
                    if (!queuedState->active
                        || queuedState
                               ->runtime
                               .isNull()) {
                        return;
                    }
                    queuedRuntime =
                        queuedState->runtime;
                }
                queuedRuntime
                    ->invalidateChatCapabilities();
            },
            Qt::QueuedConnection);
    }
}

} // namespace

Result<std::unique_ptr<CodexRuntimeLifetime>>
CodexRuntimeLifetime::create(
    CodexRuntimeDependencies dependencies,
    CodexRuntimeCadence cadence)
{
    if (!validBaseDependencies(dependencies)) {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::failure(
            runtimeUnavailableError());
    }
    try {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::success(
            std::unique_ptr<
                CodexRuntimeLifetime>(
                new CodexRuntimeLifetime(
                    std::move(dependencies),
                    cadence,
                    {})));
    } catch (...) {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::failure(
            runtimeUnavailableError());
    }
}

Result<std::unique_ptr<CodexRuntimeLifetime>>
CodexRuntimeLifetime::createProduction(
    CodexRuntimeDependencies dependencies,
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        onDeviceBackend,
    CodexRuntimeCadence cadence)
{
    if (!validProductionDependencies(
            dependencies)
        || onDeviceBackend == nullptr) {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::failure(
            runtimeUnavailableError());
    }
    try {
        auto lifetime =
            std::unique_ptr<
                CodexRuntimeLifetime>(
                new CodexRuntimeLifetime(
                    std::move(dependencies),
                    cadence,
                    std::move(
                        onDeviceBackend)));
        if (!lifetime
                 ->productionSubscriptionReady()) {
            return Result<
                std::unique_ptr<
                    CodexRuntimeLifetime>>::failure(
                runtimeUnavailableError());
        }
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::success(
            std::move(lifetime));
    } catch (...) {
        return Result<
            std::unique_ptr<
                CodexRuntimeLifetime>>::failure(
            runtimeUnavailableError());
    }
}

CodexRuntimeLifetime::CodexRuntimeLifetime(
    CodexRuntimeDependencies dependencies,
    CodexRuntimeCadence cadence,
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        onDeviceBackend)
    : continuationHost_(
          std::make_shared<
              RuntimeContinuationHost>()),
      state_(
          std::make_unique<CompanionState>()),
      commandBus_(
          std::make_unique<
              CompanionCommandBus>()),
      dependencies_(std::move(dependencies)),
      onDeviceBackend_(
          std::move(onDeviceBackend)),
      invalidationState_(
          std::make_shared<
              CodexRuntimeInvalidationState>()),
      runtime_(
          std::make_unique<CodexRuntime>(
              *state_,
              *commandBus_,
              dependencies_,
              continuationHost_,
              cadence))
{
    invalidationState_->runtime =
        runtime_.get();
    if (onDeviceBackend_ == nullptr) {
        return;
    }
    try {
        invalidationState_
            ->latestBackendRevision =
            onDeviceBackend_->status().revision;
        backendStatusSubscription_ =
            onDeviceBackend_->subscribeStatus(
                [weakState =
                     std::weak_ptr<
                         CodexRuntimeInvalidationState>(
                         invalidationState_)](
                    WindowsOnDeviceChatStatus
                        status) {
                    queueCapabilityInvalidation(
                        weakState,
                        status.revision);
                });
        productionSubscriptionReady_ =
            backendStatusSubscription_
            != nullptr;
    } catch (...) {
        productionSubscriptionReady_ =
            false;
    }
}

CodexRuntimeLifetime::~CodexRuntimeLifetime()
{
    if (invalidationState_ != nullptr) {
        const std::scoped_lock lock(
            invalidationState_->mutex);
        invalidationState_->active = false;
        invalidationState_->runtime.clear();
    }
    backendStatusSubscription_.reset();
    if (runtime_ != nullptr) {
        runtime_->stop();
        runtime_.reset();
    }
    if (continuationHost_ != nullptr) {
        continuationHost_
            ->stopAcceptingAndDrain();
    }
    commandBus_.reset();
    state_.reset();
    dependencies_ = {};
    onDeviceBackend_.reset();
    invalidationState_.reset();
    continuationHost_.reset();
}

RuntimeContinuationHost&
CodexRuntimeLifetime::
continuationHost() noexcept
{
    return *continuationHost_;
}

CompanionState&
CodexRuntimeLifetime::state() noexcept
{
    return *state_;
}

CompanionCommandBus&
CodexRuntimeLifetime::commandBus() noexcept
{
    return *commandBus_;
}

CodexRuntime&
CodexRuntimeLifetime::runtime() noexcept
{
    return *runtime_;
}

void CodexRuntimeLifetime::
notifyCredentialStateChanged()
{
    queueCapabilityInvalidation(
        invalidationState_,
        std::nullopt);
}

bool CodexRuntimeLifetime::
productionSubscriptionReady()
    const noexcept
{
    return productionSubscriptionReady_;
}

} // namespace companion
