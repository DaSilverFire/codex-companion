#include "platform/windows/UtilityWindowPolicy.h"

#include <QWindow>

namespace companion {

Result<void> UtilityWindowPolicy::apply(QWindow& window)
{
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const auto current = nativeExtendedStyle(
        hwnd,
        QStringLiteral("window.style-read-failed"),
        QStringLiteral("Could not read the Companion utility-window policy."));
    if (!current.hasValue()) {
        return Result<void>::failure(current.error());
    }

    const LONG_PTR next =
        (current.value() | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
    const auto written = nativeSetExtendedStyle(
        hwnd,
        next,
        QStringLiteral("window.style-failed"),
        QStringLiteral("Could not apply the Companion utility-window policy."));
    if (!written.hasValue()) {
        return written;
    }

    return nativeNotifyFrameChanged(
        hwnd,
        QStringLiteral("window.frame-refresh-failed"),
        QStringLiteral("Could not refresh the Companion utility-window frame."));
}

Result<UtilityWindowStyle> UtilityWindowPolicy::inspect(HWND hwnd)
{
    const auto style = nativeExtendedStyle(
        hwnd,
        QStringLiteral("window.style-read-failed"),
        QStringLiteral("Could not inspect the Companion utility-window policy."));
    if (!style.hasValue()) {
        return Result<UtilityWindowStyle>::failure(style.error());
    }

    return Result<UtilityWindowStyle>::success({
        (style.value() & WS_EX_TOOLWINDOW) != 0,
        (style.value() & WS_EX_APPWINDOW) != 0,
        (style.value() & WS_EX_NOACTIVATE) != 0,
        nativeOwner(hwnd),
    });
}

} // namespace companion
