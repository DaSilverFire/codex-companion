#pragma once

#include "codex/runtime/CodexRuntime.h"

#include <memory>

namespace companion {

class CompanionCommandBus;
class CompanionState;
class RuntimeContinuationHost;
class WindowsOnDeviceChatBackend;
class WindowsOnDeviceChatStatusSubscription;
struct CodexRuntimeInvalidationState;

class CodexRuntimeLifetime final {
public:
    static Result<
        std::unique_ptr<
            CodexRuntimeLifetime>>
    create(
        CodexRuntimeDependencies dependencies,
        CodexRuntimeCadence cadence = {});

    static Result<
        std::unique_ptr<
            CodexRuntimeLifetime>>
    createProduction(
        CodexRuntimeDependencies dependencies,
        std::shared_ptr<
            WindowsOnDeviceChatBackend>
            onDeviceBackend,
        CodexRuntimeCadence cadence = {});

    ~CodexRuntimeLifetime();

    CodexRuntimeLifetime(
        const CodexRuntimeLifetime&) = delete;
    CodexRuntimeLifetime& operator=(
        const CodexRuntimeLifetime&) = delete;

    RuntimeContinuationHost&
    continuationHost() noexcept;
    CompanionState& state() noexcept;
    CompanionCommandBus& commandBus() noexcept;
    CodexRuntime& runtime() noexcept;
    void notifyCredentialStateChanged();

private:
    CodexRuntimeLifetime(
        CodexRuntimeDependencies dependencies,
        CodexRuntimeCadence cadence,
        std::shared_ptr<
            WindowsOnDeviceChatBackend>
            onDeviceBackend = {});

    bool productionSubscriptionReady()
        const noexcept;

    std::shared_ptr<
        RuntimeContinuationHost>
        continuationHost_;
    std::unique_ptr<CompanionState> state_;
    std::unique_ptr<CompanionCommandBus>
        commandBus_;
    CodexRuntimeDependencies dependencies_;
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        onDeviceBackend_;
    std::shared_ptr<
        CodexRuntimeInvalidationState>
        invalidationState_;
    std::unique_ptr<CodexRuntime> runtime_;
    std::shared_ptr<
        WindowsOnDeviceChatStatusSubscription>
        backendStatusSubscription_;
    bool productionSubscriptionReady_ = true;
};

} // namespace companion
