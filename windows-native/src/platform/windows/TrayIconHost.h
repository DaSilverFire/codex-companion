#pragma once

#include "core/Result.h"

#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace companion {

struct TrayMonitorCoordinateSpace final {
    QRect nativeGeometry;
    QRect qtGeometry;
    qreal devicePixelRatio = 1.0;
};

struct TrayRouteState final {
    bool petRegistered = false;
    bool petVisible = false;
    bool companionMenuRegistered = false;
    bool companionMenuVisible = false;
    bool processesRegistered = false;
    bool chatRegistered = false;
    bool settingsVisible = false;

    friend bool operator==(const TrayRouteState&, const TrayRouteState&) = default;
};

class TrayActivationDeduplicator final {
public:
    explicit TrayActivationDeduplicator(int windowMilliseconds) noexcept;

    bool accept(qint64 messageTime) noexcept;

private:
    int windowMilliseconds_;
    qint64 lastAcceptedTime_ = -1;
};

class TrayIconHost final : public QObject {
    Q_OBJECT

public:
    using NotifyIconFunction = std::function<bool(DWORD, NOTIFYICONDATAW*)>;
    using RouteStateProvider = std::function<TrayRouteState()>;

    enum class Command {
        TogglePet,
        ToggleCompanionMenu,
        ShowProcesses,
        ShowChat,
        ShowSettings,
        Quit,
    };

    explicit TrayIconHost(QObject* parent = nullptr);
    explicit TrayIconHost(NotifyIconFunction notifyIcon, QObject* parent = nullptr);
    ~TrayIconHost() override;

    Result<void> show(HICON icon, QString tooltip);
    void hide();
    Result<void> restoreAfterTaskbarCreated();
    void setRouteStateProvider(RouteStateProvider provider);
    void setRouteState(TrayRouteState state) noexcept;
    Result<void> refreshRouteStateForMenu();
    void invokeCommand(Command command);
    std::optional<QPoint> lastActivationPoint() const noexcept;

    static QStringList commandOrder();
    static bool shouldUseNotifyVersion4(bool myDockFinderPresent) noexcept;
    static UINT callbackEventFromMessage(LPARAM lParam, bool notifyVersion4) noexcept;
    static QPoint activationPointFromNative(POINT point) noexcept;
    static QPoint activationPointFromNative(
        POINT point,
        const TrayMonitorCoordinateSpace& coordinateSpace) noexcept;
    static QStringList menuEntryKeysForState(const TrayRouteState& state);
    static QStringList menuEntryLabelsForState(const TrayRouteState& state);

signals:
    void showPetRequested();
    void showMenuRequested();
    void showProcessesRequested();
    void showChatRequested();
    void showSettingsRequested();
    void hideSettingsRequested();
    void quitRequested();
    void runtimeErrorOccurred(CompanionError error);

private:
    Result<void> ensureWindow();
    Result<void> addOrRestoreIcon();
    void deleteIcon();
    void destroyWindow() noexcept;
    void handleWindowMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void handleTrayCallback(WPARAM wParam, LPARAM lParam);
    void showContextMenu(POINT anchorPoint);
    HMENU createMenu() const;
    void dispatchCommand(UINT commandId);
    POINT keyboardActivationPoint() const noexcept;
    POINT cursorPointFromCallback(WPARAM wParam, UINT event) const noexcept;
    static LRESULT CALLBACK windowProcedure(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept;

    HWND hwnd_ = nullptr;
    UINT callbackMessage_ = WM_APP + 0x51;
    UINT taskbarCreatedMessage_ = 0;
    HICON icon_ = nullptr;
    QString tooltip_;
    bool visible_ = false;
    bool notifyVersion4_ = true;
    NotifyIconFunction notifyIcon_;
    RouteStateProvider routeStateProvider_;
    TrayRouteState routeState_;
    TrayActivationDeduplicator activationDeduplicator_{175};
    std::optional<QPoint> lastActivationPoint_;
};

} // namespace companion
