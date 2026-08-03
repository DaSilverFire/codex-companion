#pragma once

#include "core/Result.h"

#include <QtGlobal>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace companion {

class CodexRuntimeOperationRegistry;
class CodexRuntimeOperationState;

struct CodexRuntimeMutationObservation final {
    std::function<void()> requestStopBeforeCommit;
    std::function<void(
        CodexRuntimeOperationState&)> waitForTerminal;
    CompanionError observationFailure;
};

class CodexRuntimeOperationState final {
public:
    using Completion =
        std::function<void(Result<void>)>;
    using StopRequest = std::function<void()>;

    static std::shared_ptr<
        CodexRuntimeOperationState>
    createRead(
        Completion completion,
        std::uint64_t runtimeGeneration,
        std::uint64_t operationGeneration,
        StopRequest requestStop);

    static std::shared_ptr<
        CodexRuntimeOperationState>
    createMutation(
        Completion completion,
        std::uint64_t runtimeGeneration,
        std::uint64_t operationGeneration);

    ~CodexRuntimeOperationState();

    CodexRuntimeOperationState(
        const CodexRuntimeOperationState&) = delete;
    CodexRuntimeOperationState& operator=(
        const CodexRuntimeOperationState&) = delete;

    bool finish(Result<void> result) noexcept;
    void requestRuntimeStop() noexcept;

    bool installMutationObservation(
        CodexRuntimeMutationObservation observation);
    bool finishBeforeMutationHandle(
        Result<void> result) noexcept;
    void observeMutationTerminal() noexcept;

    std::uint64_t runtimeGeneration() const noexcept;
    std::uint64_t operationGeneration() const noexcept;
    quint64 operationId() const noexcept;
    bool terminal() const noexcept;
    bool stopCallbackFailed() const noexcept;

private:
    enum class Kind {
        Read,
        Mutation,
    };

    enum class MutationPhase {
        AwaitingHandle,
        Installed,
        FailedBeforeHandle,
    };

    CodexRuntimeOperationState(
        Kind kind,
        Completion completion,
        std::uint64_t runtimeGeneration,
        std::uint64_t operationGeneration,
        StopRequest readStopRequest);

    bool bindRegistry(
        std::weak_ptr<
            CodexRuntimeOperationRegistry> registry,
        quint64 operationId) noexcept;
    void recordStopCallbackFailure() noexcept;

    friend class CodexRuntimeOperationRegistry;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    Kind kind_;
    Completion completion_;
    StopRequest readStopRequest_;
    std::optional<
        CodexRuntimeMutationObservation>
        mutationObservation_;
    std::weak_ptr<
        CodexRuntimeOperationRegistry>
        registry_;
    const std::uint64_t runtimeGeneration_;
    const std::uint64_t operationGeneration_;
    quint64 operationId_ = 0;
    MutationPhase mutationPhase_ =
        MutationPhase::AwaitingHandle;
    bool terminal_ = false;
    bool runtimeStopRequested_ = false;
    bool stopCallbackInvoked_ = false;
    bool stopCallbackFailed_ = false;
    bool observationClaimed_ = false;
};

} // namespace companion
