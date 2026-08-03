#pragma once

#include "core/AppSettings.h"
#include "core/Result.h"
#include "platform/windows/NativeWindowApi.h"
#include "platform/windows/WindowCoordinator.h"

namespace companion {

struct BackdropApplication final {
    BackdropMode requested = BackdropMode::SolidBlack;
    BackdropMode effective = BackdropMode::SolidBlack;
    bool usedFallback = false;
};

class BackdropController final {
public:
    explicit BackdropController(INativeWindowApi& nativeApi);

    Result<BackdropApplication> apply(
        HWND hwnd,
        BackdropMode requested,
        WindowRole role);

private:
    Result<BackdropApplication> applySolidBlack(
        HWND hwnd,
        BackdropMode requested,
        bool usedFallback);
    Result<BackdropApplication> applyEnhancedBackdrop(
        HWND hwnd,
        BackdropMode requested,
        DwmSystemBackdropType backdrop);
    Result<BackdropApplication> fallbackAfterError(
        HWND hwnd,
        BackdropMode requested,
        const CompanionError& originalError);
    static Result<BackdropApplication> logicalSolidBlackFallback(
        BackdropMode requested);

    INativeWindowApi& nativeApi_;
};

} // namespace companion
