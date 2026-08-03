#pragma once

#include "codex/chat/WindowsOnDeviceChatBackendInternal.h"

#include <functional>
#include <memory>
#include <mutex>

namespace companion::detail {

using WindowsOnDeviceChatDriverFactory =
    std::function<Result<std::shared_ptr<
        WindowsOnDeviceChatDriver>>()>;

class WindowsOnDeviceChatBackendRegistry final {
public:
    explicit WindowsOnDeviceChatBackendRegistry(
        WindowsOnDeviceChatDriverFactory
            driverFactory);
    ~WindowsOnDeviceChatBackendRegistry();

    Result<std::shared_ptr<
        WindowsOnDeviceChatBackend>>
    acquire();
    void shutdownForProcessExit() noexcept;

private:
    enum class Phase {
        Running,
        Stopping,
        Stopped,
    };

    std::mutex mutex_;
    WindowsOnDeviceChatDriverFactory
        driverFactory_;
    std::shared_ptr<
        WindowsOnDeviceChatBackend>
        owner_;
    Phase phase_ = Phase::Running;
};

} // namespace companion::detail
