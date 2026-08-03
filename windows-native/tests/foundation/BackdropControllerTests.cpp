#include "platform/windows/BackdropController.h"

#include <QtTest>

namespace {

class FakeNativeWindowApi final : public companion::INativeWindowApi {
public:
    DWORD windowsBuild = 22631;
    bool compositionEnabled = true;
    bool highContrast = false;
    bool remoteSession = false;
    int immersiveDarkCalls = 0;
    int systemBackdropCalls = 0;
    companion::DwmSystemBackdropType lastBackdrop =
        companion::DwmSystemBackdropType::Auto;
    bool failBuildQuery = false;
    bool failCompositionQuery = false;
    bool failHighContrastQuery = false;
    bool failImmersiveDark = false;
    bool failSystemBackdrop = false;
    bool failBackdropClear = false;

    companion::Result<DWORD> currentWindowsBuildNumber() override
    {
        if (failBuildQuery) {
            return companion::Result<DWORD>::failure({
                QStringLiteral("window.version-failed"),
                QStringLiteral("Could not read the Windows build number."),
                false,
                {{QStringLiteral("native"), QStringLiteral("build")}},
            });
        }
        return companion::Result<DWORD>::success(windowsBuild);
    }

    companion::Result<bool> isDwmCompositionEnabled() override
    {
        if (failCompositionQuery) {
            return companion::Result<bool>::failure({
                QStringLiteral("window.composition-failed"),
                QStringLiteral("Could not read DWM composition state."),
                false,
                {{QStringLiteral("native"), QStringLiteral("composition")}},
            });
        }
        return companion::Result<bool>::success(compositionEnabled);
    }

    companion::Result<bool> isHighContrastEnabled() override
    {
        if (failHighContrastQuery) {
            return companion::Result<bool>::failure({
                QStringLiteral("window.high-contrast-failed"),
                QStringLiteral("Could not read high-contrast state."),
                false,
                {{QStringLiteral("native"), QStringLiteral("highContrast")}},
            });
        }
        return companion::Result<bool>::success(highContrast);
    }

    bool isRemoteSession() override { return remoteSession; }

    companion::Result<void> setDwmWindowAttribute(
        HWND,
        DWORD attribute,
        const void* value,
        DWORD,
        QString code,
        QString message) override
    {
        if (attribute == companion::DWMWA_USE_IMMERSIVE_DARK_MODE_VALUE) {
            ++immersiveDarkCalls;
            if (failImmersiveDark) {
                return companion::Result<void>::failure({
                    std::move(code),
                    std::move(message),
                    false,
                    {{QStringLiteral("native"), QStringLiteral("dark")}},
                });
            }
            return companion::Result<void>::success();
        }

        if (attribute == companion::DWMWA_SYSTEMBACKDROP_TYPE_VALUE) {
            ++systemBackdropCalls;
            lastBackdrop = *static_cast<const companion::DwmSystemBackdropType*>(
                value);
            if (lastBackdrop == companion::DwmSystemBackdropType::None &&
                failBackdropClear) {
                return companion::Result<void>::failure({
                    std::move(code),
                    std::move(message),
                    false,
                    {{QStringLiteral("native"), QStringLiteral("clear")}},
                });
            }
            if (lastBackdrop != companion::DwmSystemBackdropType::None &&
                failSystemBackdrop) {
                return companion::Result<void>::failure({
                    std::move(code),
                    std::move(message),
                    false,
                    {{QStringLiteral("native"), QStringLiteral("backdrop")}},
                });
            }
            return companion::Result<void>::success();
        }

        return companion::Result<void>::failure({
            std::move(code),
            std::move(message),
            false,
            {{QStringLiteral("native"), QStringLiteral("unexpected")}},
        });
    }
};

void expectSolidBlackFallback(
    const companion::Result<companion::BackdropApplication>& result,
    companion::BackdropMode requested)
{
    QVERIFY(result.hasValue());
    QCOMPARE(result.value().requested, requested);
    QCOMPARE(result.value().effective, companion::BackdropMode::SolidBlack);
    QVERIFY(result.value().usedFallback);
}

} // namespace

class BackdropControllerTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsBuildBelow22621FallsBackWithoutCallingUnsupportedAttribute()
    {
        FakeNativeWindowApi api;
        api.windowsBuild = 22620;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::Mica);
        QCOMPARE(api.systemBackdropCalls, 0);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::Auto);
    }

    void compositionDisabledFallsBackWithoutChangingRequestedMode()
    {
        FakeNativeWindowApi api;
        api.compositionEnabled = false;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::WindowsGlass);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void highContrastForcesSolidBlackWithoutChangingRequestedMode()
    {
        FakeNativeWindowApi api;
        api.highContrast = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::Mica);
        QCOMPARE(result.value().effective, companion::BackdropMode::SolidBlack);
        QVERIFY(result.value().usedFallback);
    }

    void remoteSessionFallsBackWithoutChangingRequestedMode()
    {
        FakeNativeWindowApi api;
        api.remoteSession = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::WindowsGlass);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void buildQueryFailureClearsToSolidBlackFallback()
    {
        FakeNativeWindowApi api;
        api.failBuildQuery = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::Mica);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void buildQueryFailureWithRequestedSolidBlackClearsWithoutFallbackFlag()
    {
        FakeNativeWindowApi api;
        api.failBuildQuery = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::SolidBlack,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::SolidBlack);
        QCOMPARE(result.value().effective, companion::BackdropMode::SolidBlack);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void compositionQueryFailureClearsToSolidBlackFallback()
    {
        FakeNativeWindowApi api;
        api.failCompositionQuery = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::WindowsGlass);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void highContrastQueryFailureClearsToSolidBlackFallback()
    {
        FakeNativeWindowApi api;
        api.failHighContrastQuery = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::Mica);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void buildQueryFailureWithClearFailureReturnsCombinedContext()
    {
        FakeNativeWindowApi api;
        api.failBuildQuery = true;
        api.failBackdropClear = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("backdrop.fallback-failed"));
        QCOMPARE(result.error().context.value(QStringLiteral("originalOperation"))
                     .toString(),
                 QStringLiteral("window.version-failed"));
        QVERIFY(result.error().context.contains(QStringLiteral("fallbackContext")));
    }

    void compositionQueryFailureWithClearFailureReturnsCombinedContext()
    {
        FakeNativeWindowApi api;
        api.failCompositionQuery = true;
        api.failBackdropClear = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("backdrop.fallback-failed"));
        QCOMPARE(result.error().context.value(QStringLiteral("originalOperation"))
                     .toString(),
                 QStringLiteral("window.composition-failed"));
        QVERIFY(result.error().context.contains(QStringLiteral("fallbackContext")));
    }

    void highContrastQueryFailureWithClearFailureReturnsCombinedContext()
    {
        FakeNativeWindowApi api;
        api.failHighContrastQuery = true;
        api.failBackdropClear = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("backdrop.fallback-failed"));
        QCOMPARE(result.error().context.value(QStringLiteral("originalOperation"))
                     .toString(),
                 QStringLiteral("window.high-contrast-failed"));
        QVERIFY(result.error().context.contains(QStringLiteral("fallbackContext")));
    }

    void micaAppliesMainWindowBackdrop()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::Mica);
        QCOMPARE(result.value().effective, companion::BackdropMode::Mica);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.immersiveDarkCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::MainWindow);
    }

    void windowsGlassAppliesTransientWindowBackdrop()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::WindowsGlass);
        QCOMPARE(result.value().effective, companion::BackdropMode::WindowsGlass);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.immersiveDarkCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::TransientWindow);
    }

    void solidBlackClearsBackdropWithoutFallback()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::SolidBlack,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::SolidBlack);
        QCOMPARE(result.value().effective, companion::BackdropMode::SolidBlack);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.immersiveDarkCalls, 0);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void solidBlackOnBuildBelow22000DoesNotCallUnsupportedBackdropAttribute()
    {
        FakeNativeWindowApi api;
        api.windowsBuild = 21999;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::SolidBlack,
            companion::WindowRole::Settings);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::SolidBlack);
        QCOMPARE(result.value().effective, companion::BackdropMode::SolidBlack);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.systemBackdropCalls, 0);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::Auto);
    }

    void petNeverReceivesBackdrop()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Pet);

        QVERIFY(result.hasValue());
        QCOMPARE(api.systemBackdropCalls, 0);
    }

    void companionMenuAppliesMicaOnSupportedWindows()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::CompanionMenu);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::Mica);
        QCOMPARE(result.value().effective, companion::BackdropMode::Mica);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.immersiveDarkCalls, 1);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::MainWindow);
    }

    void companionMenuAppliesWindowsGlassOnSupportedWindows()
    {
        FakeNativeWindowApi api;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::CompanionMenu);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().requested, companion::BackdropMode::WindowsGlass);
        QCOMPARE(result.value().effective, companion::BackdropMode::WindowsGlass);
        QVERIFY(!result.value().usedFallback);
        QCOMPARE(api.immersiveDarkCalls, 1);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::TransientWindow);
    }

    void immersiveDarkModeFailureFallsBackToSolidBlack()
    {
        FakeNativeWindowApi api;
        api.failImmersiveDark = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::Mica);
        QCOMPARE(api.systemBackdropCalls, 1);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void enhancedBackdropFailureFallsBackToSolidBlack()
    {
        FakeNativeWindowApi api;
        api.failSystemBackdrop = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::Mica,
            companion::WindowRole::Settings);

        expectSolidBlackFallback(result, companion::BackdropMode::Mica);
        QCOMPARE(api.systemBackdropCalls, 2);
        QCOMPARE(api.lastBackdrop, companion::DwmSystemBackdropType::None);
    }

    void failedClearAfterEnhancedBackdropFailureReturnsCombinedContext()
    {
        FakeNativeWindowApi api;
        api.failSystemBackdrop = true;
        api.failBackdropClear = true;
        companion::BackdropController controller(api);

        const auto result = controller.apply(
            reinterpret_cast<HWND>(1),
            companion::BackdropMode::WindowsGlass,
            companion::WindowRole::Settings);

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("backdrop.fallback-failed"));
        QVERIFY(result.error().context.contains(QStringLiteral("originalOperation")));
        QVERIFY(result.error().context.contains(QStringLiteral("fallbackContext")));
    }
};

QTEST_MAIN(BackdropControllerTests)
#include "BackdropControllerTests.moc"
