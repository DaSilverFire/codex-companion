#include "platform/windows/TrayIconHost.h"

#include <QGuiApplication>
#include <QMetaType>
#include <QScreen>
#include <QtGui/qscreen_platform.h>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <optional>
#include <shellapi.h>
#include <utility>
#include <windowsx.h>

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kCommandBase = 40000;
constexpr GUID kCompanionTrayGuid = {
    0x9B3C42CB,
    0x4B7F,
    0x4A08,
    {0xB6, 0x75, 0x07, 0x17, 0x08, 0x94, 0x8C, 0x88},
};
constexpr wchar_t kWindowClassName[] = L"CodexCompanion.NotificationAreaHost";

using companion::CompanionError;
using companion::TrayIconHost;

struct CommandDescriptor final {
    const char* key;
    TrayIconHost::Command command;
    UINT id;
};

constexpr std::array<CommandDescriptor, 6> kCommandDescriptors{{
    {"toggle-pet", TrayIconHost::Command::TogglePet, kCommandBase + 1},
    {"toggle-companion-menu", TrayIconHost::Command::ToggleCompanionMenu, kCommandBase + 2},
    {"show-processes", TrayIconHost::Command::ShowProcesses, kCommandBase + 3},
    {"show-chat", TrayIconHost::Command::ShowChat, kCommandBase + 4},
    {"show-settings", TrayIconHost::Command::ShowSettings, kCommandBase + 5},
    {"quit", TrayIconHost::Command::Quit, kCommandBase + 6},
}};

CompanionError trayError(QString code, QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

bool myDockFinderPresent() noexcept
{
    return FindWindowW(L"MyFinderAppDesktopBg", nullptr) != nullptr;
}

void populateNotifyIconData(
    NOTIFYICONDATAW& data,
    HWND hwnd,
    UINT callbackMessage,
    HICON icon,
    const QString& tooltip,
    bool notifyVersion4)
{
    data = {};
    data.cbSize = sizeof(NOTIFYICONDATAW);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    if (notifyVersion4) {
        data.uFlags |= NIF_SHOWTIP;
    }
    data.uCallbackMessage = callbackMessage;
    data.hIcon = icon;
    data.guidItem = kCompanionTrayGuid;

    const std::wstring nativeTip = tooltip.toStdWString();
    if (!nativeTip.empty()) {
        wcsncpy_s(data.szTip, nativeTip.c_str(), _TRUNCATE);
    }
}

UINT eventFromCallback(LPARAM lParam, bool notifyVersion4) noexcept
{
    return TrayIconHost::callbackEventFromMessage(lParam, notifyVersion4);
}

bool isKeyboardEvent(UINT event) noexcept
{
    return event == NIN_KEYSELECT;
}

bool isActivationEvent(UINT event) noexcept
{
    switch (event) {
    case WM_CONTEXTMENU:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        return true;
    default:
        return false;
    }
}

const CommandDescriptor* descriptorForId(UINT id) noexcept
{
    for (const auto& descriptor : kCommandDescriptors) {
        if (descriptor.id == id) {
            return &descriptor;
        }
    }
    return nullptr;
}

bool appendMenuText(HMENU menu, UINT id, const QString& label)
{
    const std::wstring nativeLabel = label.toStdWString();
    return AppendMenuW(menu, MF_STRING, id, nativeLabel.c_str()) != 0;
}

bool shellNotifyIcon(DWORD message, NOTIFYICONDATAW* data) noexcept
{
    return Shell_NotifyIconW(message, data) != 0;
}

bool commandIsAvailable(TrayIconHost::Command command, const companion::TrayRouteState& state)
{
    switch (command) {
    case TrayIconHost::Command::TogglePet:
        return state.petRegistered;
    case TrayIconHost::Command::ToggleCompanionMenu:
        return state.companionMenuRegistered;
    case TrayIconHost::Command::ShowProcesses:
        return state.processesRegistered;
    case TrayIconHost::Command::ShowChat:
        return state.chatRegistered;
    case TrayIconHost::Command::ShowSettings:
    case TrayIconHost::Command::Quit:
        return true;
    }
    return false;
}

std::optional<companion::TrayMonitorCoordinateSpace>
coordinateSpaceForNativePoint(POINT point) noexcept
{
    const HMONITOR monitor =
        MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return std::nullopt;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(monitor, &monitorInfo) == FALSE) {
        return std::nullopt;
    }

    for (QScreen* const screen : QGuiApplication::screens()) {
        if (screen == nullptr) {
            continue;
        }

        const auto* const nativeScreen =
            screen->nativeInterface<
                QNativeInterface::QWindowsScreen>();
        if (nativeScreen == nullptr
            || nativeScreen->handle() != monitor) {
            continue;
        }

        const RECT nativeRect = monitorInfo.rcMonitor;
        return companion::TrayMonitorCoordinateSpace{
            QRect(
                nativeRect.left,
                nativeRect.top,
                nativeRect.right - nativeRect.left,
                nativeRect.bottom - nativeRect.top),
            screen->geometry(),
            screen->devicePixelRatio(),
        };
    }

    return std::nullopt;
}

QString labelForCommand(TrayIconHost::Command command, const companion::TrayRouteState& state)
{
    switch (command) {
    case TrayIconHost::Command::TogglePet:
        return state.petVisible
            ? QStringLiteral("Hide Pet")
            : QStringLiteral("Show Pet");
    case TrayIconHost::Command::ToggleCompanionMenu:
        return state.companionMenuVisible
            ? QStringLiteral("Hide Companion Menu")
            : QStringLiteral("Show Companion Menu");
    case TrayIconHost::Command::ShowProcesses:
        return QStringLiteral("Codex Processes");
    case TrayIconHost::Command::ShowChat:
        return QStringLiteral("Local Chat");
    case TrayIconHost::Command::ShowSettings:
        return QStringLiteral("Settings");
    case TrayIconHost::Command::Quit:
        return QStringLiteral("Quit Codex Companion");
    }
    return {};
}

const CommandDescriptor* descriptorForKey(const QString& key) noexcept
{
    const auto descriptor = std::find_if(
        kCommandDescriptors.cbegin(),
        kCommandDescriptors.cend(),
        [&key](const CommandDescriptor& candidate) {
            return key == QLatin1String(candidate.key);
        });
    return descriptor == kCommandDescriptors.cend() ? nullptr : &(*descriptor);
}

} // namespace

namespace companion {

TrayActivationDeduplicator::TrayActivationDeduplicator(
    int windowMilliseconds) noexcept
    : windowMilliseconds_(windowMilliseconds)
{
}

bool TrayActivationDeduplicator::accept(qint64 messageTime) noexcept
{
    if (lastAcceptedTime_ >= 0 &&
        messageTime - lastAcceptedTime_ < windowMilliseconds_) {
        return false;
    }

    lastAcceptedTime_ = messageTime;
    return true;
}

TrayIconHost::TrayIconHost(QObject* parent)
    : TrayIconHost(NotifyIconFunction(shellNotifyIcon), parent)
{
}

TrayIconHost::TrayIconHost(NotifyIconFunction notifyIcon, QObject* parent)
    : QObject(parent),
      taskbarCreatedMessage_(RegisterWindowMessageW(L"TaskbarCreated"))
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    notifyIcon_ = notifyIcon ? std::move(notifyIcon) : NotifyIconFunction(shellNotifyIcon);
}

TrayIconHost::~TrayIconHost()
{
    try {
        hide();
    } catch (...) {
    }
}

Result<void> TrayIconHost::show(HICON icon, QString tooltip)
{
    if (icon == nullptr) {
        return Result<void>::failure(trayError(
            QStringLiteral("tray.icon-unavailable"),
            QStringLiteral("The Companion notification-area icon resource is unavailable.")));
    }

    icon_ = icon;
    tooltip_ = std::move(tooltip);
    notifyVersion4_ = shouldUseNotifyVersion4(myDockFinderPresent());

    const auto windowReady = ensureWindow();
    if (!windowReady.hasValue()) {
        return windowReady;
    }

    return addOrRestoreIcon();
}

void TrayIconHost::hide()
{
    deleteIcon();
    destroyWindow();
}

Result<void> TrayIconHost::restoreAfterTaskbarCreated()
{
    if (icon_ == nullptr) {
        return Result<void>::success();
    }

    visible_ = false;
    const auto restored = addOrRestoreIcon();
    if (!restored.hasValue()) {
        emit runtimeErrorOccurred(restored.error());
    }
    return restored;
}

void TrayIconHost::setRouteStateProvider(RouteStateProvider provider)
{
    routeStateProvider_ = std::move(provider);
}

void TrayIconHost::setRouteState(TrayRouteState state) noexcept
{
    routeState_ = state;
}

Result<void> TrayIconHost::refreshRouteStateForMenu()
{
    if (!routeStateProvider_) {
        return Result<void>::success();
    }

    try {
        routeState_ = routeStateProvider_();
        return Result<void>::success();
    } catch (...) {
        const auto error = trayError(
            QStringLiteral("tray.route-state-failed"),
            QStringLiteral("Could not refresh the Companion tray route state."));
        emit runtimeErrorOccurred(error);
        return Result<void>::failure(error);
    }
}

QStringList TrayIconHost::commandOrder()
{
    return {
        QStringLiteral("toggle-pet"),
        QStringLiteral("toggle-companion-menu"),
        QStringLiteral("separator"),
        QStringLiteral("show-processes"),
        QStringLiteral("show-chat"),
        QStringLiteral("show-settings"),
        QStringLiteral("separator"),
        QStringLiteral("quit"),
    };
}

bool TrayIconHost::shouldUseNotifyVersion4(bool myDockFinderPresent) noexcept
{
    return !myDockFinderPresent;
}

UINT TrayIconHost::callbackEventFromMessage(LPARAM lParam, bool notifyVersion4) noexcept
{
    if (notifyVersion4) {
        return LOWORD(lParam);
    }
    return static_cast<UINT>(lParam);
}

QPoint TrayIconHost::activationPointFromNative(POINT point) noexcept
{
    const auto coordinateSpace =
        coordinateSpaceForNativePoint(point);
    if (coordinateSpace.has_value()) {
        return activationPointFromNative(
            point,
            *coordinateSpace);
    }
    return {point.x, point.y};
}

QPoint TrayIconHost::activationPointFromNative(
    POINT point,
    const TrayMonitorCoordinateSpace& coordinateSpace)
    noexcept
{
    const QPoint nativePoint(point.x, point.y);
    if (!coordinateSpace.nativeGeometry.isValid()
        || !coordinateSpace.qtGeometry.isValid()
        || !qIsFinite(coordinateSpace.devicePixelRatio)
        || coordinateSpace.devicePixelRatio <= 0.0) {
        return nativePoint;
    }

    const QPoint nativeOffset =
        nativePoint
        - coordinateSpace.nativeGeometry.topLeft();
    return coordinateSpace.qtGeometry.topLeft()
        + QPoint(
            qRound(
                nativeOffset.x()
                / coordinateSpace.devicePixelRatio),
            qRound(
                nativeOffset.y()
                / coordinateSpace.devicePixelRatio));
}

QStringList TrayIconHost::menuEntryKeysForState(const TrayRouteState& state)
{
    QStringList entries;
    bool hasMenuItem = false;
    bool separatorPending = false;
    for (const auto& key : commandOrder()) {
        if (key == QStringLiteral("separator")) {
            if (hasMenuItem) {
                separatorPending = true;
            }
            continue;
        }

        const auto* descriptor = descriptorForKey(key);
        if (descriptor == nullptr || !commandIsAvailable(descriptor->command, state)) {
            continue;
        }

        if (separatorPending) {
            entries.append(QStringLiteral("separator"));
            separatorPending = false;
        }
        entries.append(key);
        hasMenuItem = true;
    }
    return entries;
}

QStringList TrayIconHost::menuEntryLabelsForState(const TrayRouteState& state)
{
    QStringList labels;
    for (const auto& key : menuEntryKeysForState(state)) {
        if (key == QStringLiteral("separator")) {
            continue;
        }

        const auto* descriptor = descriptorForKey(key);
        if (descriptor != nullptr) {
            labels.append(labelForCommand(descriptor->command, state));
        }
    }
    return labels;
}

Result<void> TrayIconHost::ensureWindow()
{
    if (hwnd_ != nullptr) {
        return Result<void>::success();
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &TrayIconHost::windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClassName;

    const ATOM registered = RegisterClassExW(&windowClass);
    if (registered == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return Result<void>::failure(trayError(
            QStringLiteral("tray.window-class-failed"),
            QStringLiteral("Could not register the Companion notification-area host window class.")));
    }

    hwnd_ = CreateWindowExW(
        0,
        kWindowClassName,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        this);
    if (hwnd_ == nullptr) {
        return Result<void>::failure(trayError(
            QStringLiteral("tray.window-create-failed"),
            QStringLiteral("Could not create the Companion notification-area host window.")));
    }

    return Result<void>::success();
}

Result<void> TrayIconHost::addOrRestoreIcon()
{
    NOTIFYICONDATAW data{};
    populateNotifyIconData(
        data,
        hwnd_,
        callbackMessage_,
        icon_,
        tooltip_,
        notifyVersion4_);

    if (visible_) {
        if (!notifyIcon_(NIM_MODIFY, &data)) {
            return Result<void>::failure(trayError(
                QStringLiteral("tray.icon-modify-failed"),
                QStringLiteral("Could not refresh the Companion notification-area icon.")));
        }
        return Result<void>::success();
    }

    if (!notifyIcon_(NIM_ADD, &data)) {
        return Result<void>::failure(trayError(
            QStringLiteral("tray.icon-add-failed"),
            QStringLiteral("Could not add the Companion notification-area icon.")));
    }

    visible_ = true;
    if (notifyVersion4_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        if (!notifyIcon_(NIM_SETVERSION, &data)) {
            deleteIcon();
            return Result<void>::failure(trayError(
                QStringLiteral("tray.icon-version-failed"),
                QStringLiteral("Could not select the Companion notification-area callback version.")));
        }
    }

    return Result<void>::success();
}

void TrayIconHost::deleteIcon()
{
    if (!visible_ || hwnd_ == nullptr) {
        visible_ = false;
        return;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(NOTIFYICONDATAW);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_GUID;
    data.guidItem = kCompanionTrayGuid;
    notifyIcon_(NIM_DELETE, &data);
    visible_ = false;
}

void TrayIconHost::destroyWindow() noexcept
{
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void TrayIconHost::handleWindowMessage(
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message == callbackMessage_) {
        handleTrayCallback(wParam, lParam);
        return;
    }

    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        restoreAfterTaskbarCreated();
    }
}

void TrayIconHost::handleTrayCallback(WPARAM wParam, LPARAM lParam)
{
    const UINT event = eventFromCallback(lParam, notifyVersion4_);
    if (!isActivationEvent(event)) {
        return;
    }

    if (!activationDeduplicator_.accept(GetMessageTime())) {
        return;
    }

    const POINT anchorPoint =
        isKeyboardEvent(event) ? keyboardActivationPoint() : cursorPointFromCallback(wParam, event);
    showContextMenu(anchorPoint);
}

void TrayIconHost::showContextMenu(POINT anchorPoint)
{
    lastActivationPoint_ = activationPointFromNative(anchorPoint);
    const auto refreshed = refreshRouteStateForMenu();
    if (!refreshed.hasValue()) {
        return;
    }

    HMENU menu = createMenu();
    if (menu == nullptr) {
        return;
    }

    SetForegroundWindow(hwnd_);
    const UINT selected = TrackPopupMenuEx(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        anchorPoint.x,
        anchorPoint.y,
        hwnd_,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    if (selected != 0) {
        dispatchCommand(selected);
    }
}

HMENU TrayIconHost::createMenu() const
{
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return nullptr;
    }

    for (const auto& key : menuEntryKeysForState(routeState_)) {
        if (key == QStringLiteral("separator")) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }

        const auto* descriptor = descriptorForKey(key);
        if (descriptor == nullptr) {
            continue;
        }

        if (!appendMenuText(menu, descriptor->id, labelForCommand(descriptor->command, routeState_))) {
            DestroyMenu(menu);
            return nullptr;
        }
    }

    if (GetMenuItemCount(menu) <= 0) {
        DestroyMenu(menu);
        return nullptr;
    }
    return menu;
}

void TrayIconHost::dispatchCommand(UINT commandId)
{
    const auto* descriptor = descriptorForId(commandId);
    if (descriptor == nullptr) {
        return;
    }

    invokeCommand(descriptor->command);
}

void TrayIconHost::invokeCommand(Command command)
{
    switch (command) {
    case Command::TogglePet:
        emit showPetRequested();
        break;
    case Command::ToggleCompanionMenu:
        emit showMenuRequested();
        break;
    case Command::ShowProcesses:
        emit showProcessesRequested();
        break;
    case Command::ShowChat:
        emit showChatRequested();
        break;
    case Command::ShowSettings:
        emit showSettingsRequested();
        break;
    case Command::Quit:
        emit quitRequested();
        break;
    }
}

std::optional<QPoint> TrayIconHost::lastActivationPoint() const noexcept
{
    return lastActivationPoint_;
}

POINT TrayIconHost::keyboardActivationPoint() const noexcept
{
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(NOTIFYICONIDENTIFIER);
    identifier.hWnd = hwnd_;
    identifier.uID = kTrayIconId;
    identifier.guidItem = kCompanionTrayGuid;

    RECT rect{};
    if (Shell_NotifyIconGetRect(&identifier, &rect) == S_OK) {
        return {
            rect.left + ((rect.right - rect.left) / 2),
            rect.top + ((rect.bottom - rect.top) / 2),
        };
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    return cursor;
}

POINT TrayIconHost::cursorPointFromCallback(WPARAM wParam, UINT event) const noexcept
{
    if (notifyVersion4_ && event == WM_CONTEXTMENU) {
        const POINT point{
            GET_X_LPARAM(wParam),
            GET_Y_LPARAM(wParam),
        };
        if (point.x != -1 || point.y != -1) {
            return point;
        }
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    return cursor;
}

LRESULT CALLBACK TrayIconHost::windowProcedure(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) noexcept
{
    auto* host = reinterpret_cast<TrayIconHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        host = reinterpret_cast<TrayIconHost*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    if (host != nullptr) {
        try {
            host->handleWindowMessage(message, wParam, lParam);
        } catch (...) {
            return 0;
        }
    }

    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace companion
