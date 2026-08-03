#include "codex/chat/WindowsOnDeviceChatBackendRegistryInternal.h"

#include <utility>

namespace companion::detail {

namespace {

CompanionError registryUnavailableError()
{
    return {
        QStringLiteral(
            "chat.on_device_unavailable"),
        QStringLiteral(
            "The Windows on-device chat provider is unavailable."),
        false,
        {},
    };
}

} // namespace

WindowsOnDeviceChatBackendRegistry::
WindowsOnDeviceChatBackendRegistry(
    WindowsOnDeviceChatDriverFactory
        driverFactory)
    : driverFactory_(
          std::move(driverFactory))
{
}

WindowsOnDeviceChatBackendRegistry::
~WindowsOnDeviceChatBackendRegistry()
{
    shutdownForProcessExit();
}

Result<std::shared_ptr<
    WindowsOnDeviceChatBackend>>
WindowsOnDeviceChatBackendRegistry::acquire()
{
    const std::scoped_lock lock(mutex_);
    if (phase_ != Phase::Running
        || !driverFactory_) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(
                registryUnavailableError());
    }
    if (owner_ != nullptr) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            success(owner_);
    }

    Result<std::shared_ptr<
        WindowsOnDeviceChatDriver>>
        driver =
            Result<std::shared_ptr<
                WindowsOnDeviceChatDriver>>::
                failure(
                    registryUnavailableError());
    try {
        driver = driverFactory_();
    } catch (...) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(
                registryUnavailableError());
    }
    if (!driver.hasValue()
        || driver.value() == nullptr) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(
                registryUnavailableError());
    }

    auto backend =
        createWindowsOnDeviceChatBackend(
            std::move(driver.value()));
    if (!backend.hasValue()
        || backend.value() == nullptr) {
        return Result<std::shared_ptr<
            WindowsOnDeviceChatBackend>>::
            failure(
                registryUnavailableError());
    }
    owner_ =
        std::move(backend.value());
    return Result<std::shared_ptr<
        WindowsOnDeviceChatBackend>>::
        success(owner_);
}

void WindowsOnDeviceChatBackendRegistry::
shutdownForProcessExit() noexcept
{
    std::shared_ptr<
        WindowsOnDeviceChatBackend> owner;
    try {
        {
            const std::scoped_lock lock(
                mutex_);
            if (phase_ != Phase::Running) {
                return;
            }
            phase_ = Phase::Stopping;
            owner = std::move(owner_);
            driverFactory_ = {};
        }
        owner.reset();
        {
            const std::scoped_lock lock(
                mutex_);
            phase_ = Phase::Stopped;
        }
    } catch (...) {
        try {
            const std::scoped_lock lock(
                mutex_);
            owner_.reset();
            driverFactory_ = {};
            phase_ = Phase::Stopped;
        } catch (...) {
        }
    }
}

} // namespace companion::detail
