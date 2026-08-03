#pragma once

#include "core/AppSettings.h"
#include "core/Result.h"
#include "platform/windows/TrayIconHost.h"

#include <QAbstractNativeEventFilter>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QPoint>
#include <QHash>
#include <memory>
#include <optional>

class QEvent;
class QQuickWindow;

namespace companion {

class BackdropController;
class NativeWindowApi;

enum class WindowRole {
    Settings,
    Pet,
    CompanionMenu,
    ModelPicker,
    Goal,
    Usage,
    Attention,
};

class WindowCoordinator final
    : public QObject,
      public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    enum class ErrorReportMode {
        EmitRuntimeError,
        ReturnOnly,
    };

    explicit WindowCoordinator(QObject* parent = nullptr);
    explicit WindowCoordinator(
        BackdropController& backdropController,
        QObject* parent = nullptr);
    ~WindowCoordinator() override;

    bool nativeEventFilter(
        const QByteArray& eventType,
        void* message,
        qintptr* result) override;

    Result<void> registerWindow(WindowRole role, QQuickWindow& window);
    Result<void> show(WindowRole role);
    Result<void> hide(WindowRole role);
    Result<void> activate(WindowRole role);
    Result<void> move(WindowRole role, QPoint position);
    Result<void> setOwner(WindowRole role, WindowRole ownerRole);
    Result<void> destroy(WindowRole role);
    Result<void> toggle(WindowRole role);
    Result<void> setBackdropMode(BackdropMode mode);
    Result<void> setBackdropMode(BackdropMode mode, ErrorReportMode errorReportMode);
    Result<BackdropMode> effectiveBackdropMode(WindowRole role) const;
    TrayRouteState trayRouteState() const;

signals:
    void runtimeErrorOccurred(CompanionError error);
    void effectiveBackdropModeChanged(WindowRole role, BackdropMode mode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct RegisteredWindow final {
        QPointer<QQuickWindow> window;
        std::optional<WindowRole> ownerRole;
        bool registered = false;
    };

    Result<QQuickWindow*> windowFor(WindowRole role, bool allowMissingSettings);
    Result<void> applyRegisteredPolicy(WindowRole role, QQuickWindow& window);
    Result<void> applyPolicy(WindowRole role, QQuickWindow& window);
    Result<void> reapplyBackdropMode();
    Result<void> assignOwner(QQuickWindow& window, QQuickWindow& owner);
    WindowRole roleForWindow(const QQuickWindow& window) const noexcept;
    void installNativeEventFilter();
    void reportRuntimeError(const CompanionError& error);
    static bool isKnownRole(WindowRole role) noexcept;
    bool roleIsRegistered(WindowRole role) const noexcept;
    bool roleIsVisible(WindowRole role) const noexcept;
    void setEffectiveBackdropMode(WindowRole role, BackdropMode mode);

    QHash<WindowRole, RegisteredWindow> windows_;
    QHash<WindowRole, BackdropMode> effectiveBackdropModes_;
    std::unique_ptr<NativeWindowApi> nativeWindowApi_;
    std::unique_ptr<BackdropController> ownedBackdropController_;
    BackdropController* backdropController_ = nullptr;
    BackdropMode requestedBackdropMode_ = BackdropMode::Mica;
    bool nativeEventFilterInstalled_ = false;
};

} // namespace companion
