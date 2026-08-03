#pragma once

#include "codex/chat/ChatService.h"
#include "core/Result.h"

#include <QFuture>
#include <QtGlobal>

#include <functional>
#include <memory>

namespace companion {

enum class WindowsOnDeviceChatPhase {
    ConsentRequired,
    Idle,
    DiscoveringExecutionProviders,
    DownloadingExecutionProviders,
    ResolvingModel,
    DownloadingModel,
    LoadingModel,
    Ready,
    Failed,
    Stopping,
};

struct WindowsOnDeviceChatStatus final {
    WindowsOnDeviceChatPhase phase =
        WindowsOnDeviceChatPhase::
            ConsentRequired;
    bool downloadConsentGranted = false;
    bool available = false;
    bool supportsAttachments = false;
    double progressPercent = 0.0;
    quint64 revision = 0;

    friend bool operator==(
        const WindowsOnDeviceChatStatus&,
        const WindowsOnDeviceChatStatus&) = default;
};

class WindowsOnDeviceChatStatusSubscription {
public:
    virtual ~WindowsOnDeviceChatStatusSubscription() =
        default;
};

class WindowsOnDeviceChatBackend {
public:
    virtual ~WindowsOnDeviceChatBackend() = default;

    virtual WindowsOnDeviceChatStatus status()
        const = 0;
    virtual Result<void> setDownloadConsent(
        bool granted) = 0;
    virtual QFuture<Result<void>> prepare() = 0;
    virtual std::shared_ptr<
        WindowsOnDeviceChatStatusSubscription>
    subscribeStatus(
        std::function<void(
            WindowsOnDeviceChatStatus)>
            observer) = 0;
    virtual Result<ChatResult> send(
        const ChatRequest& request) = 0;
};

Result<std::shared_ptr<
    WindowsOnDeviceChatBackend>>
acquireWindowsOnDeviceChatBackend();
void shutdownWindowsOnDeviceChatBackendForProcessExit()
    noexcept;

} // namespace companion
