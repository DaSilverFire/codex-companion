#include "platform/windows/BackdropController.h"

#include <QVariant>

namespace companion {

namespace {

CompanionError backdropError(
    QString code,
    QString message,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

DwmSystemBackdropType backdropTypeFor(BackdropMode mode)
{
    switch (mode) {
    case BackdropMode::Mica:
        return DwmSystemBackdropType::MainWindow;
    case BackdropMode::WindowsGlass:
        return DwmSystemBackdropType::TransientWindow;
    case BackdropMode::SolidBlack:
        return DwmSystemBackdropType::None;
    }
    return DwmSystemBackdropType::None;
}

} // namespace

BackdropController::BackdropController(INativeWindowApi& nativeApi)
    : nativeApi_(nativeApi)
{
}

Result<BackdropApplication> BackdropController::apply(
    HWND hwnd,
    BackdropMode requested,
    WindowRole role)
{
    if (role == WindowRole::Pet) {
        return Result<BackdropApplication>::success({
            requested,
            requested,
            false,
        });
    }

    const auto build = nativeApi_.currentWindowsBuildNumber();
    if (!build.hasValue()) {
        return fallbackAfterError(hwnd, requested, build.error());
    }
    if (build.value() < 22621) {
        return Result<BackdropApplication>::success({
            requested,
            BackdropMode::SolidBlack,
            requested != BackdropMode::SolidBlack,
        });
    }

    if (requested == BackdropMode::SolidBlack) {
        return applySolidBlack(hwnd, requested, false);
    }

    const auto compositionEnabled = nativeApi_.isDwmCompositionEnabled();
    if (!compositionEnabled.hasValue()) {
        return fallbackAfterError(hwnd, requested, compositionEnabled.error());
    }
    if (!compositionEnabled.value()) {
        return applySolidBlack(hwnd, requested, true);
    }

    const auto highContrastEnabled = nativeApi_.isHighContrastEnabled();
    if (!highContrastEnabled.hasValue()) {
        return fallbackAfterError(hwnd, requested, highContrastEnabled.error());
    }
    if (highContrastEnabled.value()) {
        return applySolidBlack(hwnd, requested, true);
    }

    if (nativeApi_.isRemoteSession()) {
        return applySolidBlack(hwnd, requested, true);
    }

    return applyEnhancedBackdrop(hwnd, requested, backdropTypeFor(requested));
}

Result<BackdropApplication> BackdropController::applySolidBlack(
    HWND hwnd,
    BackdropMode requested,
    bool usedFallback)
{
    const auto backdrop = DwmSystemBackdropType::None;
    const auto cleared = nativeApi_.setDwmWindowAttribute(
        hwnd,
        DWMWA_SYSTEMBACKDROP_TYPE_VALUE,
        &backdrop,
        sizeof(backdrop),
        QStringLiteral("backdrop.solid-black-failed"),
        QStringLiteral("Could not apply the Solid Black Companion material."));
    if (!cleared.hasValue()) {
        return Result<BackdropApplication>::failure(cleared.error());
    }

    return Result<BackdropApplication>::success({
        requested,
        BackdropMode::SolidBlack,
        usedFallback,
    });
}

Result<BackdropApplication> BackdropController::applyEnhancedBackdrop(
    HWND hwnd,
    BackdropMode requested,
    DwmSystemBackdropType backdrop)
{
    const BOOL darkMode = TRUE;
    const auto darkModeApplied = nativeApi_.setDwmWindowAttribute(
        hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE_VALUE,
        &darkMode,
        sizeof(darkMode),
        QStringLiteral("backdrop.dark-mode-failed"),
        QStringLiteral("Could not apply Companion immersive dark mode."));
    if (!darkModeApplied.hasValue()) {
        const auto fallback = applySolidBlack(hwnd, requested, true);
        if (fallback.hasValue()) {
            return fallback;
        }

        return Result<BackdropApplication>::failure(backdropError(
            QStringLiteral("backdrop.fallback-failed"),
            QStringLiteral("Could not clear a failed Companion material."),
            {
                {QStringLiteral("originalOperation"), darkModeApplied.error().code},
                {QStringLiteral("originalContext"), darkModeApplied.error().context},
                {QStringLiteral("fallbackOperation"), fallback.error().code},
                {QStringLiteral("fallbackContext"), fallback.error().context},
            }));
    }

    const auto applied = nativeApi_.setDwmWindowAttribute(
        hwnd,
        DWMWA_SYSTEMBACKDROP_TYPE_VALUE,
        &backdrop,
        sizeof(backdrop),
        QStringLiteral("backdrop.enhanced-failed"),
        QStringLiteral("Could not apply the requested Companion material."));
    if (applied.hasValue()) {
        return Result<BackdropApplication>::success({
            requested,
            requested,
            false,
        });
    }

    const auto fallback = applySolidBlack(hwnd, requested, true);
    if (fallback.hasValue()) {
        return fallback;
    }

    return Result<BackdropApplication>::failure(backdropError(
        QStringLiteral("backdrop.fallback-failed"),
        QStringLiteral("Could not clear a failed Companion material."),
        {
            {QStringLiteral("originalOperation"), applied.error().code},
            {QStringLiteral("originalContext"), applied.error().context},
            {QStringLiteral("fallbackOperation"), fallback.error().code},
            {QStringLiteral("fallbackContext"), fallback.error().context},
        }));
}

Result<BackdropApplication> BackdropController::fallbackAfterError(
    HWND hwnd,
    BackdropMode requested,
    const CompanionError& originalError)
{
    const auto fallback =
        applySolidBlack(hwnd, requested, requested != BackdropMode::SolidBlack);
    if (fallback.hasValue()) {
        return fallback;
    }

    return Result<BackdropApplication>::failure(backdropError(
        QStringLiteral("backdrop.fallback-failed"),
        QStringLiteral("Could not clear a failed Companion material."),
        {
            {QStringLiteral("originalOperation"), originalError.code},
            {QStringLiteral("originalContext"), originalError.context},
            {QStringLiteral("fallbackOperation"), fallback.error().code},
            {QStringLiteral("fallbackContext"), fallback.error().context},
        }));
}

Result<BackdropApplication> BackdropController::logicalSolidBlackFallback(
    BackdropMode requested)
{
    return Result<BackdropApplication>::success({
        requested,
        BackdropMode::SolidBlack,
        true,
    });
}

} // namespace companion
