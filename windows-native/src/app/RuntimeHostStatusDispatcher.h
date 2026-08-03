#pragma once

#include "codex/chat/WindowsOnDeviceChatBackend.h"

#include <functional>
#include <memory>

namespace companion::detail {

class RuntimeHostStatusDispatcher final {
public:
    using Delivery =
        std::function<void(WindowsOnDeviceChatStatus)>;
    using Queue =
        std::function<bool(std::function<void()>)>;

    explicit RuntimeHostStatusDispatcher(
        Delivery delivery,
        Queue queue = {});

    void publish(
        WindowsOnDeviceChatStatus status) const;
    void invalidate() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace companion::detail
