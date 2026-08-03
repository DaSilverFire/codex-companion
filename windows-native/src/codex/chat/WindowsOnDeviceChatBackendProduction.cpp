#include "codex/chat/FoundryLocalDriverInternal.h"
#include "codex/chat/WindowsOnDeviceChatBackend.h"
#include "codex/chat/WindowsOnDeviceChatBackendRegistryInternal.h"

namespace companion {

namespace {

detail::WindowsOnDeviceChatBackendRegistry&
productionRegistry()
{
    static detail::
        WindowsOnDeviceChatBackendRegistry
            registry([] {
                auto api =
                    detail::
                        createFoundryLocalApi();
                if (!api.hasValue()) {
                    return Result<
                        std::shared_ptr<
                            detail::
                                WindowsOnDeviceChatDriver>>::
                        failure(
                            api.error());
                }
                return detail::
                    createFoundryLocalChatDriver(
                        std::move(
                            api.value()));
            });
    return registry;
}

} // namespace

Result<std::shared_ptr<
    WindowsOnDeviceChatBackend>>
acquireWindowsOnDeviceChatBackend()
{
    return productionRegistry().acquire();
}

void shutdownWindowsOnDeviceChatBackendForProcessExit()
    noexcept
{
    productionRegistry()
        .shutdownForProcessExit();
}

} // namespace companion
