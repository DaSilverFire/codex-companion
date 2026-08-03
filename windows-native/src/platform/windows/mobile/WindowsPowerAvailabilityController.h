#pragma once

#include "core/Result.h"

#include <QString>

#include <memory>

namespace companion {

using WindowsPowerRequestHandle = void*;

class IWindowsPowerRequestApi {
public:
    virtual ~IWindowsPowerRequestApi() = default;

    virtual Result<WindowsPowerRequestHandle>
    create(const QString& reason) = 0;
    virtual Result<void> setSystemRequired(
        WindowsPowerRequestHandle handle) = 0;
    virtual Result<void> clearSystemRequired(
        WindowsPowerRequestHandle handle) = 0;
    virtual void close(
        WindowsPowerRequestHandle handle)
        noexcept = 0;
};

class WindowsPowerAvailabilityController final {
public:
    explicit WindowsPowerAvailabilityController(
        IWindowsPowerRequestApi* api =
            nullptr);
    ~WindowsPowerAvailabilityController();

    WindowsPowerAvailabilityController(
        const WindowsPowerAvailabilityController&) =
        delete;
    WindowsPowerAvailabilityController& operator=(
        const WindowsPowerAvailabilityController&) =
        delete;

    Result<void> setAvailable(bool value);
    bool isAvailable() const noexcept;

    static QString requestReason();

private:
    void closeRequest() noexcept;

    std::unique_ptr<IWindowsPowerRequestApi>
        ownedApi_;
    IWindowsPowerRequestApi* api_ = nullptr;
    WindowsPowerRequestHandle request_ =
        nullptr;
    bool available_ = false;
};

} // namespace companion
