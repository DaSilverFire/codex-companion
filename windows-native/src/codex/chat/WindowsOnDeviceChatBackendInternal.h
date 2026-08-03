#pragma once

#include "codex/chat/WindowsOnDeviceChatBackend.h"

#include <functional>
#include <memory>
#include <stop_token>

namespace companion::detail {

using WindowsOnDevicePreparationObserver =
    std::function<void(
        WindowsOnDeviceChatPhase phase,
        double progressPercent)>;

class WindowsOnDeviceChatDriver {
public:
    virtual ~WindowsOnDeviceChatDriver() =
        default;

    virtual Result<void> prepare(
        WindowsOnDevicePreparationObserver
            observer,
        std::stop_token stopToken) = 0;
    virtual Result<ChatResult> send(
        const ChatRequest& request) = 0;
    virtual void shutdown() noexcept = 0;
};

Result<std::shared_ptr<
    WindowsOnDeviceChatBackend>>
createWindowsOnDeviceChatBackend(
    std::shared_ptr<
        WindowsOnDeviceChatDriver> driver);

} // namespace companion::detail
