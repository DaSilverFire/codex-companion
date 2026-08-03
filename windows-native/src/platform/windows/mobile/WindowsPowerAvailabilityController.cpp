#include "platform/windows/mobile/WindowsPowerAvailabilityController.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <utility>

namespace companion {
namespace {

CompanionError powerRequestError(
    QString code,
    QString message,
    QString operation,
    DWORD nativeError)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {QStringLiteral("operation"),
             std::move(operation)},
            {QStringLiteral("nativeError"),
             static_cast<qulonglong>(
                 nativeError)},
        },
    };
}

class WindowsNativePowerRequestApi final
    : public IWindowsPowerRequestApi {
public:
    Result<WindowsPowerRequestHandle>
    create(const QString& reason) override
    {
        REASON_CONTEXT context{};
        context.Version =
            POWER_REQUEST_CONTEXT_VERSION;
        context.Flags =
            POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString =
            const_cast<PWSTR>(
                reinterpret_cast<PCWSTR>(
                    reason.utf16()));
        const HANDLE request =
            PowerCreateRequest(&context);
        if (request
            == INVALID_HANDLE_VALUE) {
            return Result<
                WindowsPowerRequestHandle>::
                failure(
                    powerRequestError(
                        QStringLiteral(
                            "power.request-create-failed"),
                        QStringLiteral(
                            "Windows could not create the Companion availability request."),
                        QStringLiteral(
                            "PowerCreateRequest"),
                        GetLastError()));
        }
        return Result<
            WindowsPowerRequestHandle>::
            success(request);
    }

    Result<void> setSystemRequired(
        WindowsPowerRequestHandle handle)
        override
    {
        if (!PowerSetRequest(
                static_cast<HANDLE>(handle),
                PowerRequestSystemRequired)) {
            return Result<void>::failure(
                powerRequestError(
                    QStringLiteral(
                        "power.request-set-failed"),
                    QStringLiteral(
                        "Windows could not keep Companion available while the display is off."),
                    QStringLiteral(
                        "PowerSetRequest"),
                    GetLastError()));
        }
        return Result<void>::success();
    }

    Result<void> clearSystemRequired(
        WindowsPowerRequestHandle handle)
        override
    {
        if (!PowerClearRequest(
                static_cast<HANDLE>(handle),
                PowerRequestSystemRequired)) {
            return Result<void>::failure(
                powerRequestError(
                    QStringLiteral(
                        "power.request-clear-failed"),
                    QStringLiteral(
                        "Windows could not release the Companion availability request."),
                    QStringLiteral(
                        "PowerClearRequest"),
                    GetLastError()));
        }
        return Result<void>::success();
    }

    void close(
        WindowsPowerRequestHandle handle)
        noexcept override
    {
        if (handle != nullptr
            && handle
                != INVALID_HANDLE_VALUE) {
            CloseHandle(
                static_cast<HANDLE>(
                    handle));
        }
    }
};

} // namespace

WindowsPowerAvailabilityController::
WindowsPowerAvailabilityController(
    IWindowsPowerRequestApi* api)
{
    if (api == nullptr) {
        ownedApi_ =
            std::make_unique<
                WindowsNativePowerRequestApi>();
        api_ = ownedApi_.get();
    } else {
        api_ = api;
    }
}

WindowsPowerAvailabilityController::
~WindowsPowerAvailabilityController()
{
    if (available_
        && request_ != nullptr) {
        (void)api_->clearSystemRequired(
            request_);
    }
    closeRequest();
}

Result<void>
WindowsPowerAvailabilityController::
setAvailable(bool value)
{
    if (available_ == value) {
        return Result<void>::success();
    }
    if (value) {
        const auto created =
            api_->create(requestReason());
        if (!created.hasValue()) {
            return Result<void>::failure(
                created.error());
        }
        request_ = created.value();
        if (request_ == nullptr) {
            closeRequest();
            return Result<void>::failure(
                {
                    QStringLiteral(
                        "power.request-create-failed"),
                    QStringLiteral(
                        "Windows returned an invalid Companion availability request."),
                    false,
                    {},
                });
        }
        const auto activated =
            api_->setSystemRequired(
                request_);
        if (!activated.hasValue()) {
            closeRequest();
            return activated;
        }
        available_ = true;
        return Result<void>::success();
    }

    const auto released =
        api_->clearSystemRequired(
            request_);
    if (!released.hasValue()) {
        return released;
    }
    available_ = false;
    closeRequest();
    return Result<void>::success();
}

bool WindowsPowerAvailabilityController::
isAvailable() const noexcept
{
    return available_;
}

QString WindowsPowerAvailabilityController::
requestReason()
{
    return QStringLiteral(
        "Keep Codex Companion mobile access available while the display is off.");
}

void WindowsPowerAvailabilityController::
closeRequest() noexcept
{
    if (request_ == nullptr) {
        return;
    }
    api_->close(request_);
    request_ = nullptr;
}

} // namespace companion
