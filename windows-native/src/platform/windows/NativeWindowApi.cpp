#include "platform/windows/NativeWindowApi.h"

#include <QVariantMap>
#include <array>

#include <dwmapi.h>

namespace companion {

namespace {

CompanionError hresultError(QString code, QString message, HRESULT result)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {{QStringLiteral("hresult"), QVariant::fromValue<qlonglong>(result)}},
    };
}

using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

} // namespace

CompanionError win32Error(QString code, QString message, DWORD errorCode)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {{QStringLiteral("win32Error"), QVariant::fromValue<qulonglong>(errorCode)}},
    };
}

Result<DWORD> NativeWindowApi::currentWindowsBuildNumber()
{
    auto* module = GetModuleHandleW(L"ntdll.dll");
    if (module == nullptr) {
        return Result<DWORD>::failure(
            win32Error(
                QStringLiteral("window.version-failed"),
                QStringLiteral("Could not load the Windows version module.")));
    }

    auto* proc = reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion"));
    if (proc == nullptr) {
        return Result<DWORD>::failure(
            win32Error(
                QStringLiteral("window.version-failed"),
                QStringLiteral("Could not resolve the Windows version API.")));
    }

    OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    const LONG status = proc(&version);
    if (status < 0) {
        return Result<DWORD>::failure({
            QStringLiteral("window.version-failed"),
            QStringLiteral("Could not read the Windows build number."),
            false,
            {{QStringLiteral("ntstatus"), QVariant::fromValue<qlonglong>(status)}},
        });
    }

    return Result<DWORD>::success(version.dwBuildNumber);
}

Result<bool> NativeWindowApi::isDwmCompositionEnabled()
{
    BOOL enabled = FALSE;
    const HRESULT result = DwmIsCompositionEnabled(&enabled);
    if (FAILED(result)) {
        return Result<bool>::failure(hresultError(
            QStringLiteral("window.composition-failed"),
            QStringLiteral("Could not read DWM composition state."),
            result));
    }
    return Result<bool>::success(enabled != FALSE);
}

Result<bool> NativeWindowApi::isHighContrastEnabled()
{
    HIGHCONTRASTW highContrast = {};
    highContrast.cbSize = sizeof(highContrast);
    if (SystemParametersInfoW(
            SPI_GETHIGHCONTRAST,
            sizeof(highContrast),
            &highContrast,
            0) == FALSE) {
        return Result<bool>::failure(win32Error(
            QStringLiteral("window.high-contrast-failed"),
            QStringLiteral("Could not read high-contrast state.")));
    }
    return Result<bool>::success(
        (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0);
}

bool NativeWindowApi::isRemoteSession()
{
    return GetSystemMetrics(SM_REMOTESESSION) != 0;
}

Result<void> NativeWindowApi::setDwmWindowAttribute(
    HWND hwnd,
    DWORD attribute,
    const void* value,
    DWORD valueSize,
    QString code,
    QString message)
{
    const HRESULT result =
        DwmSetWindowAttribute(hwnd, attribute, value, valueSize);
    if (FAILED(result)) {
        return Result<void>::failure(
            hresultError(std::move(code), std::move(message), result));
    }
    return Result<void>::success();
}

Result<LONG_PTR> nativeExtendedStyle(HWND hwnd, QString code, QString message)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style == 0 && GetLastError() != ERROR_SUCCESS) {
        return Result<LONG_PTR>::failure(
            win32Error(std::move(code), std::move(message)));
    }
    return Result<LONG_PTR>::success(style);
}

Result<void> nativeSetExtendedStyle(
    HWND hwnd,
    LONG_PTR style,
    QString code,
    QString message)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return Result<void>::failure(
            win32Error(std::move(code), std::move(message)));
    }
    return Result<void>::success();
}

Result<void> nativeNotifyFrameChanged(HWND hwnd, QString code, QString message)
{
    SetLastError(ERROR_SUCCESS);
    const BOOL positioned = SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
            SWP_FRAMECHANGED);
    if (positioned == FALSE) {
        return Result<void>::failure(
            win32Error(std::move(code), std::move(message)));
    }
    return Result<void>::success();
}

HWND nativeOwner(HWND hwnd)
{
    return reinterpret_cast<HWND>(GetWindowLongPtrW(hwnd, GWLP_HWNDPARENT));
}

Result<void> nativeSetOwner(HWND hwnd, HWND owner, QString code, QString message)
{
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        hwnd,
        GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(owner));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        return Result<void>::failure(
            win32Error(std::move(code), std::move(message)));
    }
    return Result<void>::success();
}

NativeWindowInfo nativeWindowInfo(HWND hwnd)
{
    NativeWindowInfo info;
    info.visible = IsWindowVisible(hwnd) != FALSE;
    info.extendedStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    info.owner = nativeOwner(hwnd);

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            &cloaked,
            sizeof(cloaked)))) {
        info.cloaked = cloaked != FALSE;
    }

    const int titleLength = GetWindowTextLengthW(hwnd);
    if (titleLength > 0) {
        std::wstring title(static_cast<std::size_t>(titleLength) + 1, L'\0');
        const int copied =
            GetWindowTextW(hwnd, title.data(), static_cast<int>(title.size()));
        title.resize(static_cast<std::size_t>(copied));
        info.title = QString::fromStdWString(title);
    }

    return info;
}

NativeWindowShellClassification classifyNativeWindowShell(
    HWND hwnd,
    const NativeWindowInfo& info,
    HWND altTabRepresentative)
{
    const bool toolWindow =
        (info.extendedStyle & WS_EX_TOOLWINDOW) != 0;
    const bool appWindow =
        (info.extendedStyle & WS_EX_APPWINDOW) != 0;
    const bool noActivate =
        (info.extendedStyle & WS_EX_NOACTIVATE) != 0;
    const bool shellVisible =
        info.visible && !info.cloaked;

    return {
        shellVisible
            && (appWindow
                || (!toolWindow
                    && !noActivate
                    && info.owner == nullptr)),
        shellVisible
            && !toolWindow
            && !noActivate
            && altTabRepresentative == hwnd,
        altTabRepresentative,
    };
}

HWND nativeAltTabRepresentative(HWND hwnd)
{
    const NativeWindowInfo info =
        nativeWindowInfo(hwnd);
    if ((info.extendedStyle & WS_EX_APPWINDOW) != 0) {
        return hwnd;
    }

    HWND walk = GetAncestor(hwnd, GA_ROOTOWNER);
    if (walk == nullptr) {
        walk = hwnd;
    }

    std::array<HWND, 64> visited {};
    std::size_t visitedCount = 0;
    while (walk != nullptr
           && visitedCount < visited.size()) {
        const auto alreadyVisited =
            std::find(
                visited.cbegin(),
                visited.cbegin()
                    + static_cast<std::ptrdiff_t>(
                        visitedCount),
                walk);
        if (alreadyVisited
            != visited.cbegin()
                + static_cast<std::ptrdiff_t>(
                    visitedCount)) {
            break;
        }
        visited.at(visitedCount++) = walk;

        const HWND popup =
            GetLastActivePopup(walk);
        if (popup == nullptr || popup == walk) {
            break;
        }

        const NativeWindowInfo popupInfo =
            nativeWindowInfo(popup);
        const bool popupToolWindow =
            (popupInfo.extendedStyle
             & WS_EX_TOOLWINDOW) != 0;
        const bool popupNoActivate =
            (popupInfo.extendedStyle
             & WS_EX_NOACTIVATE) != 0;
        if (popupInfo.visible
            && !popupInfo.cloaked
            && !popupToolWindow
            && !popupNoActivate) {
            return popup;
        }
        walk = popup;
    }

    return walk;
}

NativeWindowShellClassification
nativeWindowShellClassification(HWND hwnd)
{
    const NativeWindowInfo info =
        nativeWindowInfo(hwnd);
    return classifyNativeWindowShell(
        hwnd,
        info,
        nativeAltTabRepresentative(hwnd));
}

} // namespace companion
