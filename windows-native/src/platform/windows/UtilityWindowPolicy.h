#pragma once

#include "core/Result.h"
#include "platform/windows/NativeWindowApi.h"

class QWindow;

namespace companion {

struct UtilityWindowStyle final {
    bool isToolWindow = false;
    bool isAppWindow = false;
    bool isNoActivate = false;
    HWND owner = nullptr;
};

class UtilityWindowPolicy final {
public:
    static Result<void> apply(QWindow& window);
    static Result<UtilityWindowStyle> inspect(HWND hwnd);
};

} // namespace companion
