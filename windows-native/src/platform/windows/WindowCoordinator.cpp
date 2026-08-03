#include "platform/windows/WindowCoordinator.h"

#include "platform/windows/BackdropController.h"
#include "platform/windows/NativeWindowApi.h"
#include "platform/windows/UtilityWindowPolicy.h"
#include "platform/windows/WindowRegionPolicy.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QMetaType>
#include <QPlatformSurfaceEvent>
#include <QQuickWindow>
#include <QVariantList>
#include <array>
#include <utility>

namespace {

using companion::CompanionError;

CompanionError roleError(QString code, QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

QVariantMap errorDetails(const CompanionError& error)
{
    return {
        {QStringLiteral("code"), error.code},
        {QStringLiteral("message"), error.message},
        {QStringLiteral("retryable"), error.retryable},
        {QStringLiteral("context"), error.context},
    };
}

CompanionError backdropRollbackError(
    const CompanionError& reapplyFailure,
    const CompanionError& rollbackFailure)
{
    return {
        QStringLiteral("window.backdrop-rollback-failed"),
        QStringLiteral(
            "Could not apply the requested Companion material or restore every previous material."),
        false,
        {
            {QStringLiteral("reapplyFailure"), errorDetails(reapplyFailure)},
            {QStringLiteral("rollbackFailure"), errorDetails(rollbackFailure)},
        },
    };
}

QString roleName(companion::WindowRole role)
{
    switch (role) {
    case companion::WindowRole::Settings:
        return QStringLiteral("Settings");
    case companion::WindowRole::Pet:
        return QStringLiteral("Pet");
    case companion::WindowRole::CompanionMenu:
        return QStringLiteral("CompanionMenu");
    case companion::WindowRole::ModelPicker:
        return QStringLiteral("ModelPicker");
    case companion::WindowRole::Goal:
        return QStringLiteral("Goal");
    case companion::WindowRole::Usage:
        return QStringLiteral("Usage");
    case companion::WindowRole::Attention:
        return QStringLiteral("Attention");
    }
    return QStringLiteral("Unknown");
}

} // namespace

namespace companion {

WindowCoordinator::WindowCoordinator(QObject* parent)
    : QObject(parent),
      nativeWindowApi_(std::make_unique<NativeWindowApi>()),
      ownedBackdropController_(std::make_unique<BackdropController>(*nativeWindowApi_)),
      backdropController_(ownedBackdropController_.get())
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    qRegisterMetaType<BackdropMode>("companion::BackdropMode");
    qRegisterMetaType<WindowRole>("companion::WindowRole");
    installNativeEventFilter();
}

WindowCoordinator::WindowCoordinator(
    BackdropController& backdropController,
    QObject* parent)
    : QObject(parent),
      backdropController_(&backdropController)
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    qRegisterMetaType<BackdropMode>("companion::BackdropMode");
    qRegisterMetaType<WindowRole>("companion::WindowRole");
    installNativeEventFilter();
}

WindowCoordinator::~WindowCoordinator()
{
    if (nativeEventFilterInstalled_) {
        if (auto* application = QCoreApplication::instance()) {
            application->removeNativeEventFilter(this);
        }
    }
    for (const auto& registered : std::as_const(windows_)) {
        if (registered.window) {
            registered.window->removeEventFilter(this);
        }
    }
}

bool WindowCoordinator::nativeEventFilter(
    const QByteArray& eventType,
    void* message,
    qintptr* result)
{
    if (message == nullptr
        || result == nullptr
        || (eventType != QByteArrayLiteral("windows_generic_MSG")
            && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))) {
        return false;
    }

    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage->message != WM_MOUSEACTIVATE) {
        return false;
    }

    for (auto iterator = windows_.constBegin();
         iterator != windows_.constEnd();
         ++iterator) {
        const auto& registered = iterator.value();
        if (!registered.registered
            || registered.window.isNull()
            || registered.window->handle() == nullptr
            || reinterpret_cast<HWND>(
                   registered.window->winId())
                != nativeMessage->hwnd) {
            continue;
        }

        *result = iterator.key() == WindowRole::Attention
            ? MA_NOACTIVATE
            : MA_ACTIVATE;
        return true;
    }

    return false;
}

Result<void> WindowCoordinator::registerWindow(WindowRole role, QQuickWindow& window)
{
    if (!isKnownRole(role)) {
        const auto error = roleError(
            QStringLiteral("window.role-unknown"),
            QStringLiteral("Unknown Companion top-level window role."));
        reportRuntimeError(error);
        return Result<void>::failure(error);
    }

    auto& registered = windows_[role];
    if (registered.window && registered.window != &window) {
        registered.window->removeEventFilter(this);
    }

    registered.window = &window;
    registered.registered = true;
    window.installEventFilter(this);

    if (window.handle() != nullptr) {
        const auto applied =
            applyRegisteredPolicy(
                role,
                window);
        if (!applied.hasValue()) {
            reportRuntimeError(applied.error());
            return applied;
        }
    }

    return Result<void>::success();
}

Result<void> WindowCoordinator::show(WindowRole role)
{
    const auto window = windowFor(role, role == WindowRole::Settings);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    if (window.value() == nullptr) {
        return Result<void>::success();
    }

    if (window.value()->isVisible() && window.value()->handle() != nullptr) {
        const auto hwnd =
            reinterpret_cast<HWND>(window.value()->winId());
        if (!nativeWindowInfo(hwnd).visible) {
            // Shell integrations can hide an HWND without updating QWindow.
            window.value()->hide();
        }
    }

    const auto applied =
        applyRegisteredPolicy(
            role,
            *window.value());
    if (!applied.hasValue()) {
        reportRuntimeError(applied.error());
        return applied;
    }
    window.value()->show();
    const auto reapplied =
        applyRegisteredPolicy(
            role,
            *window.value());
    if (!reapplied.hasValue()) {
        reportRuntimeError(reapplied.error());
    }
    return reapplied;
}

Result<void> WindowCoordinator::hide(WindowRole role)
{
    const auto window = windowFor(role, role == WindowRole::Settings);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    if (window.value() == nullptr) {
        return Result<void>::success();
    }

    window.value()->hide();
    return Result<void>::success();
}

Result<void> WindowCoordinator::activate(WindowRole role)
{
    const auto window = windowFor(role, role == WindowRole::Settings);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    if (window.value() == nullptr) {
        return Result<void>::success();
    }

    const auto applied =
        applyRegisteredPolicy(
            role,
            *window.value());
    if (!applied.hasValue()) {
        reportRuntimeError(applied.error());
        return applied;
    }
    window.value()->requestActivate();
    return Result<void>::success();
}

Result<void> WindowCoordinator::move(WindowRole role, QPoint position)
{
    const auto window = windowFor(role, role == WindowRole::Settings);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    if (window.value() == nullptr) {
        return Result<void>::success();
    }

    window.value()->setPosition(position);
    return Result<void>::success();
}

Result<void> WindowCoordinator::setOwner(WindowRole role, WindowRole ownerRole)
{
    const auto window = windowFor(role, false);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    const auto owner = windowFor(ownerRole, false);
    if (!owner.hasValue()) {
        reportRuntimeError(owner.error());
        return Result<void>::failure(owner.error());
    }

    const auto assigned = assignOwner(*window.value(), *owner.value());
    if (!assigned.hasValue()) {
        reportRuntimeError(assigned.error());
        return assigned;
    }
    windows_[role].ownerRole = ownerRole;
    return Result<void>::success();
}

Result<void> WindowCoordinator::destroy(WindowRole role)
{
    const auto window = windowFor(role, role == WindowRole::Settings);
    if (!window.hasValue()) {
        reportRuntimeError(window.error());
        return Result<void>::failure(window.error());
    }
    if (window.value() == nullptr) {
        return Result<void>::success();
    }

    window.value()->close();
    window.value()->destroy();
    return Result<void>::success();
}

Result<void> WindowCoordinator::toggle(WindowRole role)
{
    if (roleIsVisible(role)) {
        return hide(role);
    }
    return show(role);
}

Result<void> WindowCoordinator::setBackdropMode(BackdropMode mode)
{
    return setBackdropMode(mode, ErrorReportMode::EmitRuntimeError);
}

Result<void> WindowCoordinator::setBackdropMode(
    BackdropMode mode,
    ErrorReportMode errorReportMode)
{
    const BackdropMode previousRequestedMode = requestedBackdropMode_;

    requestedBackdropMode_ = mode;
    const auto reapplied = reapplyBackdropMode();
    if (!reapplied.hasValue()) {
        requestedBackdropMode_ = previousRequestedMode;
        const auto rolledBack = reapplyBackdropMode();
        CompanionError resultError = reapplied.error();
        if (!rolledBack.hasValue()) {
            resultError =
                backdropRollbackError(reapplied.error(), rolledBack.error());
        }
        if (errorReportMode == ErrorReportMode::EmitRuntimeError) {
            reportRuntimeError(resultError);
        }
        return Result<void>::failure(std::move(resultError));
    }
    return reapplied;
}

Result<BackdropMode> WindowCoordinator::effectiveBackdropMode(WindowRole role) const
{
    if (!isKnownRole(role)) {
        return Result<BackdropMode>::failure(roleError(
            QStringLiteral("window.role-unknown"),
            QStringLiteral("Unknown Companion top-level window role.")));
    }

    const auto iterator = effectiveBackdropModes_.constFind(role);
    if (iterator == effectiveBackdropModes_.constEnd()) {
        return Result<BackdropMode>::success(requestedBackdropMode_);
    }

    return Result<BackdropMode>::success(iterator.value());
}

TrayRouteState WindowCoordinator::trayRouteState() const
{
    const bool companionMenuRegistered =
        roleIsRegistered(WindowRole::CompanionMenu);
    return {
        roleIsRegistered(WindowRole::Pet),
        roleIsVisible(WindowRole::Pet),
        companionMenuRegistered,
        roleIsVisible(WindowRole::CompanionMenu),
        companionMenuRegistered,
        companionMenuRegistered,
        roleIsVisible(WindowRole::Settings),
    };
}

bool WindowCoordinator::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::PlatformSurface) {
        auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
        if (surfaceEvent->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceCreated) {
            if (auto* window = qobject_cast<QQuickWindow*>(watched)) {
                const auto applied =
                    applyRegisteredPolicy(
                        roleForWindow(*window),
                        *window);
                if (!applied.hasValue()) {
                    reportRuntimeError(applied.error());
                }
            }
        }
    } else if (event->type() == QEvent::Resize
               || event->type() == QEvent::ScreenChangeInternal
               || event->type() == QEvent::DevicePixelRatioChange) {
        if (auto* window =
                qobject_cast<QQuickWindow*>(
                    watched)) {
            const WindowRole role =
                roleForWindow(*window);
            if (role != WindowRole::Pet
                && window->handle() != nullptr) {
                const auto applied =
                    WindowRegionPolicy::apply(
                        *window);
                if (!applied.hasValue()) {
                    reportRuntimeError(
                        applied.error());
                }
            }
        }
    }

    return QObject::eventFilter(watched, event);
}

Result<QQuickWindow*> WindowCoordinator::windowFor(
    WindowRole role,
    bool allowMissingSettings)
{
    if (!isKnownRole(role)) {
        return Result<QQuickWindow*>::failure(roleError(
            QStringLiteral("window.role-unknown"),
            QStringLiteral("Unknown Companion top-level window role.")));
    }

    const auto iterator = windows_.constFind(role);
    if (iterator == windows_.constEnd()) {
        if (allowMissingSettings) {
            return Result<QQuickWindow*>::success(nullptr);
        }
        return Result<QQuickWindow*>::failure(roleError(
            QStringLiteral("window.not-registered"),
            QStringLiteral("Companion top-level window is not registered.")));
    }
    if (iterator.value().registered && iterator.value().window.isNull()) {
        return Result<QQuickWindow*>::failure(roleError(
            QStringLiteral("window.destroyed"),
            QStringLiteral("Companion top-level window was destroyed.")));
    }

    return Result<QQuickWindow*>::success(iterator.value().window.data());
}

Result<void> WindowCoordinator::applyRegisteredPolicy(
    WindowRole role,
    QQuickWindow& window)
{
    const auto iterator =
        windows_.constFind(role);
    if (iterator != windows_.constEnd()
        && iterator.value().ownerRole.has_value()) {
        const auto owner =
            windowFor(
                *iterator.value().ownerRole,
                false);
        if (!owner.hasValue()) {
            return Result<void>::failure(
                owner.error());
        }
        return assignOwner(
            window,
            *owner.value());
    }

    return applyPolicy(role, window);
}

Result<void> WindowCoordinator::applyPolicy(WindowRole role, QQuickWindow& window)
{
    const auto utilityApplied = UtilityWindowPolicy::apply(window);
    if (!utilityApplied.hasValue()) {
        return utilityApplied;
    }

    if (role == WindowRole::Pet) {
        setEffectiveBackdropMode(role, requestedBackdropMode_);
        return Result<void>::success();
    }

    const auto regionApplied =
        WindowRegionPolicy::apply(
            window);
    if (!regionApplied.hasValue()) {
        return regionApplied;
    }

    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const auto applied =
        backdropController_->apply(hwnd, requestedBackdropMode_, role);
    if (!applied.hasValue()) {
        return Result<void>::failure(applied.error());
    }
    setEffectiveBackdropMode(role, applied.value().effective);
    return Result<void>::success();
}

Result<void> WindowCoordinator::reapplyBackdropMode()
{
    constexpr std::array roles = {
        WindowRole::Settings,
        WindowRole::CompanionMenu,
        WindowRole::ModelPicker,
        WindowRole::Goal,
        WindowRole::Usage,
        WindowRole::Attention,
    };
    QVariantList failures;

    for (const auto role : roles) {
        const auto iterator = windows_.constFind(role);
        if (iterator == windows_.constEnd() ||
            !iterator.value().registered ||
            iterator.value().window.isNull() ||
            iterator.value().window->handle() == nullptr) {
            continue;
        }

        const auto applied = backdropController_->apply(
            reinterpret_cast<HWND>(iterator.value().window->winId()),
            requestedBackdropMode_,
            role);
        if (!applied.hasValue()) {
            failures.append(QVariantMap {
                {QStringLiteral("role"), roleName(role)},
                {QStringLiteral("code"), applied.error().code},
                {QStringLiteral("context"), applied.error().context},
            });
            continue;
        }

        setEffectiveBackdropMode(role, applied.value().effective);
    }

    if (!failures.isEmpty()) {
        return Result<void>::failure({
            QStringLiteral("window.backdrop-reapply-failed"),
            QStringLiteral("Could not reapply Companion materials to every window."),
            false,
            {{QStringLiteral("failures"), failures}},
        });
    }

    return Result<void>::success();
}

Result<void> WindowCoordinator::assignOwner(QQuickWindow& window, QQuickWindow& owner)
{
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const auto ownerHwnd = reinterpret_cast<HWND>(owner.winId());
    const auto assigned = nativeSetOwner(
        hwnd,
        ownerHwnd,
        QStringLiteral("window.owner-failed"),
        QStringLiteral("Could not assign the Companion window owner."));
    if (!assigned.hasValue()) {
        return assigned;
    }
    return applyPolicy(roleForWindow(window), window);
}

WindowRole WindowCoordinator::roleForWindow(const QQuickWindow& window) const noexcept
{
    for (auto iterator = windows_.constBegin(); iterator != windows_.constEnd();
         ++iterator) {
        if (iterator.value().window == &window) {
            return iterator.key();
        }
    }
    return WindowRole::Pet;
}

void WindowCoordinator::installNativeEventFilter()
{
    if (nativeEventFilterInstalled_) {
        return;
    }
    if (auto* application = QCoreApplication::instance()) {
        application->installNativeEventFilter(this);
        nativeEventFilterInstalled_ = true;
    }
}

void WindowCoordinator::reportRuntimeError(const CompanionError& error)
{
    emit runtimeErrorOccurred(error);
}

void WindowCoordinator::setEffectiveBackdropMode(WindowRole role, BackdropMode mode)
{
    const auto iterator = effectiveBackdropModes_.constFind(role);
    if (iterator != effectiveBackdropModes_.constEnd() && iterator.value() == mode) {
        return;
    }

    effectiveBackdropModes_[role] = mode;
    emit effectiveBackdropModeChanged(role, mode);
}

bool WindowCoordinator::isKnownRole(WindowRole role) noexcept
{
    switch (role) {
    case WindowRole::Settings:
    case WindowRole::Pet:
    case WindowRole::CompanionMenu:
    case WindowRole::ModelPicker:
    case WindowRole::Goal:
    case WindowRole::Usage:
    case WindowRole::Attention:
        return true;
    }
    return false;
}

bool WindowCoordinator::roleIsRegistered(WindowRole role) const noexcept
{
    const auto iterator = windows_.constFind(role);
    return iterator != windows_.constEnd() &&
        iterator.value().registered &&
        !iterator.value().window.isNull();
}

bool WindowCoordinator::roleIsVisible(WindowRole role) const noexcept
{
    const auto iterator = windows_.constFind(role);
    if (iterator == windows_.constEnd() ||
        !iterator.value().registered ||
        !iterator.value().window ||
        !iterator.value().window->isVisible()) {
        return false;
    }

    if (iterator.value().window->handle() == nullptr) {
        return true;
    }

    return nativeWindowInfo(
               reinterpret_cast<HWND>(iterator.value().window->winId()))
        .visible;
}

} // namespace companion
