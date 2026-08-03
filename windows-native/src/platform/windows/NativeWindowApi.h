#pragma once

#include "core/Result.h"

#include <QString>

#define NOMINMAX
#include <windows.h>

namespace companion {

struct NativeWindowInfo final {
    bool visible = false;
    bool cloaked = false;
    LONG_PTR extendedStyle = 0;
    HWND owner = nullptr;
    QString title;
};

struct NativeWindowShellClassification final {
    bool taskbarCandidate = false;
    bool altTabCandidate = false;
    HWND altTabRepresentative = nullptr;
};

constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_VALUE = 20;
constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE_VALUE = 38;

enum class DwmSystemBackdropType : int {
    Auto = 0,
    None = 1,
    MainWindow = 2,
    TransientWindow = 3,
    TabbedWindow = 4,
};

class INativeWindowApi {
public:
    virtual ~INativeWindowApi() = default;

    virtual Result<DWORD> currentWindowsBuildNumber() = 0;
    virtual Result<bool> isDwmCompositionEnabled() = 0;
    virtual Result<bool> isHighContrastEnabled() = 0;
    virtual bool isRemoteSession() = 0;
    virtual Result<void> setDwmWindowAttribute(
        HWND hwnd,
        DWORD attribute,
        const void* value,
        DWORD valueSize,
        QString code,
        QString message) = 0;
};

class NativeWindowApi final : public INativeWindowApi {
public:
    Result<DWORD> currentWindowsBuildNumber() override;
    Result<bool> isDwmCompositionEnabled() override;
    Result<bool> isHighContrastEnabled() override;
    bool isRemoteSession() override;
    Result<void> setDwmWindowAttribute(
        HWND hwnd,
        DWORD attribute,
        const void* value,
        DWORD valueSize,
        QString code,
        QString message) override;
};

CompanionError win32Error(QString code, QString message, DWORD errorCode = GetLastError());

Result<LONG_PTR> nativeExtendedStyle(HWND hwnd, QString code, QString message);
Result<void> nativeSetExtendedStyle(HWND hwnd, LONG_PTR style, QString code, QString message);
Result<void> nativeNotifyFrameChanged(HWND hwnd, QString code, QString message);
HWND nativeOwner(HWND hwnd);
Result<void> nativeSetOwner(HWND hwnd, HWND owner, QString code, QString message);
NativeWindowInfo nativeWindowInfo(HWND hwnd);
NativeWindowShellClassification classifyNativeWindowShell(
    HWND hwnd,
    const NativeWindowInfo& info,
    HWND altTabRepresentative);
HWND nativeAltTabRepresentative(HWND hwnd);
NativeWindowShellClassification nativeWindowShellClassification(HWND hwnd);

} // namespace companion
