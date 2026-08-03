#pragma once

#include "codex/state/HistoryModels.h"
#include "core/CompanionCommandBus.h"

#include <QPointer>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace companion {

class CodexRuntime;

struct CodexRuntimeDeliveryState final {
    std::mutex mutex;
    QPointer<CodexRuntime> runtime;
    bool destroying = false;
};

struct CodexRuntimeCommandInvocationState final {
    CodexRuntimeCommandInvocationState(
        std::weak_ptr<CodexRuntimeDeliveryState>
            requestedDeliveryState,
        CompanionCommandBus::Completion
            requestedCompletion);
    ~CodexRuntimeCommandInvocationState();

    void finish(Result<void> result) noexcept;
    bool claimInvocation() noexcept;

    std::weak_ptr<CodexRuntimeDeliveryState>
        deliveryState;
    CompanionCommandBus::Completion completion;
    std::atomic_bool invoked = false;
    std::atomic_bool finished = false;
};

struct CodexHistoryPublication final {
    QString threadId;
    std::optional<QString> requestCursor;
    HistorySnapshot snapshot;

    friend bool operator==(
        const CodexHistoryPublication&,
        const CodexHistoryPublication&) = default;
};

} // namespace companion
