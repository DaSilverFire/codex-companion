#include "codex/runtime/CodexRuntimeOperationState.h"

#include "codex/runtime/CodexRuntimeOperationRegistry.h"

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

CompanionError defaultObservationFailure()
{
    return {
        QStringLiteral(
            "codex.runtime_operation_failed"),
        QStringLiteral(
            "Codex operation failed."),
        false,
        {},
    };
}

CompanionError normalizedObservationFailure(
    CompanionError error)
{
    if (error.code.trimmed().isEmpty()) {
        return defaultObservationFailure();
    }
    error.context.clear();
    return error;
}

} // namespace

std::shared_ptr<CodexRuntimeOperationState>
CodexRuntimeOperationState::createRead(
    Completion completion,
    std::uint64_t runtimeGeneration,
    std::uint64_t operationGeneration,
    StopRequest requestStop)
{
    if (!completion || !requestStop) {
        return {};
    }
    return std::shared_ptr<
        CodexRuntimeOperationState>(
        new CodexRuntimeOperationState(
            Kind::Read,
            std::move(completion),
            runtimeGeneration,
            operationGeneration,
            std::move(requestStop)));
}

std::shared_ptr<CodexRuntimeOperationState>
CodexRuntimeOperationState::createMutation(
    Completion completion,
    std::uint64_t runtimeGeneration,
    std::uint64_t operationGeneration)
{
    if (!completion) {
        return {};
    }
    return std::shared_ptr<
        CodexRuntimeOperationState>(
        new CodexRuntimeOperationState(
            Kind::Mutation,
            std::move(completion),
            runtimeGeneration,
            operationGeneration,
            {}));
}

CodexRuntimeOperationState::
CodexRuntimeOperationState(
    Kind kind,
    Completion completion,
    std::uint64_t runtimeGeneration,
    std::uint64_t operationGeneration,
    StopRequest readStopRequest)
    : kind_(kind),
      completion_(std::move(completion)),
      readStopRequest_(std::move(readStopRequest)),
      runtimeGeneration_(runtimeGeneration),
      operationGeneration_(operationGeneration)
{
}

CodexRuntimeOperationState::
~CodexRuntimeOperationState()
{
    finish(
        Result<void>::failure(
            runtimeUnavailableError()));
}

bool CodexRuntimeOperationState::finish(
    Result<void> result) noexcept
{
    Completion completion;
    std::weak_ptr<
        CodexRuntimeOperationRegistry> registry;
    quint64 operationId = 0;
    try {
        {
            const std::scoped_lock lock(mutex_);
            if (terminal_) {
                return false;
            }
            terminal_ = true;
            completion =
                std::move(completion_);
            registry = registry_;
            operationId = operationId_;
            readStopRequest_ = {};
            mutationObservation_.reset();
        }
        condition_.notify_all();

        if (const auto owner = registry.lock()) {
            owner->removeOperation(
                operationId,
                this);
        }
        if (completion) {
            try {
                completion(std::move(result));
            } catch (...) {
            }
        }
        return true;
    } catch (...) {
        condition_.notify_all();
        return false;
    }
}

void CodexRuntimeOperationState::
requestRuntimeStop() noexcept
{
    StopRequest stopRequest;
    Completion readCompletion;
    std::weak_ptr<
        CodexRuntimeOperationRegistry>
        readRegistry;
    quint64 readOperationId = 0;
    bool readTerminalClaimed = false;
    try {
        {
            const std::scoped_lock lock(mutex_);
            if (terminal_
                || runtimeStopRequested_) {
                return;
            }
            runtimeStopRequested_ = true;
            if (kind_ == Kind::Read) {
                terminal_ = true;
                readTerminalClaimed = true;
                stopRequest =
                    std::move(readStopRequest_);
                readCompletion =
                    std::move(completion_);
                readRegistry = registry_;
                readOperationId = operationId_;
                mutationObservation_.reset();
            } else if (
                mutationPhase_
                    == MutationPhase::Installed
                && mutationObservation_.has_value()
                && !stopCallbackInvoked_) {
                stopCallbackInvoked_ = true;
                stopRequest =
                    mutationObservation_
                        ->requestStopBeforeCommit;
            }
        }

        if (readTerminalClaimed) {
            condition_.notify_all();
            if (const auto owner =
                    readRegistry.lock()) {
                owner->removeOperation(
                    readOperationId,
                    this);
            }
        }
        if (stopRequest) {
            try {
                stopRequest();
            } catch (...) {
                recordStopCallbackFailure();
            }
        }
        if (readTerminalClaimed
            && readCompletion) {
            try {
                readCompletion(
                    Result<void>::failure(
                        runtimeUnavailableError()));
            } catch (...) {
            }
        }
    } catch (...) {
        condition_.notify_all();
        if (readTerminalClaimed
            && readCompletion) {
            try {
                readCompletion(
                    Result<void>::failure(
                        runtimeUnavailableError()));
            } catch (...) {
            }
        }
    }
}

bool CodexRuntimeOperationState::
installMutationObservation(
    CodexRuntimeMutationObservation observation)
{
    if (!observation.requestStopBeforeCommit
        || !observation.waitForTerminal) {
        return false;
    }
    observation.observationFailure =
        normalizedObservationFailure(
            std::move(
                observation.observationFailure));

    StopRequest pendingStop;
    {
        const std::scoped_lock lock(mutex_);
        if (kind_ != Kind::Mutation
            || terminal_
            || mutationPhase_
                != MutationPhase::AwaitingHandle) {
            return false;
        }
        mutationObservation_ =
            std::move(observation);
        mutationPhase_ =
            MutationPhase::Installed;
        if (runtimeStopRequested_
            && !stopCallbackInvoked_) {
            stopCallbackInvoked_ = true;
            pendingStop =
                mutationObservation_
                    ->requestStopBeforeCommit;
        }
    }
    condition_.notify_all();

    if (pendingStop) {
        try {
            pendingStop();
        } catch (...) {
            recordStopCallbackFailure();
        }
    }
    return true;
}

bool CodexRuntimeOperationState::
finishBeforeMutationHandle(
    Result<void> result) noexcept
{
    try {
        {
            const std::scoped_lock lock(mutex_);
            if (kind_ != Kind::Mutation
                || terminal_
                || mutationPhase_
                    != MutationPhase::AwaitingHandle) {
                return false;
            }
            mutationPhase_ =
                MutationPhase::FailedBeforeHandle;
        }
        return finish(std::move(result));
    } catch (...) {
        return false;
    }
}

void CodexRuntimeOperationState::
observeMutationTerminal() noexcept
{
    std::function<void(
        CodexRuntimeOperationState&)> observer;
    CompanionError observationFailure =
        defaultObservationFailure();
    try {
        {
            std::unique_lock lock(mutex_);
            condition_.wait(
                lock,
                [this] {
                    return terminal_
                        || mutationPhase_
                            != MutationPhase::
                                AwaitingHandle;
                });
            if (terminal_
                || mutationPhase_
                    != MutationPhase::Installed
                || observationClaimed_
                || !mutationObservation_
                        .has_value()) {
                return;
            }
            observationClaimed_ = true;
            observer =
                mutationObservation_
                    ->waitForTerminal;
            observationFailure =
                mutationObservation_
                    ->observationFailure;
        }

        try {
            observer(*this);
        } catch (...) {
            finish(
                Result<void>::failure(
                    observationFailure));
            return;
        }
        if (!terminal()) {
            finish(
                Result<void>::failure(
                    observationFailure));
        }
    } catch (...) {
        finish(
            Result<void>::failure(
                observationFailure));
    }
}

std::uint64_t CodexRuntimeOperationState::
runtimeGeneration() const noexcept
{
    return runtimeGeneration_;
}

std::uint64_t CodexRuntimeOperationState::
operationGeneration() const noexcept
{
    return operationGeneration_;
}

quint64 CodexRuntimeOperationState::
operationId() const noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        return operationId_;
    } catch (...) {
        return 0;
    }
}

bool CodexRuntimeOperationState::terminal() const noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        return terminal_;
    } catch (...) {
        return true;
    }
}

bool CodexRuntimeOperationState::
stopCallbackFailed() const noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        return stopCallbackFailed_;
    } catch (...) {
        return true;
    }
}

bool CodexRuntimeOperationState::bindRegistry(
    std::weak_ptr<
        CodexRuntimeOperationRegistry> registry,
    quint64 operationId) noexcept
{
    if (operationId == 0 || registry.expired()) {
        return false;
    }
    try {
        const std::scoped_lock lock(mutex_);
        if (terminal_ || operationId_ != 0) {
            return false;
        }
        registry_ = std::move(registry);
        operationId_ = operationId;
        return true;
    } catch (...) {
        return false;
    }
}

void CodexRuntimeOperationState::
recordStopCallbackFailure() noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        stopCallbackFailed_ = true;
    } catch (...) {
    }
}

} // namespace companion
