#include "platform/windows/UtilityWindowPolicy.h"
#include "platform/windows/BackdropController.h"
#include "platform/windows/TrayWindowPlacement.h"
#include "platform/windows/WindowCoordinator.h"
#include "platform/windows/WindowRegionPolicy.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWindow>
#include <QtTest>
#include <optional>

namespace {

const QString kTransitionTitle =
    QStringLiteral("Codex Companion Test Utility");

QString verifierPath()
{
    const QString path =
        qEnvironmentVariable("COMPANION_WINDOW_VERIFIER");
    if (!path.isEmpty()) {
        return path;
    }
    return QCoreApplication::applicationDirPath() +
        QStringLiteral("/companion-window-verifier.exe");
}

QJsonArray readJsonArray(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).array();
}

QJsonObject entryWithTitle(const QJsonArray& entries, const QString& title)
{
    for (const auto& entry : entries) {
        const QJsonObject object = entry.toObject();
        if (object.value(QStringLiteral("title")).toString() == title) {
            return object;
        }
    }
    return {};
}

bool runVerifierForCurrentProcess(
    const QString& jsonPath,
    int expectedExitCode,
    QJsonObject& matchingEntry)
{
    QProcess verifier;
    verifier.start(
        verifierPath(),
        {
            QStringLiteral("--pid"),
            QString::number(QCoreApplication::applicationPid()),
            QStringLiteral("--json"),
            jsonPath,
        });
    if (!verifier.waitForFinished(10000) ||
        verifier.exitStatus() != QProcess::NormalExit ||
        verifier.exitCode() != expectedExitCode) {
        return false;
    }
    matchingEntry = entryWithTitle(readJsonArray(jsonPath), kTransitionTitle);
    return true;
}

std::optional<QRect> nativeRegionBounds(HWND hwnd)
{
    HRGN region =
        CreateRectRgn(
            0,
            0,
            1,
            1);
    if (region == nullptr) {
        return std::nullopt;
    }

    RECT bounds {};
    const int type =
        GetWindowRgn(
            hwnd,
            region);
    const int boxed =
        type == ERROR
        ? ERROR
        : GetRgnBox(
              region,
              &bounds);
    DeleteObject(region);
    if (type == ERROR
        || boxed == ERROR) {
        return std::nullopt;
    }
    return QRect(
        bounds.left,
        bounds.top,
        bounds.right
            - bounds.left,
        bounds.bottom
            - bounds.top);
}

class FakeNativeWindowApi final : public companion::INativeWindowApi {
public:
    QVector<companion::DwmSystemBackdropType> backdrops;
    QVector<HWND> backdropHwnds;
    QHash<HWND, companion::DwmSystemBackdropType> currentBackdrops;
    HWND failingHwnd = nullptr;
    companion::DwmSystemBackdropType failingBackdrop =
        companion::DwmSystemBackdropType::None;
    QVector<companion::DwmSystemBackdropType> failingBackdrops;
    QHash<HWND, QVector<companion::DwmSystemBackdropType>>
        failingBackdropsByHwnd;
    int failingBackdropAttemptsRemaining = 0;

    companion::Result<DWORD> currentWindowsBuildNumber() override
    {
        return companion::Result<DWORD>::success(22631);
    }

    companion::Result<bool> isDwmCompositionEnabled() override
    {
        return companion::Result<bool>::success(true);
    }

    companion::Result<bool> isHighContrastEnabled() override
    {
        return companion::Result<bool>::success(false);
    }

    bool isRemoteSession() override { return false; }

    companion::Result<void> setDwmWindowAttribute(
        HWND hwnd,
        DWORD attribute,
        const void* value,
        DWORD,
        QString,
        QString) override
    {
        if (attribute == companion::DWMWA_SYSTEMBACKDROP_TYPE_VALUE) {
            const auto backdrop =
                *static_cast<const companion::DwmSystemBackdropType*>(value);
            backdropHwnds.append(hwnd);
            backdrops.append(backdrop);
            if ((failingBackdropAttemptsRemaining > 0) ||
                (hwnd == failingHwnd && backdrop == failingBackdrop) ||
                (hwnd == failingHwnd && failingBackdrops.contains(backdrop)) ||
                failingBackdropsByHwnd.value(hwnd).contains(backdrop)) {
                if (failingBackdropAttemptsRemaining > 0) {
                    --failingBackdropAttemptsRemaining;
                }
                return companion::Result<void>::failure({
                    QStringLiteral("backdrop.test-failed"),
                    QStringLiteral("Synthetic backdrop failure."),
                    false,
                    {{QStringLiteral("role"), QStringLiteral("synthetic")}},
                });
            }
            currentBackdrops[hwnd] = backdrop;
        }
        return companion::Result<void>::success();
    }
};

} // namespace

class UtilityWindowPolicyTests final : public QObject {
    Q_OBJECT

private slots:
    void trayWindowPlacement_data()
    {
        QTest::addColumn<QPoint>("anchor");
        QTest::addColumn<QRect>("availableGeometry");
        QTest::addColumn<QSize>("windowSize");
        QTest::addColumn<int>("margin");
        QTest::addColumn<QPoint>("expected");

        QTest::newRow("top-right")
            << QPoint(1800, 20)
            << QRect(0, 0, 1920, 1080)
            << QSize(392, 520)
            << 12
            << QPoint(1408, 32);
        QTest::newRow("bottom-right")
            << QPoint(1800, 1060)
            << QRect(0, 0, 1920, 1080)
            << QSize(392, 520)
            << 12
            << QPoint(1408, 528);
        QTest::newRow("offset-monitor-left-clamp")
            << QPoint(-1860, 70)
            << QRect(-1920, 40, 1920, 1040)
            << QSize(392, 520)
            << 12
            << QPoint(-1920, 82);
        QTest::newRow("bottom-clamp")
            << QPoint(1780, 300)
            << QRect(100, 50, 1800, 620)
            << QSize(392, 520)
            << 12
            << QPoint(1388, 150);
        QTest::newRow("oversized-panel")
            << QPoint(500, 300)
            << QRect(100, 50, 320, 240)
            << QSize(392, 520)
            << 12
            << QPoint(100, 50);
    }

    void trayWindowPlacement()
    {
        QFETCH(QPoint, anchor);
        QFETCH(QRect, availableGeometry);
        QFETCH(QSize, windowSize);
        QFETCH(int, margin);
        QFETCH(QPoint, expected);

        QCOMPARE(
            companion::TrayWindowPlacement::nearAnchor(
                anchor,
                availableGeometry,
                windowSize,
                margin),
            expected);
    }

    void verifierRejectsMalformedArguments()
    {
        QProcess verifier;
        verifier.start(verifierPath(), {QStringLiteral("--pid")});
        QVERIFY(verifier.waitForFinished(10000));
        QCOMPARE(verifier.exitStatus(), QProcess::NormalExit);
        QCOMPARE(verifier.exitCode(), 1);
    }

    void applySetsToolWindowAndClearsAppWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QWindow window;
        window.setTitle(kTransitionTitle);
        window.resize(240, 120);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const auto hwnd = reinterpret_cast<HWND>(window.winId());
        const LONG_PTR current = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const LONG_PTR intentionallyTaskbarVisible =
            (current | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW;
        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous = SetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE,
            intentionallyTaskbarVisible);
        QVERIFY(previous != 0 || GetLastError() == ERROR_SUCCESS);

        const auto before = companion::UtilityWindowPolicy::inspect(hwnd);
        QVERIFY(before.hasValue());
        QVERIFY(!before.value().isToolWindow);
        QVERIFY(before.value().isAppWindow);

        QJsonObject violatingEntry;
        QVERIFY(runVerifierForCurrentProcess(
            directory.filePath(QStringLiteral("violating.json")),
            2,
            violatingEntry));
        QVERIFY(!violatingEntry.isEmpty());
        QVERIFY(violatingEntry.contains(QStringLiteral("visible")));
        QVERIFY(violatingEntry.contains(QStringLiteral("cloaked")));
        QVERIFY(violatingEntry.contains(QStringLiteral("toolWindow")));
        QVERIFY(violatingEntry.contains(QStringLiteral("appWindow")));
        QVERIFY(violatingEntry.contains(QStringLiteral("owner")));
        QVERIFY(violatingEntry.contains(QStringLiteral("taskbarCandidate")));
        QVERIFY(violatingEntry.contains(QStringLiteral("altTabCandidate")));
        QVERIFY(violatingEntry.value(QStringLiteral("visible")).toBool());
        QVERIFY(!violatingEntry.value(QStringLiteral("cloaked")).toBool());
        QVERIFY(!violatingEntry.value(QStringLiteral("toolWindow")).toBool());
        QVERIFY(violatingEntry.value(QStringLiteral("appWindow")).toBool());
        QCOMPARE(violatingEntry.value(QStringLiteral("owner")).toString(),
                 QStringLiteral("0x0000000000000000"));
        QVERIFY(
            violatingEntry.value(
                QStringLiteral("taskbarCandidate")).toBool());
        QVERIFY(violatingEntry.value(QStringLiteral("altTabCandidate")).toBool());

        const auto applied = companion::UtilityWindowPolicy::apply(window);
        QVERIFY(applied.hasValue());

        const auto style = companion::UtilityWindowPolicy::inspect(hwnd);
        QVERIFY(style.hasValue());
        QVERIFY(style.value().isToolWindow);
        QVERIFY(!style.value().isAppWindow);

        QJsonObject compliantEntry;
        QVERIFY(runVerifierForCurrentProcess(
            directory.filePath(QStringLiteral("compliant.json")),
            0,
            compliantEntry));
        QVERIFY(!compliantEntry.isEmpty());
        QVERIFY(compliantEntry.contains(QStringLiteral("cloaked")));
        QVERIFY(compliantEntry.value(QStringLiteral("toolWindow")).toBool());
        QVERIFY(!compliantEntry.value(QStringLiteral("appWindow")).toBool());
        QVERIFY(
            !compliantEntry.value(
                QStringLiteral("taskbarCandidate")).toBool());
        QVERIFY(!compliantEntry.value(QStringLiteral("altTabCandidate")).toBool());
    }

    void shellClassificationSeparatesTaskbarAndAltTabRepresentatives()
    {
        const auto ownerHwnd =
            reinterpret_cast<HWND>(
                static_cast<quintptr>(1));
        const auto popupHwnd =
            reinterpret_cast<HWND>(
                static_cast<quintptr>(2));
        companion::NativeWindowInfo ownerInfo;
        ownerInfo.visible = true;
        companion::NativeWindowInfo popupInfo;
        popupInfo.visible = true;
        popupInfo.owner = ownerHwnd;

        const auto ownerClassification =
            companion::classifyNativeWindowShell(
                ownerHwnd,
                ownerInfo,
                popupHwnd);
        const auto popupClassification =
            companion::classifyNativeWindowShell(
                popupHwnd,
                popupInfo,
                popupHwnd);

        QVERIFY(ownerClassification.taskbarCandidate);
        QVERIFY(!ownerClassification.altTabCandidate);
        QCOMPARE(
            ownerClassification.altTabRepresentative,
            popupHwnd);
        QVERIFY(!popupClassification.taskbarCandidate);
        QVERIFY(popupClassification.altTabCandidate);
        QCOMPARE(
            popupClassification.altTabRepresentative,
            popupHwnd);
    }

    void coordinatorRecoversFromNativeVisibilityDrift()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow settings;
        settings.setTitle(QStringLiteral("Codex Companion Visibility Recovery"));
        settings.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&settings));

        const auto hwnd = reinterpret_cast<HWND>(settings.winId());
        QVERIFY(settings.isVisible());
        QVERIFY(IsWindowVisible(hwnd) != FALSE);

        ShowWindow(hwnd, SW_HIDE);

        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(hwnd) == FALSE, 1000);
        QVERIFY(settings.isVisible());
        QVERIFY(!coordinator.trayRouteState().settingsVisible);

        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());

        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(hwnd) != FALSE, 1000);
        QVERIFY(settings.isVisible());
        QVERIFY(coordinator.trayRouteState().settingsVisible);
    }

    void coordinatorAssignsNativeOwner()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow owner;
        QQuickWindow child;
        owner.setTitle(QStringLiteral("Codex Companion Owner Utility"));
        child.setTitle(QStringLiteral("Codex Companion Owned Utility"));
        owner.resize(240, 120);
        child.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, owner)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, child)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        QVERIFY(QTest::qWaitForWindowExposed(&child));

        QVERIFY(coordinator
                    .setOwner(companion::WindowRole::Pet,
                              companion::WindowRole::Settings)
                    .hasValue());

        const auto childHwnd = reinterpret_cast<HWND>(child.winId());
        const auto ownerHwnd = reinterpret_cast<HWND>(owner.winId());
        const auto style = companion::UtilityWindowPolicy::inspect(childHwnd);
        QVERIFY(style.hasValue());
        QCOMPARE(style.value().owner, ownerHwnd);
    }

    void coordinatorReappliesNativeOwnerAfterSurfaceRecreation()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow owner;
        QQuickWindow child;
        owner.setTitle(QStringLiteral("Codex Companion Owner Utility"));
        child.setTitle(QStringLiteral("Codex Companion Recreated Owned Utility"));
        owner.resize(240, 120);
        child.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, owner)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, child)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&owner));
        QVERIFY(QTest::qWaitForWindowExposed(&child));
        QVERIFY(coordinator
                    .setOwner(companion::WindowRole::Pet,
                              companion::WindowRole::Settings)
                    .hasValue());

        const auto firstChildHwnd =
            reinterpret_cast<HWND>(child.winId());
        QVERIFY(coordinator.destroy(companion::WindowRole::Pet).hasValue());
        QVERIFY(IsWindow(firstChildHwnd) == FALSE);

        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&child));

        const auto recreatedChildHwnd =
            reinterpret_cast<HWND>(child.winId());
        const auto ownerHwnd =
            reinterpret_cast<HWND>(owner.winId());
        QVERIFY(recreatedChildHwnd != firstChildHwnd);
        const auto style =
            companion::UtilityWindowPolicy::inspect(recreatedChildHwnd);
        QVERIFY(style.hasValue());
        QCOMPARE(style.value().owner, ownerHwnd);
    }

    void coordinatorDeliversFirstMouseToActivatingWindows()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow menu;
        menu.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(
                        companion::WindowRole::CompanionMenu,
                        menu)
                    .hasValue());
        QVERIFY(coordinator
                    .show(companion::WindowRole::CompanionMenu)
                    .hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&menu));

        MSG message {};
        message.hwnd = reinterpret_cast<HWND>(menu.winId());
        message.message = WM_MOUSEACTIVATE;
        qintptr result = 0;

        QVERIFY(coordinator.nativeEventFilter(
            QByteArrayLiteral("windows_generic_MSG"),
            &message,
            &result));
        QCOMPARE(result, static_cast<qintptr>(MA_ACTIVATE));
    }

    void coordinatorDeliversFirstMouseWithoutActivatingAttention()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow attention;
        attention.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(
                        companion::WindowRole::Attention,
                        attention)
                    .hasValue());
        QVERIFY(coordinator
                    .show(companion::WindowRole::Attention)
                    .hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&attention));

        MSG message {};
        message.hwnd =
            reinterpret_cast<HWND>(attention.winId());
        message.message = WM_MOUSEACTIVATE;
        qintptr result = 0;

        QVERIFY(coordinator.nativeEventFilter(
            QByteArrayLiteral("windows_generic_MSG"),
            &message,
            &result));
        QCOMPARE(
            result,
            static_cast<qintptr>(MA_NOACTIVATE));
    }

    void coordinatorLeavesUnregisteredMouseActivationAlone()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow unrelated;
        unrelated.resize(240, 120);
        unrelated.show();
        QVERIFY(QTest::qWaitForWindowExposed(&unrelated));

        MSG message {};
        message.hwnd =
            reinterpret_cast<HWND>(unrelated.winId());
        message.message = WM_MOUSEACTIVATE;
        qintptr result = 0;

        QVERIFY(!coordinator.nativeEventFilter(
            QByteArrayLiteral("windows_generic_MSG"),
            &message,
            &result));
        QCOMPARE(result, static_cast<qintptr>(0));
    }

    void coordinatorReportsTypedErrors()
    {
        companion::WindowCoordinator coordinator;
        QSignalSpy errorSpy(
            &coordinator,
            &companion::WindowCoordinator::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        const auto unknown =
            coordinator.show(static_cast<companion::WindowRole>(999));
        QVERIFY(!unknown.hasValue());
        QCOMPARE(unknown.error().code, QStringLiteral("window.role-unknown"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.role-unknown"));

        const auto missing = coordinator.show(companion::WindowRole::Pet);
        QVERIFY(!missing.hasValue());
        QCOMPARE(missing.error().code, QStringLiteral("window.not-registered"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.not-registered"));

        auto* window = new QQuickWindow();
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, *window)
                    .hasValue());
        delete window;

        const auto destroyed = coordinator.show(companion::WindowRole::Pet);
        QVERIFY(!destroyed.hasValue());
        QCOMPARE(destroyed.error().code, QStringLiteral("window.destroyed"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.destroyed"));
    }

    void coordinatorDestroyTearsDownNativeHandleAndRecreationReappliesPolicy()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow window;
        window.setTitle(QStringLiteral("Codex Companion Coordinator Utility"));
        window.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, window)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const auto firstHwnd = reinterpret_cast<HWND>(window.winId());
        QVERIFY(firstHwnd != nullptr);
        QVERIFY(IsWindow(firstHwnd) != FALSE);

        QVERIFY(coordinator.destroy(companion::WindowRole::Pet).hasValue());
        QVERIFY(IsWindow(firstHwnd) == FALSE);

        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        const auto recreatedHwnd = reinterpret_cast<HWND>(window.winId());
        QVERIFY(recreatedHwnd != nullptr);
        QVERIFY(IsWindow(recreatedHwnd) != FALSE);

        const auto style =
            companion::UtilityWindowPolicy::inspect(recreatedHwnd);
        QVERIFY(style.hasValue());
        QVERIFY(style.value().isToolWindow);
        QVERIFY(!style.value().isAppWindow);
    }

    void coordinatorAppliesBackdropToNonPetWindowsOnly()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QQuickWindow settings;
        QQuickWindow pet;
        settings.resize(240, 120);
        pet.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, pet)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());

        QVERIFY(api.backdrops.contains(
            companion::DwmSystemBackdropType::MainWindow));
        QCOMPARE(std::count(
                     api.backdrops.cbegin(),
                     api.backdrops.cend(),
                     companion::DwmSystemBackdropType::None),
                 0);
    }

    void coordinatorAppliesAndReappliesOptInNativeRegion()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QQuickWindow menu;
        menu.resize(292, 196);
        menu.setProperty(
            "nativeBackdropRegionEnabled",
            true);
        menu.setProperty(
            "nativeBackdropRegionInsetLeft",
            4);
        menu.setProperty(
            "nativeBackdropRegionInsetTop",
            14);
        menu.setProperty(
            "nativeBackdropRegionInsetRight",
            4);
        menu.setProperty(
            "nativeBackdropRegionInsetBottom",
            14);
        menu.setProperty(
            "nativeBackdropRegionRadius",
            28);

        QVERIFY(coordinator
                    .registerWindow(
                        companion::WindowRole::CompanionMenu,
                        menu)
                    .hasValue());
        QVERIFY(coordinator
                    .show(
                        companion::WindowRole::CompanionMenu)
                    .hasValue());
        QVERIFY(QTest::qWaitForWindowExposed(&menu));

        const auto hwnd =
            reinterpret_cast<HWND>(
                menu.winId());
        const auto initial =
            companion::WindowRegionPolicy::
                geometryFor(
                    menu.size(),
                    QMarginsF(
                        4,
                        14,
                        4,
                        14),
                    28,
                    menu.devicePixelRatio());
        QTRY_COMPARE_WITH_TIMEOUT(
            nativeRegionBounds(hwnd),
            std::optional<QRect>(
                initial.bounds),
            1000);

        menu.resize(292, 220);
        const auto resized =
            companion::WindowRegionPolicy::
                geometryFor(
                    menu.size(),
                    QMarginsF(
                        4,
                        14,
                        4,
                        14),
                    28,
                    menu.devicePixelRatio());
        QTRY_COMPARE_WITH_TIMEOUT(
            nativeRegionBounds(hwnd),
            std::optional<QRect>(
                resized.bounds),
            1000);
    }

    void coordinatorReappliesBackdropPreferenceToRegisteredNativeWindows()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QQuickWindow settings;
        QQuickWindow menu;
        QQuickWindow pet;
        settings.resize(240, 120);
        menu.resize(240, 120);
        pet.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::CompanionMenu, menu)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, pet)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::CompanionMenu).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Pet).hasValue());

        api.backdrops.clear();
        QVERIFY(coordinator
                    .setBackdropMode(companion::BackdropMode::WindowsGlass)
                    .hasValue());

        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::TransientWindow),
                 2);
        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::None),
                 0);
        QCOMPARE(
            coordinator.effectiveBackdropMode(
                companion::WindowRole::CompanionMenu).value(),
            companion::BackdropMode::WindowsGlass);
    }

    void coordinatorReapplyAttemptsLaterWindowsAfterFailure()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QSignalSpy errorSpy(
            &coordinator,
            &companion::WindowCoordinator::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        QQuickWindow settings;
        QQuickWindow menu;
        QQuickWindow goal;
        settings.resize(240, 120);
        menu.resize(240, 120);
        goal.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::CompanionMenu, menu)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Goal, goal)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::CompanionMenu).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Goal).hasValue());

        api.backdrops.clear();
        api.backdropHwnds.clear();
        api.failingHwnd = reinterpret_cast<HWND>(settings.winId());
        api.failingBackdrops = {
            companion::DwmSystemBackdropType::TransientWindow,
            companion::DwmSystemBackdropType::None,
        };

        const auto result =
            coordinator.setBackdropMode(companion::BackdropMode::WindowsGlass);

        QVERIFY(!result.hasValue());
        QCOMPARE(result.error().code, QStringLiteral("window.backdrop-reapply-failed"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.backdrop-reapply-failed"));
        QVERIFY(api.backdropHwnds.contains(reinterpret_cast<HWND>(settings.winId())));
        QVERIFY(api.backdropHwnds.contains(reinterpret_cast<HWND>(menu.winId())));
        QVERIFY(api.backdropHwnds.contains(reinterpret_cast<HWND>(goal.winId())));
        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::TransientWindow),
                 3);
        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::None),
                 1);
        QVERIFY(result.error().context.contains(QStringLiteral("failures")));
    }

    void coordinatorFailedBackdropChangeRestoresRequestedAndNativeState()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QSignalSpy errorSpy(
            &coordinator,
            &companion::WindowCoordinator::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        QQuickWindow settings;
        QQuickWindow menu;
        settings.resize(240, 120);
        menu.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::CompanionMenu, menu)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::CompanionMenu).hasValue());

        const auto settingsHwnd = reinterpret_cast<HWND>(settings.winId());
        const auto menuHwnd = reinterpret_cast<HWND>(menu.winId());
        QCOMPARE(api.currentBackdrops.value(settingsHwnd),
                 companion::DwmSystemBackdropType::MainWindow);
        QCOMPARE(api.currentBackdrops.value(menuHwnd),
                 companion::DwmSystemBackdropType::MainWindow);

        api.backdrops.clear();
        api.backdropHwnds.clear();
        api.failingHwnd = settingsHwnd;
        api.failingBackdrops = {
            companion::DwmSystemBackdropType::TransientWindow,
            companion::DwmSystemBackdropType::None,
        };

        const auto failed =
            coordinator.setBackdropMode(companion::BackdropMode::WindowsGlass);

        QVERIFY(!failed.hasValue());
        QCOMPARE(failed.error().code,
                 QStringLiteral("window.backdrop-reapply-failed"));
        QCOMPARE(coordinator.effectiveBackdropMode(companion::WindowRole::Settings).value(),
                 companion::BackdropMode::Mica);
        QCOMPARE(coordinator.effectiveBackdropMode(companion::WindowRole::CompanionMenu).value(),
                 companion::BackdropMode::Mica);
        QCOMPARE(api.currentBackdrops.value(settingsHwnd),
                 companion::DwmSystemBackdropType::MainWindow);
        QCOMPARE(api.currentBackdrops.value(menuHwnd),
                 companion::DwmSystemBackdropType::MainWindow);
        QVERIFY(api.backdrops.contains(
            companion::DwmSystemBackdropType::TransientWindow));
        QVERIFY(api.backdrops.contains(
            companion::DwmSystemBackdropType::MainWindow));

        api.failingHwnd = nullptr;
        api.failingBackdrops.clear();
        api.backdrops.clear();

        QVERIFY(coordinator
                    .setBackdropMode(companion::BackdropMode::WindowsGlass)
                    .hasValue());

        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::TransientWindow),
                 2);
        QCOMPARE(api.backdrops.count(
                     companion::DwmSystemBackdropType::None),
                 0);
    }

    void coordinatorSuccessfulFallbackUpdatesEffectiveBackdropMode()
    {
        FakeNativeWindowApi api;
        companion::BackdropController backdropController(api);
        companion::WindowCoordinator coordinator(backdropController);
        QQuickWindow settings;
        settings.resize(240, 120);

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings, settings)
                    .hasValue());
        QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
        QCOMPARE(coordinator.effectiveBackdropMode(companion::WindowRole::Settings).value(),
                 companion::BackdropMode::Mica);
        QSignalSpy effectiveSpy(
            &coordinator,
            &companion::WindowCoordinator::effectiveBackdropModeChanged);
        QVERIFY(effectiveSpy.isValid());

        api.failingHwnd = reinterpret_cast<HWND>(settings.winId());
        api.failingBackdrop =
            companion::DwmSystemBackdropType::TransientWindow;

        QVERIFY(coordinator
                    .setBackdropMode(companion::BackdropMode::WindowsGlass)
                    .hasValue());

        QCOMPARE(coordinator.effectiveBackdropMode(companion::WindowRole::Settings).value(),
                 companion::BackdropMode::SolidBlack);
        QCOMPARE(effectiveSpy.count(), 1);
        const auto arguments = effectiveSpy.takeFirst();
        QCOMPARE(qvariant_cast<companion::WindowRole>(arguments.at(0)),
                 companion::WindowRole::Settings);
        QCOMPARE(qvariant_cast<companion::BackdropMode>(arguments.at(1)),
                 companion::BackdropMode::SolidBlack);
    }

    void coordinatorRollbackFailureKeepsLastConfirmedEffectiveModesAndReportsPerPolicy()
    {
        const auto reportModes = {
            companion::WindowCoordinator::ErrorReportMode::EmitRuntimeError,
            companion::WindowCoordinator::ErrorReportMode::ReturnOnly,
        };

        for (const auto reportMode : reportModes) {
            FakeNativeWindowApi api;
            companion::BackdropController backdropController(api);
            companion::WindowCoordinator coordinator(backdropController);
            QSignalSpy errorSpy(
                &coordinator,
                &companion::WindowCoordinator::runtimeErrorOccurred);
            QVERIFY(errorSpy.isValid());

            QQuickWindow settings;
            QQuickWindow menu;
            QQuickWindow goal;
            settings.resize(240, 120);
            menu.resize(240, 120);
            goal.resize(240, 120);

            QVERIFY(coordinator
                        .registerWindow(companion::WindowRole::Settings, settings)
                        .hasValue());
            QVERIFY(coordinator
                        .registerWindow(
                            companion::WindowRole::CompanionMenu,
                            menu)
                        .hasValue());
            QVERIFY(coordinator
                        .registerWindow(companion::WindowRole::Goal, goal)
                        .hasValue());
            QVERIFY(coordinator.show(companion::WindowRole::Settings).hasValue());
            QVERIFY(coordinator
                        .show(companion::WindowRole::CompanionMenu)
                        .hasValue());
            QVERIFY(coordinator.show(companion::WindowRole::Goal).hasValue());

            const auto settingsHwnd =
                reinterpret_cast<HWND>(settings.winId());
            const auto menuHwnd = reinterpret_cast<HWND>(menu.winId());
            const auto goalHwnd = reinterpret_cast<HWND>(goal.winId());
            api.failingBackdropsByHwnd[menuHwnd] = {
                companion::DwmSystemBackdropType::TransientWindow,
                companion::DwmSystemBackdropType::None,
            };
            api.failingBackdropsByHwnd[settingsHwnd] = {
                companion::DwmSystemBackdropType::MainWindow,
                companion::DwmSystemBackdropType::None,
            };

            const auto result = coordinator.setBackdropMode(
                companion::BackdropMode::WindowsGlass,
                reportMode);

            QVERIFY(!result.hasValue());
            QCOMPARE(result.error().code,
                     QStringLiteral("window.backdrop-rollback-failed"));
            const QVariantMap reapplyFailure =
                result.error().context.value(
                    QStringLiteral("reapplyFailure")).toMap();
            const QVariantMap rollbackFailure =
                result.error().context.value(
                    QStringLiteral("rollbackFailure")).toMap();
            QCOMPARE(reapplyFailure.value(QStringLiteral("code")).toString(),
                     QStringLiteral("window.backdrop-reapply-failed"));
            QCOMPARE(rollbackFailure.value(QStringLiteral("code")).toString(),
                     QStringLiteral("window.backdrop-reapply-failed"));
            QVERIFY(reapplyFailure.value(
                        QStringLiteral("context")).toMap().contains(
                            QStringLiteral("failures")));
            QVERIFY(rollbackFailure.value(
                        QStringLiteral("context")).toMap().contains(
                            QStringLiteral("failures")));

            QCOMPARE(
                coordinator.effectiveBackdropMode(
                    companion::WindowRole::Settings).value(),
                companion::BackdropMode::WindowsGlass);
            QCOMPARE(
                coordinator.effectiveBackdropMode(
                    companion::WindowRole::CompanionMenu).value(),
                companion::BackdropMode::Mica);
            QCOMPARE(
                coordinator.effectiveBackdropMode(
                    companion::WindowRole::Goal).value(),
                companion::BackdropMode::Mica);
            QCOMPARE(
                coordinator.effectiveBackdropMode(
                    companion::WindowRole::Usage).value(),
                companion::BackdropMode::Mica);
            QCOMPARE(api.currentBackdrops.value(settingsHwnd),
                     companion::DwmSystemBackdropType::TransientWindow);
            QCOMPARE(api.currentBackdrops.value(menuHwnd),
                     companion::DwmSystemBackdropType::MainWindow);
            QCOMPARE(api.currentBackdrops.value(goalHwnd),
                     companion::DwmSystemBackdropType::MainWindow);

            const int expectedErrors =
                reportMode ==
                    companion::WindowCoordinator::ErrorReportMode::EmitRuntimeError
                ? 1
                : 0;
            QCOMPARE(errorSpy.count(), expectedErrors);
            if (expectedErrors == 1) {
                QCOMPARE(
                    qvariant_cast<companion::CompanionError>(
                        errorSpy.takeFirst().at(0)),
                    result.error());
            }
        }
    }
};

QTEST_MAIN(UtilityWindowPolicyTests)
#include "UtilityWindowPolicyTests.moc"
