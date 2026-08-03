#include "app/CompanionApplication.h"
#include "app/PetProcessReactionController.h"
#include "app/UpdateViewModel.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/runtime/ProcessListModel.h"
#include "core/SettingsRepository.h"
#include "platform/windows/BackdropController.h"
#include "platform/windows/NativeWindowApi.h"
#include "platform/windows/SingleInstanceGate.h"
#include "platform/windows/TrayIconHost.h"
#include "platform/windows/UtilityWindowPolicy.h"
#include "platform/windows/WindowCoordinator.h"
#include "ui/CompanionShellViewModel.h"
#include "ui/PetViewModel.h"
#include "ui/PetWindowController.h"
#include "ui/SettingsViewModel.h"
#include "ui/WindowBackdropState.h"
#include "ui/pet/PetMenuPlacement.h"
#include "update/UpdateBuildConfiguration.h"
#include "updater-helper/UpdateInstallRequest.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QCursor>
#include <QDateTime>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <utility>

#define NOMINMAX
#include <windows.h>

namespace companion::detail {

class CompanionApplicationTestAccess final {
public:
    static const AppSettings& loadedSettings(
        const CompanionApplication& application)
        noexcept
    {
        return application.loadedSettings_;
    }

    static void setCompanionUtilityCursorPresenceSource(
        CompanionApplication& application,
        std::function<bool()> source)
    {
        application
            .companionUtilityCursorPresenceSource_ =
            std::move(source);
    }

    static void setCompanionProcessCursorPositionSource(
        CompanionApplication& application,
        std::function<QPoint()> source)
    {
        application
            .companionProcessCursorPositionSource_ =
            std::move(source);
    }

    static void setCompanionMenuWindow(
        CompanionApplication& application,
        QQuickWindow& window)
    {
        application.companionMenuWindow_ =
            &window;
    }

    static void reconcileCompanionUtilityPointerHover(
        CompanionApplication& application)
    {
        application
            .reconcileCompanionUtilityPointerHover();
    }

    static quint64
    companionMenuGeometryReconciliationCount(
        const CompanionApplication& application)
    {
        return application
            .companionMenuGeometryReconciliationCount_;
    }

    static Result<void> initializeQmlOnly(
        CompanionApplication& application)
    {
        const auto loaded =
            application.settingsRepository_.load();
        if (!loaded.hasValue()) {
            return Result<void>::failure(
                loaded.error());
        }
        application.loadedSettings_ =
            loaded.value();
        return application.initializeQmlEngine();
    }

    static Result<void>
    applyIsolatedTestStartupRoute(
        CompanionApplication& application)
    {
        return application
            .applyIsolatedTestStartupRoute();
    }

    static Result<void>
    initializeCodexAccountServices(
        CompanionApplication& application,
        const CodexEnvironment& environment,
        QString executable)
    {
        return application
            .initializeCodexAccountServices(
                environment,
                std::move(executable));
    }

    static SettingsViewModel*
    settingsViewModel(
        CompanionApplication& application)
    {
        return application
            .settingsViewModel_.get();
    }
};

} // namespace companion::detail

namespace {

QString uniqueInstanceName()
{
    return QStringLiteral("CodexCompanion.AppTest.") +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString temporarySettingsPath(QTemporaryDir& directory, const QString& fileName)
{
    return directory.filePath(fileName);
}

companion::BridgeDate currentBridgeDate()
{
    constexpr double swiftReferenceDateUnixSeconds =
        978307200.0;
    return {
        static_cast<double>(
            QDateTime::currentMSecsSinceEpoch())
            / 1000.0
        - swiftReferenceDateUnixSeconds,
    };
}

QQuickWindow* topLevelWindowNamed(const QString& objectName)
{
    for (QWindow* window : QGuiApplication::topLevelWindows()) {
        if (window->objectName() == objectName) {
            return qobject_cast<QQuickWindow*>(window);
        }
    }
    return nullptr;
}

bool nativeWindowHasRegion(HWND hwnd)
{
    HRGN region =
        CreateRectRgn(
            0,
            0,
            1,
            1);
    if (region == nullptr) {
        return false;
    }
    const int type =
        GetWindowRgn(
            hwnd,
            region);
    DeleteObject(region);
    return type != ERROR;
}

class ScopedMutex final {
public:
    explicit ScopedMutex(const QString& name)
        : handle_(CreateMutexW(
              nullptr, FALSE, reinterpret_cast<LPCWSTR>(name.utf16())))
    {
    }

    ~ScopedMutex()
    {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    bool isValid() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

class EnvironmentVariableGuard final {
public:
    explicit EnvironmentVariableGuard(QByteArray name)
        : name_(std::move(name)),
          value_(qgetenv(name_.constData())),
          wasSet_(qEnvironmentVariableIsSet(name_.constData()))
    {
    }

    ~EnvironmentVariableGuard()
    {
        if (wasSet_) {
            qputenv(name_.constData(), value_);
        } else {
            qunsetenv(name_.constData());
        }
    }

private:
    QByteArray name_;
    QByteArray value_;
    bool wasSet_ = false;
};

class AppTestNativeWindowApi final : public companion::INativeWindowApi {
public:
    QVector<companion::DwmSystemBackdropType> failingBackdrops;
    QHash<HWND, QVector<companion::DwmSystemBackdropType>>
        failingBackdropsByHwnd;
    QHash<HWND, companion::DwmSystemBackdropType> currentBackdrops;

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
            if (failingBackdrops.contains(backdrop) ||
                failingBackdropsByHwnd.value(hwnd).contains(backdrop)) {
                return companion::Result<void>::failure({
                    QStringLiteral("backdrop.test-failed"),
                    QStringLiteral("Synthetic backdrop failure."),
                    false,
                    {},
                });
            }
            currentBackdrops[hwnd] = backdrop;
        }
        return companion::Result<void>::success();
    }
};

class ProcessHoverProbeWindow final : public QQuickWindow {
    Q_OBJECT

public:
    Q_INVOKABLE void reconcileProcessHoverAt(
        QVariant windowX,
        QVariant windowY)
    {
        ++reconciliationCount;
        lastWindowPoint = {
            windowX.toInt(),
            windowY.toInt(),
        };
    }

    int reconciliationCount = 0;
    QPoint lastWindowPoint;
};

} // namespace

class CompanionApplicationTests final : public QObject {
    Q_OBJECT

private slots:
    void defaultSettingsPathUsesAppConfigLocation()
    {
        const auto path = companion::CompanionApplication::defaultSettingsFilePath();
        const QString appConfigLocation =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

        QVERIFY(path.hasValue());
        QVERIFY(!appConfigLocation.isEmpty());
        QCOMPARE(QFileInfo(path.value()).fileName(),
                 QStringLiteral("CodexCompanion.ini"));
        QCOMPARE(QFileInfo(path.value()).dir().path(), appConfigLocation);
    }

    void launchStandardPathsUseTestModeOnlyAfterExplicitOptIn()
    {
        EnvironmentVariableGuard guard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"));
        QStandardPaths::setTestModeEnabled(false);
        qunsetenv(
            "CODEX_COMPANION_TEST_STANDARD_PATHS");

        companion::CompanionApplication::
            configureStandardPathsForLaunch();

        QVERIFY(
            !QStandardPaths::isTestModeEnabled());

        qputenv(
            "CODEX_COMPANION_TEST_STANDARD_PATHS",
            QByteArrayLiteral("1"));
        companion::CompanionApplication::
            configureStandardPathsForLaunch();

        QVERIFY(
            QStandardPaths::isTestModeEnabled());
        QStandardPaths::setTestModeEnabled(false);
    }

    void productionAccountServicesUseLocalCompanionDataAndSettingsModel()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(
                        application)
                    .hasValue());
        companion::CodexEnvironment environment;
        environment.localAppData =
            directory.filePath(
                QStringLiteral("LocalAppData"));
        environment.codexHome =
            directory.filePath(
                QStringLiteral(".codex"));

        const auto initialized =
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeCodexAccountServices(
                        application,
                        environment,
                        directory.filePath(
                            QStringLiteral(
                                "codex.exe")));

        QVERIFY(initialized.hasValue());
        auto* settings =
            companion::detail::
                CompanionApplicationTestAccess::
                    settingsViewModel(
                        application);
        QVERIFY(settings != nullptr);
        QVERIFY(
            settings->codexAccountsAvailable());
        QVERIFY(
            settings->addCodexAccount(
                QStringLiteral("Main")));
        QVERIFY(
            QFileInfo::exists(
                QDir(environment.localAppData)
                    .filePath(
                        QStringLiteral(
                            "Codex Companion/"
                            "codex-account-profiles.json"))));
        QVERIFY(
            !QFileInfo::exists(
                QDir(environment.localAppData)
                    .filePath(
                        QStringLiteral(
                            "Codex Companion/"
                            "Codex Profiles/"
                            "credentials.json"))));
    }

    void isolatedTestStartupRouteRequiresStandardPathsOptIn()
    {
        EnvironmentVariableGuard standardPathsGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"));
        EnvironmentVariableGuard routeGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STARTUP_ROUTE"));
        qunsetenv(
            "CODEX_COMPANION_TEST_STANDARD_PATHS");
        qputenv(
            "CODEX_COMPANION_TEST_STARTUP_ROUTE",
            QByteArrayLiteral("processes"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(
                        application)
                    .hasValue());
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(menuWindow != nullptr);
        QVERIFY(!menuWindow->isVisible());

        const auto applied =
            companion::detail::
                CompanionApplicationTestAccess::
                    applyIsolatedTestStartupRoute(
                        application);

        QVERIFY(applied.hasValue());
        QVERIFY(!menuWindow->isVisible());
    }

    void isolatedTestStartupRouteOpensRequestedSurface_data()
    {
        QTest::addColumn<QByteArray>("route");
        QTest::addColumn<QString>("expectedRoute");
        QTest::newRow("processes")
            << QByteArrayLiteral("processes")
            << QStringLiteral("processes");
        QTest::newRow("local-chat")
            << QByteArrayLiteral("local-chat")
            << QStringLiteral("local-chat");
    }

    void isolatedTestStartupRouteOpensRequestedSurface()
    {
        QFETCH(QByteArray, route);
        QFETCH(QString, expectedRoute);
        EnvironmentVariableGuard standardPathsGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"));
        EnvironmentVariableGuard routeGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STARTUP_ROUTE"));
        qputenv(
            "CODEX_COMPANION_TEST_STANDARD_PATHS",
            QByteArrayLiteral("1"));
        qputenv(
            "CODEX_COMPANION_TEST_STARTUP_ROUTE",
            route);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(
                        application)
                    .hasValue());
        auto* shell =
            application.findChild<
                companion::
                    CompanionShellViewModel*>();
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(shell != nullptr);
        QVERIFY(menuWindow != nullptr);
        QVERIFY(!menuWindow->isVisible());

        const auto applied =
            companion::detail::
                CompanionApplicationTestAccess::
                    applyIsolatedTestStartupRoute(
                        application);

        QVERIFY(applied.hasValue());
        QCOMPARE(
            shell->routeMode(),
            expectedRoute);
        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
    }

    void isolatedTestStartupRouteRejectsUnknownValues()
    {
        EnvironmentVariableGuard standardPathsGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"));
        EnvironmentVariableGuard routeGuard(
            QByteArrayLiteral(
                "CODEX_COMPANION_TEST_STARTUP_ROUTE"));
        qputenv(
            "CODEX_COMPANION_TEST_STANDARD_PATHS",
            QByteArrayLiteral("1"));
        qputenv(
            "CODEX_COMPANION_TEST_STARTUP_ROUTE",
            QByteArrayLiteral("unknown"));

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(
                        application)
                    .hasValue());

        const auto applied =
            companion::detail::
                CompanionApplicationTestAccess::
                    applyIsolatedTestStartupRoute(
                        application);

        QVERIFY(!applied.hasValue());
        QCOMPARE(
            applied.error().code,
            QStringLiteral(
                "app.test_startup_route_invalid"));
    }

    void duplicateStartReturnsAlreadyRunningAfterForwardingActivation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication primary(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("primary.ini")));
        QSignalSpy showSpy(
            &primary,
            &companion::CompanionApplication::settingsShowRequested);

        const auto primaryStarted = primary.start();
        const QString primaryStartFailure =
            primaryStarted.hasValue()
            ? QString()
            : primaryStarted.error().code
                + QStringLiteral(": ")
                + primaryStarted.error().message;
        QVERIFY2(
            primaryStarted.hasValue(),
            qPrintable(primaryStartFailure));
        QCOMPARE(showSpy.count(), 1);
        auto* settingsWindow =
            topLevelWindowNamed(QStringLiteral("settingsWindow"));
        QVERIFY(settingsWindow != nullptr);

        QTRY_VERIFY_WITH_TIMEOUT(settingsWindow->isVisible(), 1000);
        const auto settingsHwnd =
            reinterpret_cast<HWND>(settingsWindow->winId());
        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(settingsHwnd) != FALSE, 1000);

        ShowWindow(settingsHwnd, SW_HIDE);
        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(settingsHwnd) == FALSE, 1000);
        QVERIFY(settingsWindow->isVisible());
        const qsizetype showCountBeforeDuplicate = showSpy.count();

        companion::CompanionApplication duplicate(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("duplicate.ini")));

        const auto duplicateStarted = duplicate.start();

        QVERIFY(!duplicateStarted.hasValue());
        QCOMPARE(duplicateStarted.error().code,
                 QStringLiteral("app.already-running"));
        QTRY_COMPARE_WITH_TIMEOUT(
            showSpy.count(),
            showCountBeforeDuplicate + 1,
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(IsWindowVisible(settingsHwnd) != FALSE, 1000);
    }

    void primaryStartOpensSettingsAndRegistersSharedRoutes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));

        QVERIFY(application.start().hasValue());
        auto* coordinator =
            application.findChild<companion::WindowCoordinator*>();
        auto* settingsWindow =
            topLevelWindowNamed(QStringLiteral("settingsWindow"));
        auto* companionWindow =
            topLevelWindowNamed(QStringLiteral("companionMenuWindow"));
        auto* usageWindow =
            topLevelWindowNamed(QStringLiteral("usageWindow"));
        auto* attentionWindow =
            topLevelWindowNamed(
                QStringLiteral("attentionWindow"));
        auto* petWindow =
            topLevelWindowNamed(QStringLiteral("petWindow"));
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* petController =
            application.findChild<
                companion::PetWindowController*>();
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* reactionController =
            application.findChild<
                companion::
                    PetProcessReactionController*>();
        auto* updateViewModel =
            application.findChild<
                companion::UpdateViewModel*>();

        QVERIFY(coordinator != nullptr);
        QVERIFY(settingsWindow != nullptr);
        QVERIFY(companionWindow != nullptr);
        QVERIFY(usageWindow != nullptr);
        QVERIFY(attentionWindow != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(petModel != nullptr);
        QVERIFY(petController != nullptr);
        QVERIFY(shell != nullptr);
        QVERIFY(reactionController != nullptr);
        QVERIFY(updateViewModel != nullptr);
        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("local-chat"));
        QCOMPARE(
            qvariant_cast<QObject*>(
                settingsWindow->property(
                    "routingModel")),
            shell);
        QCOMPARE(
            updateViewModel
                ->installedVersion(),
            QStringLiteral(
                COMPANION_WINDOWS_VERSION));
        QCOMPARE(
            updateViewModel
                ->installedBuild(),
            1);
        QVERIFY(settingsWindow->isVisible());
        QVERIFY(!petController->roamingSuspended());
        QVERIFY(!companionWindow->isVisible());
        QVERIFY(!usageWindow->isVisible());
        QVERIFY(!attentionWindow->isVisible());
        QVERIFY(petWindow->isVisible());
        QVERIFY(petModel->visible());

        const auto petStyle =
            companion::UtilityWindowPolicy::inspect(
                reinterpret_cast<HWND>(
                    petWindow->winId()));
        QVERIFY(petStyle.hasValue());
        QVERIFY(petStyle.value().isToolWindow);
        QVERIFY(!petStyle.value().isAppWindow);
        const auto usageStyle =
            companion::UtilityWindowPolicy::inspect(
                reinterpret_cast<HWND>(
                    usageWindow->winId()));
        QVERIFY(usageStyle.hasValue());
        QVERIFY(usageStyle.value().isToolWindow);
        QVERIFY(!usageStyle.value().isAppWindow);
        const auto attentionStyle =
            companion::UtilityWindowPolicy::inspect(
                reinterpret_cast<HWND>(
                    attentionWindow->winId()));
        QVERIFY(attentionStyle.hasValue());
        QVERIFY(attentionStyle.value().isToolWindow);
        QVERIFY(!attentionStyle.value().isAppWindow);
        QVERIFY(attentionStyle.value().isNoActivate);

        const auto state = coordinator->trayRouteState();
        QVERIFY(state.petRegistered);
        QVERIFY(state.petVisible);
        QVERIFY(state.companionMenuRegistered);
        QVERIFY(state.processesRegistered);
        QVERIFY(state.chatRegistered);
        QVERIFY(!state.companionMenuVisible);
        QVERIFY(state.settingsVisible);
    }

    void petAttentionOpensProcessesAndReusesTrayOnlyWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));

        QVERIFY(application.start().hasValue());
        auto* controller =
            application.findChild<
                companion::
                    PetProcessReactionController*>();
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* attentionWindow =
            topLevelWindowNamed(
                QStringLiteral("attentionWindow"));
        auto* companionWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(controller != nullptr);
        QVERIFY(shell != nullptr);
        QVERIFY(petModel != nullptr);
        QVERIFY(attentionWindow != nullptr);
        QVERIFY(companionWindow != nullptr);

        companion::ProcessListModel processModel;
        controller->setProcessModel(&processModel);
        companion::BridgeTask task;
        task.id =
            QStringLiteral("task-1");
        task.title =
            QStringLiteral("Windows parity");
        task.preview =
            QStringLiteral("Working");
        task.status =
            companion::TaskStatus::Running;
        task.updatedAt = currentBridgeDate();
        companion::CodexProcessSnapshot
            processSnapshot;
        processSnapshot.tasks = {task};
        processModel.setSnapshot(
            processSnapshot);

        task.preview =
            QStringLiteral(
                "Approve the build command");
        task.status =
            companion::TaskStatus::Waiting;
        task.needsApproval = true;
        processSnapshot.tasks = {task};
        processModel.setSnapshot(
            processSnapshot);

        QTRY_VERIFY_WITH_TIMEOUT(
            attentionWindow->isVisible(),
            1000);
        QVERIFY(!companionWindow->isVisible());
        const WId attentionWindowId =
            attentionWindow->winId();
        const auto attentionStyle =
            companion::UtilityWindowPolicy::inspect(
                reinterpret_cast<HWND>(
                    attentionWindowId));
        QVERIFY(attentionStyle.hasValue());
        QVERIFY(attentionStyle.value().isToolWindow);
        QVERIFY(!attentionStyle.value().isAppWindow);
        QVERIFY(attentionStyle.value().isNoActivate);

        QVERIFY(QMetaObject::invokeMethod(
            attentionWindow,
            "openRequested",
            Qt::DirectConnection));

        QTRY_VERIFY_WITH_TIMEOUT(
            !attentionWindow->isVisible(),
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            companionWindow->isVisible(),
            1000);
        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("processes"));
        QVERIFY(petModel->menuOpen());

        petModel->setMenuOpen(false);
        task.status =
            companion::TaskStatus::Running;
        task.needsApproval = false;
        processSnapshot.tasks = {task};
        processModel.setSnapshot(
            processSnapshot);
        task.status =
            companion::TaskStatus::Failed;
        task.preview =
            QStringLiteral("Build failed");
        processSnapshot.tasks = {task};
        processModel.setSnapshot(
            processSnapshot);

        QTRY_VERIFY_WITH_TIMEOUT(
            attentionWindow->isVisible(),
            1000);
        QCOMPARE(
            attentionWindow->winId(),
            attentionWindowId);

        QVERIFY(QMetaObject::invokeMethod(
            attentionWindow,
            "dismissRequested",
            Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(
            !attentionWindow->isVisible(),
            1000);
        QVERIFY(
            attentionWindow->handle()
            != nullptr);
        QVERIFY(!application.explicitQuitRequested());
    }

    void hiddenPetPreferenceRestoresAndTrayTogglePersists()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini"));
        companion::SettingsRepository repository(
            settingsPath);
        companion::AppSettings settings;
        settings.petVisible = false;
        QVERIFY(repository.save(settings).hasValue());

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            settingsPath);
        QVERIFY(application.start().hasValue());
        auto* tray =
            application.findChild<
                companion::TrayIconHost*>();
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* petWindow =
            topLevelWindowNamed(
                QStringLiteral("petWindow"));
        QVERIFY(tray != nullptr);
        QVERIFY(petModel != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(!petModel->visible());
        QVERIFY(!petWindow->isVisible());

        tray->invokeCommand(
            companion::TrayIconHost::Command::
                TogglePet);

        QTRY_VERIFY_WITH_TIMEOUT(
            petWindow->isVisible(),
            1000);
        QVERIFY(petModel->visible());
        auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(persisted.value().petVisible);

        tray->invokeCommand(
            companion::TrayIconHost::Command::
                TogglePet);

        QTRY_VERIFY_WITH_TIMEOUT(
            !petWindow->isVisible(),
            1000);
        QVERIFY(!petModel->visible());
        persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(!persisted.value().petVisible);
    }

    void petMenuStateReusesCompanionMenuWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(petModel != nullptr);
        QVERIFY(menuWindow != nullptr);
        QVERIFY(!menuWindow->isVisible());

        petModel->setMenuOpen(true);

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
        QVERIFY(petModel->menuOpen());

        petModel->setMenuOpen(false);

        QTRY_VERIFY_WITH_TIMEOUT(
            !menuWindow->isVisible(),
            1000);
        QVERIFY(!petModel->menuOpen());
    }

    void visiblePetMenuRepositionsAfterContentDrivenResize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* petWindow =
            topLevelWindowNamed(
                QStringLiteral("petWindow"));
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(petModel != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(menuWindow != nullptr);

        petModel->setAllowAutonomousMovement(false);
        petWindow->setPosition(520, 520);
        menuWindow->setMinimumSize(QSize(292, 180));
        menuWindow->setMaximumSize(QSize(292, 180));
        menuWindow->resize(292, 180);
        petModel->setMenuOpen(true);

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
        QScreen* screen =
            QGuiApplication::screenAt(
                petWindow->geometry().center());
        QVERIFY(screen != nullptr);
        const auto expectedOrigin = [&] {
            return companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            petModel->controlsVisible()),
                    menuWindow->size(),
                    screen->availableGeometry());
        };
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            expectedOrigin(),
            1000);

        const QPoint compactOrigin =
            menuWindow->position();
        // QML route content can expand the visible utility window after it opens.
        menuWindow->setMinimumSize(
            QSize(292, 346));
        menuWindow->setMaximumSize(
            QSize(292, 346));
        menuWindow->resize(292, 346);

        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->size(),
            QSize(292, 346),
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            expectedOrigin(),
            1000);
        QVERIFY(
            menuWindow->position()
            != compactOrigin);
    }

    void petMenuControlVisibilityChangeUsesBoundedMotion()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* petController =
            application.findChild<
                companion::PetWindowController*>();
        auto* petWindow =
            topLevelWindowNamed(
                QStringLiteral("petWindow"));
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(petModel != nullptr);
        QVERIFY(petController != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(menuWindow != nullptr);

        petController->stop();
        petModel->setAllowAutonomousMovement(false);
        petModel->setHideControlsUntilHover(true);
        petModel->setPointerHovered(false);
        petModel->setControlsHovered(false);
        petWindow->setPosition(520, 520);
        menuWindow->setMinimumSize(
            QSize(292, 180));
        menuWindow->setMaximumSize(
            QSize(292, 180));
        menuWindow->resize(292, 180);
        petModel->setMenuOpen(true);

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
        QScreen* screen =
            QGuiApplication::screenAt(
                petWindow->geometry().center());
        QVERIFY(screen != nullptr);
        const QPoint originalCursor =
            QCursor::pos();
        QCursor::setPos(
            screen->availableGeometry()
                .topLeft()
            + QPoint(4, 4));
        const QPoint hiddenOrigin =
            companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            false),
                    menuWindow->size(),
                    screen->availableGeometry());
        const QPoint visibleOrigin =
            companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            true),
                    menuWindow->size(),
                    screen->availableGeometry());
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            hiddenOrigin,
            1000);
        QSignalSpy positionSpy(
            menuWindow,
            &QWindow::yChanged);

        QCursor::setPos(
            petWindow->position()
            + QPoint(60, 100));
        petModel->setPointerHovered(true);

        QCOMPARE(
            menuWindow->position(),
            hiddenOrigin);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            visibleOrigin,
            1000);
        bool observedIntermediateOrigin = false;
        for (const auto& arguments :
             std::as_const(positionSpy)) {
            const int y = arguments.at(0).toInt();
            if (y != hiddenOrigin.y()
                && y != visibleOrigin.y()) {
                observedIntermediateOrigin = true;
                break;
            }
        }
        QVERIFY(observedIntermediateOrigin);
        QCursor::setPos(originalCursor);
    }

    void hoveringMenuFreezesControlAnchorMotionButRepositionsForContentSize()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        bool cursorOverMenu = false;
        companion::detail::
            CompanionApplicationTestAccess::
                setCompanionUtilityCursorPresenceSource(
                    application,
                    [&cursorOverMenu] {
                        return cursorOverMenu;
                    });
        QVERIFY(application.start().hasValue());
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* petController =
            application.findChild<
                companion::PetWindowController*>();
        auto* petWindow =
            topLevelWindowNamed(
                QStringLiteral("petWindow"));
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(petModel != nullptr);
        QVERIFY(petController != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(menuWindow != nullptr);

        petController->stop();
        petModel->setAllowAutonomousMovement(false);
        petModel->setControlsHovered(false);
        petModel->setPointerHovered(true);
        petWindow->setPosition(520, 520);
        menuWindow->setMinimumSize(
            QSize(292, 94));
        menuWindow->setMaximumSize(
            QSize(292, 94));
        menuWindow->resize(292, 94);
        petModel->setMenuOpen(true);

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
        QScreen* screen =
            QGuiApplication::screenAt(
                petWindow->geometry().center());
        QVERIFY(screen != nullptr);
        const QPoint visibleOrigin =
            companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            true),
                    menuWindow->size(),
                    screen->availableGeometry());
        const QPoint hiddenOrigin =
            companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            false),
                    menuWindow->size(),
                    screen->availableGeometry());
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            visibleOrigin,
            1000);

        cursorOverMenu = true;
        QTest::qWait(100);
        QCOMPARE(
            menuWindow->position(),
            visibleOrigin);

        const QSize compactSize(292, 94);
        const QSize expandedSize(292, 123);
        const QPoint expandedOrigin =
            companion::PetMenuPlacement::
                positionedOrigin(
                    companion::PetMenuPlacement::
                        anchorFrame(
                            petWindow->geometry(),
                            true),
                    expandedSize,
                    screen->availableGeometry());
        QCOMPARE(
            expandedOrigin.y()
                + expandedSize.height(),
            visibleOrigin.y()
                + compactSize.height());
        const quint64 expansionReconciliations =
            companion::detail::
                CompanionApplicationTestAccess::
                    companionMenuGeometryReconciliationCount(
                        application);
        menuWindow->setMinimumSize(expandedSize);
        menuWindow->setMaximumSize(expandedSize);
        menuWindow->resize(expandedSize);

        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->size(),
            expandedSize,
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            expandedOrigin,
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            companion::detail::
                CompanionApplicationTestAccess::
                    companionMenuGeometryReconciliationCount(
                        application),
            expansionReconciliations + 1,
            1000);
        QVERIFY(menuWindow->isVisible());
        QVERIFY(petModel->menuOpen());

        const quint64 collapseReconciliations =
            companion::detail::
                CompanionApplicationTestAccess::
                    companionMenuGeometryReconciliationCount(
                        application);
        menuWindow->setMinimumSize(compactSize);
        menuWindow->setMaximumSize(compactSize);
        menuWindow->resize(compactSize);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->size(),
            compactSize,
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            visibleOrigin,
            1000);
        QTRY_COMPARE_WITH_TIMEOUT(
            companion::detail::
                CompanionApplicationTestAccess::
                    companionMenuGeometryReconciliationCount(
                        application),
            collapseReconciliations + 1,
            1000);
        QVERIFY(menuWindow->isVisible());
        QVERIFY(petModel->menuOpen());

        petModel->setPointerHovered(false);

        QTest::qWait(700);
        QCOMPARE(
            menuWindow->position(),
            visibleOrigin);
        QVERIFY(
            menuWindow->position()
            != hiddenOrigin);

        cursorOverMenu = false;

        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow->position(),
            hiddenOrigin,
            1000);
        petModel->setMenuOpen(false);
    }

    void stationaryUtilityHoverPollingDoesNotRetargetProcessCardHover()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        ProcessHoverProbeWindow menuWindow;
        menuWindow.setPosition(320, 220);
        menuWindow.resize(292, 274);
        menuWindow.setProperty(
            "hoveredProcessId",
            QStringLiteral("thread-active"));
        menuWindow.show();
        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow.isVisible(),
            1000);
        QPoint cursorPosition(360, 260);
        bool cursorOverUtility = true;
        companion::detail::
            CompanionApplicationTestAccess::
                setCompanionMenuWindow(
                    application,
                    menuWindow);
        companion::detail::
            CompanionApplicationTestAccess::
                setCompanionUtilityCursorPresenceSource(
                    application,
                    [&cursorOverUtility] {
                        return cursorOverUtility;
                    });
        companion::detail::
            CompanionApplicationTestAccess::
                setCompanionProcessCursorPositionSource(
                    application,
                    [&cursorPosition] {
                        return cursorPosition;
                    });

        companion::detail::
            CompanionApplicationTestAccess::
                reconcileCompanionUtilityPointerHover(
                    application);

        const QPoint initialLocalPointer =
            menuWindow.mapFromGlobal(
                cursorPosition);
        QCOMPARE(
            menuWindow.reconciliationCount,
            1);
        QCOMPARE(
            menuWindow.lastWindowPoint,
            initialLocalPointer);

        menuWindow.setPosition(280, 180);
        QTRY_COMPARE_WITH_TIMEOUT(
            menuWindow.position(),
            QPoint(280, 180),
            1000);
        companion::detail::
            CompanionApplicationTestAccess::
                reconcileCompanionUtilityPointerHover(
                    application);

        QCOMPARE(
            menuWindow.reconciliationCount,
            1);
        QCOMPARE(
            menuWindow.lastWindowPoint,
            initialLocalPointer);

        cursorPosition = QPoint(410, 310);
        companion::detail::
            CompanionApplicationTestAccess::
                reconcileCompanionUtilityPointerHover(
                    application);

        QCOMPARE(
            menuWindow.reconciliationCount,
            2);
        QCOMPARE(
            menuWindow.lastWindowPoint,
            menuWindow.mapFromGlobal(
                cursorPosition));

        cursorOverUtility = false;
        cursorPosition = QPoint(40, 30);
        companion::detail::
            CompanionApplicationTestAccess::
                reconcileCompanionUtilityPointerHover(
                    application);

        QCOMPARE(
            menuWindow.reconciliationCount,
            3);
        QCOMPARE(
            menuWindow.lastWindowPoint,
            menuWindow.mapFromGlobal(
                cursorPosition));
    }

    void petUtilitiesFollowTheActiveRouteAndOpenTheModelPicker()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        auto* reactionController =
            application.findChild<
                companion::
                    PetProcessReactionController*>();
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(shell != nullptr);
        QVERIFY(petModel != nullptr);
        QVERIFY(reactionController != nullptr);
        QVERIFY(menuWindow != nullptr);
        QVERIFY(!menuWindow->isVisible());

        application.showChatFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->isVisible(),
            1000);
        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("local-chat"));
        petModel->setSelectedAnimation(
            QStringLiteral("review"));
        QCOMPARE(
            petModel->renderedAnimation(),
            QStringLiteral("review"));

        companion::ProcessListModel processModel;
        reactionController->setProcessModel(
            &processModel);
        companion::BridgeTask failedTask;
        failedTask.id =
            QStringLiteral("chat-route-failure");
        failedTask.title =
            QStringLiteral("Background process");
        failedTask.preview =
            QStringLiteral("Build failed");
        failedTask.status =
            companion::TaskStatus::Failed;
        failedTask.updatedAt =
            currentBridgeDate();
        companion::CodexProcessSnapshot
            failedSnapshot;
        failedSnapshot.tasks = {failedTask};
        processModel.setSnapshot(failedSnapshot);
        QCOMPARE(
            petModel->renderedAnimation(),
            QStringLiteral("review"));
        const WId menuWindowId =
            menuWindow->winId();

        application.showModelPickerFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->property(
                "modelPickerOpen")
                .toBool(),
            1000);

        application.showProcessesFromPet();

        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("processes"));
        QTRY_COMPARE_WITH_TIMEOUT(
            petModel->renderedAnimation(),
            QStringLiteral("failed"),
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !menuWindow->property(
                "modelPickerOpen")
                 .toBool(),
            1000);
        QCOMPARE(
            menuWindow->winId(),
            menuWindowId);
    }

    void chatModelSelectionPersistsAcrossApplicationRestart()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini"));

        {
            companion::CompanionApplication firstRun(
                *QCoreApplication::instance(),
                uniqueInstanceName(),
                settingsPath);
            QVERIFY(firstRun.start().hasValue());
            auto* shell =
                firstRun.findChild<
                    companion::CompanionShellViewModel*>();
            auto* settings =
                firstRun.findChild<
                    companion::SettingsViewModel*>();
            QVERIFY(shell != nullptr);
            QVERIFY(settings != nullptr);

            shell->setSelectedChatModelId(
                QStringLiteral(
                    "openai:gpt56Terra"));
            settings->setAnimationSpeedScale(1.5);

            QCOMPARE(
                shell->selectedChatModelId(),
                QStringLiteral(
                    "openai:gpt56Terra"));
        }

        companion::CompanionApplication secondRun(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            settingsPath);
        QVERIFY(secondRun.start().hasValue());
        auto* restored =
            secondRun.findChild<
                companion::CompanionShellViewModel*>();
        QVERIFY(restored != nullptr);
        QCOMPARE(
            restored->selectedChatModelId(),
            QStringLiteral(
                "openai:gpt56Terra"));
    }

    void generalPetSettingsApplyToLivePetModelImmediately()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini"));

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            settingsPath);
        const auto started = application.start();
        const QByteArray startFailure =
            started.hasValue()
            ? QByteArray()
            : (started.error().code
               + QStringLiteral(": ")
               + started.error().message)
                  .toUtf8();
        QVERIFY2(started.hasValue(), startFailure.constData());
        auto* settings =
            application.findChild<
                companion::SettingsViewModel*>();
        auto* petModel =
            application.findChild<
                companion::PetViewModel*>();
        QVERIFY(settings != nullptr);
        QVERIFY(petModel != nullptr);

        QVERIFY(petModel->controlsVisible());
        QVERIFY(petModel->allowAutonomousMovement());

        settings->setAnimationSpeedScale(1.65);
        settings->setHideControlsUntilHover(true);
        settings->setAllowAutonomousMovement(false);

        QCOMPARE(petModel->animationSpeedScale(), 1.65);
        QVERIFY(petModel->hideControlsUntilHover());
        QVERIFY(!petModel->controlsVisible());
        QVERIFY(!petModel->allowAutonomousMovement());

        companion::SettingsRepository repository(settingsPath);
        const auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QCOMPARE(persisted.value().animationSpeedScale, 1.65);
        QVERIFY(persisted.value().hideControlsUntilHover);
        QVERIFY(!persisted.value().allowAutonomousMovement);
    }

    void mobileSettingsSignalKeepsApplicationSnapshotSynchronized()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini"));

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            settingsPath);
        QVERIFY(application.start().hasValue());
        auto* settings =
            application.findChild<
                companion::SettingsViewModel*>();
        QVERIFY(settings != nullptr);
        QVERIFY(
            !companion::detail::
                CompanionApplicationTestAccess::
                    loadedSettings(application)
                        .allowNearbyOnPublicNetworks);

        settings->
            setAllowNearbyOnPublicNetworks(
                true);

        QVERIFY(
            companion::detail::
                CompanionApplicationTestAccess::
                    loadedSettings(application)
                        .allowNearbyOnPublicNetworks);
        companion::SettingsRepository repository(
            settingsPath);
        const auto persisted =
            repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(
            persisted.value()
                .allowNearbyOnPublicNetworks);
    }

    void staleTransportReturnsActivationConnectFailed()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();
        ScopedMutex mutex(name);
        QVERIFY(mutex.isValid());

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));

        const auto started = application.start();

        QVERIFY(!started.hasValue());
        QCOMPARE(started.error().code,
                 QStringLiteral("app.activation-connect-failed"));
    }

    void coordinatorRuntimeErrorsReachApplicationBoundaryOnce()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));
        auto* coordinator =
            application.findChild<companion::WindowCoordinator*>();
        QVERIFY(coordinator != nullptr);

        QSignalSpy runtimeErrorSpy(
            &application,
            &companion::CompanionApplication::runtimeErrorOccurred);
        QVERIFY(runtimeErrorSpy.isValid());

        const auto missing = coordinator->show(companion::WindowRole::Pet);
        QVERIFY(!missing.hasValue());
        QCOMPARE(runtimeErrorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     runtimeErrorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.not-registered"));

        auto* settingsWindow = new QQuickWindow();
        QVERIFY(coordinator
                    ->registerWindow(companion::WindowRole::Settings,
                                     *settingsWindow)
                    .hasValue());
        delete settingsWindow;

        application.showSettings();

        QCOMPARE(runtimeErrorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     runtimeErrorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.destroyed"));
    }

    void userMaterialChangeUsesCoordinatorEffectiveModeImmediately()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")),
            std::move(coordinator));
        const auto started = application.start();
        const QString startFailure = started.hasValue()
            ? QString()
            : started.error().code
                + QStringLiteral(": ")
                + started.error().message;
        QVERIFY2(started.hasValue(), qPrintable(startFailure));
        auto* viewModel =
            application.findChild<companion::SettingsViewModel*>();
        QVERIFY(viewModel != nullptr);
        QCOMPARE(viewModel->effectiveBackdropMode(), QStringLiteral("mica"));
        QSignalSpy effectiveSpy(
            viewModel,
            &companion::SettingsViewModel::effectiveBackdropModeChanged);
        QVERIFY(effectiveSpy.isValid());

        viewModel->setBackdropMode(QStringLiteral("solid-black"));

        QCOMPARE(viewModel->backdropMode(), QStringLiteral("solid-black"));
        QCOMPARE(viewModel->effectiveBackdropMode(), QStringLiteral("solid-black"));
        QCOMPARE(effectiveSpy.count(), 1);
    }

    void roleSpecificBackdropFallbacksReachTheMatchingQmlSurfaces()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")),
            std::move(coordinator));
        const auto started =
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(application);
        const QString startFailure = started.hasValue()
            ? QString()
            : started.error().code
                + QStringLiteral(": ")
                + started.error().message;
        QVERIFY2(started.hasValue(), qPrintable(startFailure));

        auto* viewModel =
            application.findChild<companion::SettingsViewModel*>();
        auto* backdropState =
            application.findChild<companion::WindowBackdropState*>();
        auto* menuWindow =
            topLevelWindowNamed(QStringLiteral("companionMenuWindow"));
        auto* usageWindow =
            topLevelWindowNamed(QStringLiteral("usageWindow"));
        auto* attentionWindow =
            topLevelWindowNamed(QStringLiteral("attentionWindow"));
        QVERIFY(viewModel != nullptr);
        QVERIFY(backdropState != nullptr);
        QVERIFY(menuWindow != nullptr);
        QVERIFY(usageWindow != nullptr);
        QVERIFY(attentionWindow != nullptr);

        nativeApi.failingBackdropsByHwnd[
            reinterpret_cast<HWND>(usageWindow->winId())] = {
            companion::DwmSystemBackdropType::TransientWindow,
        };

        viewModel->setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(viewModel->backdropMode(), QStringLiteral("windows-glass"));
        QCOMPARE(
            backdropState->settingsEffectiveMode(),
            QStringLiteral("windows-glass"));
        QCOMPARE(
            backdropState->companionMenuEffectiveMode(),
            QStringLiteral("windows-glass"));
        QCOMPARE(
            backdropState->usageEffectiveMode(),
            QStringLiteral("solid-black"));
        QCOMPARE(
            backdropState->attentionEffectiveMode(),
            QStringLiteral("windows-glass"));
        QCOMPARE(
            menuWindow->property("effectiveBackdropMode").toString(),
            QStringLiteral("windows-glass"));
        QCOMPARE(
            usageWindow->property("effectiveBackdropMode").toString(),
            QStringLiteral("solid-black"));
        QCOMPARE(
            attentionWindow->property("effectiveBackdropMode").toString(),
            QStringLiteral("windows-glass"));
    }

    void modelPickerPopupIsRegisteredAsAnOwnedRoundedUtility()
    {
        QTest::failOnWarning(
            QRegularExpression(
                QStringLiteral(
                    "External WM_DESTROY received.*QQuickPopupWindow")));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(
                backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")),
            std::move(coordinator));
        const auto initialized =
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(application);
        QVERIFY(initialized.hasValue());

        auto* liveCoordinator =
            application.findChild<
                companion::WindowCoordinator*>();
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(liveCoordinator != nullptr);
        QVERIFY(menuWindow != nullptr);

        application.showModelPickerFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->property(
                "modelPickerOpen")
                .toBool(),
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->property(
                "modelPickerWindow")
                    .value<QObject*>()
                != nullptr,
            1000);
        auto* modelWindow =
            qobject_cast<QQuickWindow*>(
                menuWindow->property(
                    "modelPickerWindow")
                    .value<QObject*>());
        QVERIFY(modelWindow != nullptr);
        QVERIFY(modelWindow->handle() != nullptr);
        QCOMPARE(
            modelWindow->property(
                "nativeBackdropRegionEnabled")
                .toBool(),
            true);
        QCOMPARE(
            modelWindow->property(
                "nativeBackdropRegionRadius")
                .toDouble(),
            20.0);

        const auto modelStyle =
            companion::UtilityWindowPolicy::
                inspect(
                    reinterpret_cast<HWND>(
                        modelWindow->winId()));
        QVERIFY(modelStyle.hasValue());
        QVERIFY(modelStyle.value().isToolWindow);
        QVERIFY(!modelStyle.value().isAppWindow);
        QCOMPARE(
            modelStyle.value().owner,
            reinterpret_cast<HWND>(
                menuWindow->winId()));
        QVERIFY(nativeWindowHasRegion(
            reinterpret_cast<HWND>(
                modelWindow->winId())));
        QVERIFY(liveCoordinator
                    ->setOwner(
                        companion::WindowRole::ModelPicker,
                        companion::WindowRole::CompanionMenu)
                    .hasValue());
    }

    void goalPopupIsRegisteredAsAnOwnedRoundedUtility()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(
                backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")),
            std::move(coordinator));
        const auto initialized =
            companion::detail::
                CompanionApplicationTestAccess::
                    initializeQmlOnly(application);
        QVERIFY(initialized.hasValue());

        auto* liveCoordinator =
            application.findChild<
                companion::WindowCoordinator*>();
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(liveCoordinator != nullptr);
        QVERIFY(shell != nullptr);
        QVERIFY(menuWindow != nullptr);

        application.showProcessesFromPet();
        shell->openGoalControls(
            QStringLiteral(
                "Windows parity"),
            {
                {
                    QStringLiteral(
                        "threadId"),
                    QStringLiteral(
                        "goal-popup-test"),
                },
                {
                    QStringLiteral(
                        "objective"),
                    QStringLiteral(
                        "Verify the native goal popup."),
                },
                {
                    QStringLiteral(
                        "status"),
                    QStringLiteral(
                        "active"),
                },
                {
                    QStringLiteral(
                        "elapsedSeconds"),
                    30,
                },
            });

        QTRY_VERIFY_WITH_TIMEOUT(
            menuWindow->property(
                "goalWindow")
                    .value<QObject*>()
                != nullptr,
            1000);
        auto* goalWindow =
            qobject_cast<QQuickWindow*>(
                menuWindow->property(
                    "goalWindow")
                    .value<QObject*>());
        QVERIFY(goalWindow != nullptr);
        QVERIFY(goalWindow->handle() != nullptr);
        QCOMPARE(
            goalWindow->property(
                "nativeBackdropRegionEnabled")
                .toBool(),
            true);
        QCOMPARE(
            goalWindow->property(
                "nativeBackdropRegionRadius")
                .toDouble(),
            22.0);

        const auto goalStyle =
            companion::UtilityWindowPolicy::
                inspect(
                    reinterpret_cast<HWND>(
                        goalWindow->winId()));
        QVERIFY(goalStyle.hasValue());
        QVERIFY(goalStyle.value().isToolWindow);
        QVERIFY(!goalStyle.value().isAppWindow);
        QCOMPARE(
            goalStyle.value().owner,
            reinterpret_cast<HWND>(
                menuWindow->winId()));
        QVERIFY(nativeWindowHasRegion(
            reinterpret_cast<HWND>(
                goalWindow->winId())));
        QVERIFY(liveCoordinator
                    ->setOwner(
                        companion::WindowRole::Goal,
                        companion::WindowRole::CompanionMenu)
                    .hasValue());

        shell->dismissGoalControls();
        QTRY_VERIFY_WITH_TIMEOUT(
            !shell->goalControlVisible(),
            1000);
    }

    void failedUserMaterialChangeReportsOneApplicationRuntimeError()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")),
            std::move(coordinator));
        QVERIFY(application.start().hasValue());
        auto* viewModel =
            application.findChild<companion::SettingsViewModel*>();
        QVERIFY(viewModel != nullptr);
        auto* settingsWindow =
            topLevelWindowNamed(QStringLiteral("settingsWindow"));
        QVERIFY(settingsWindow != nullptr);
        const auto settingsHwnd =
            reinterpret_cast<HWND>(settingsWindow->winId());
        QSignalSpy runtimeErrorSpy(
            &application,
            &companion::CompanionApplication::runtimeErrorOccurred);
        QVERIFY(runtimeErrorSpy.isValid());

        nativeApi.failingBackdropsByHwnd[settingsHwnd] = {
            companion::DwmSystemBackdropType::TransientWindow,
            companion::DwmSystemBackdropType::None,
        };
        viewModel->setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(viewModel->backdropMode(), QStringLiteral("mica"));
        QCOMPARE(runtimeErrorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     runtimeErrorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.backdrop-reapply-failed"));
    }

    void failedUserMaterialChangeWithFailedRollbackReportsCompositeAndKeepsEffectiveState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();
        const QString settingsPath =
            temporarySettingsPath(directory, QStringLiteral("settings.ini"));
        AppTestNativeWindowApi nativeApi;
        companion::BackdropController backdropController(nativeApi);
        auto coordinator =
            std::make_unique<companion::WindowCoordinator>(backdropController);

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            settingsPath,
            std::move(coordinator));
        QVERIFY(application.start().hasValue());
        auto* viewModel =
            application.findChild<companion::SettingsViewModel*>();
        auto* liveCoordinator =
            application.findChild<companion::WindowCoordinator*>();
        QVERIFY(viewModel != nullptr);
        QVERIFY(liveCoordinator != nullptr);
        auto* settingsWindow =
            topLevelWindowNamed(QStringLiteral("settingsWindow"));
        auto* menuWindow =
            topLevelWindowNamed(QStringLiteral("companionMenuWindow"));
        QVERIFY(settingsWindow != nullptr);
        QVERIFY(menuWindow != nullptr);
        const auto settingsHwnd =
            reinterpret_cast<HWND>(settingsWindow->winId());
        const auto menuHwnd =
            reinterpret_cast<HWND>(menuWindow->winId());
        QVERIFY(nativeApi.currentBackdrops.contains(settingsHwnd));
        QVERIFY(nativeApi.currentBackdrops.contains(menuHwnd));
        QVERIFY(liveCoordinator
                    ->show(companion::WindowRole::CompanionMenu)
                    .hasValue());

        QSignalSpy applicationErrorSpy(
            &application,
            &companion::CompanionApplication::runtimeErrorOccurred);
        QSignalSpy coordinatorErrorSpy(
            liveCoordinator,
            &companion::WindowCoordinator::runtimeErrorOccurred);
        QVERIFY(applicationErrorSpy.isValid());
        QVERIFY(coordinatorErrorSpy.isValid());

        nativeApi.failingBackdropsByHwnd[menuHwnd] = {
            companion::DwmSystemBackdropType::TransientWindow,
            companion::DwmSystemBackdropType::None,
        };
        nativeApi.failingBackdropsByHwnd[settingsHwnd] = {
            companion::DwmSystemBackdropType::MainWindow,
            companion::DwmSystemBackdropType::None,
        };

        viewModel->setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(applicationErrorSpy.count(), 1);
        QCOMPARE(coordinatorErrorSpy.count(), 0);
        const auto error = qvariant_cast<companion::CompanionError>(
            applicationErrorSpy.takeFirst().at(0));
        QCOMPARE(error.code,
                 QStringLiteral("window.backdrop-rollback-failed"));
        QVERIFY(error.context.contains(QStringLiteral("reapplyFailure")));
        QVERIFY(error.context.contains(QStringLiteral("rollbackFailure")));
        QCOMPARE(viewModel->backdropMode(), QStringLiteral("mica"));
        QCOMPARE(viewModel->effectiveBackdropMode(),
                 QStringLiteral("windows-glass"));
        QCOMPARE(
            liveCoordinator->effectiveBackdropMode(
                companion::WindowRole::Settings).value(),
            companion::BackdropMode::WindowsGlass);
        QCOMPARE(
            liveCoordinator->effectiveBackdropMode(
                companion::WindowRole::CompanionMenu).value(),
            companion::BackdropMode::Mica);
        QCOMPARE(nativeApi.currentBackdrops.value(settingsHwnd),
                 companion::DwmSystemBackdropType::TransientWindow);
        QCOMPARE(nativeApi.currentBackdrops.value(menuHwnd),
                 companion::DwmSystemBackdropType::MainWindow);

        companion::SettingsRepository repository(settingsPath);
        const auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QCOMPARE(persisted.value().backdrop, companion::BackdropMode::Mica);
    }

    void coordinatorProvidesTrayRouteState()
    {
        companion::WindowCoordinator coordinator;
        QQuickWindow petWindow;
        QQuickWindow menuWindow;
        QQuickWindow settingsWindow;

        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Pet, petWindow)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::CompanionMenu,
                                    menuWindow)
                    .hasValue());
        QVERIFY(coordinator
                    .registerWindow(companion::WindowRole::Settings,
                                    settingsWindow)
                    .hasValue());

        petWindow.show();
        settingsWindow.show();
        menuWindow.hide();

        const auto state = coordinator.trayRouteState();

        QVERIFY(state.petRegistered);
        QVERIFY(state.petVisible);
        QVERIFY(state.companionMenuRegistered);
        QVERIFY(!state.companionMenuVisible);
        QVERIFY(state.processesRegistered);
        QVERIFY(state.chatRegistered);
        QVERIFY(state.settingsVisible);
    }

    void closeSettingsHidesReusableWindowSurface()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));
        auto* coordinator =
            application.findChild<companion::WindowCoordinator*>();
        QVERIFY(coordinator != nullptr);
        QQuickWindow settingsWindow;
        settingsWindow.create();
        QVERIFY(settingsWindow.handle() != nullptr);
        QVERIFY(coordinator
                    ->registerWindow(companion::WindowRole::Settings,
                                     settingsWindow)
                    .hasValue());

        application.showSettings();
        QVERIFY(settingsWindow.isVisible());
        QVERIFY(settingsWindow.handle() != nullptr);

        application.closeSettings();

        QVERIFY(!settingsWindow.isVisible());
        QVERIFY(settingsWindow.handle() != nullptr);

        application.showSettings();

        QVERIFY(settingsWindow.isVisible());
        QVERIFY(settingsWindow.handle() != nullptr);
    }

    void trayCommandsToggleRegisteredPetAndMenuWindows()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* coordinator =
            application.findChild<companion::WindowCoordinator*>();
        auto* tray = application.findChild<companion::TrayIconHost*>();
        auto* petModel =
            application.findChild<companion::PetViewModel*>();
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* petWindow =
            topLevelWindowNamed(
                QStringLiteral("petWindow"));
        auto* menuWindow =
            topLevelWindowNamed(
                QStringLiteral("companionMenuWindow"));
        QVERIFY(coordinator != nullptr);
        QVERIFY(tray != nullptr);
        QVERIFY(petModel != nullptr);
        QVERIFY(shell != nullptr);
        QVERIFY(petWindow != nullptr);
        QVERIFY(menuWindow != nullptr);
        QVERIFY(petWindow->isVisible());
        QVERIFY(!menuWindow->isVisible());

        tray->invokeCommand(companion::TrayIconHost::Command::TogglePet);

        QTRY_VERIFY_WITH_TIMEOUT(!petWindow->isVisible(), 1000);
        QVERIFY(!petModel->visible());

        tray->invokeCommand(companion::TrayIconHost::Command::TogglePet);

        QTRY_VERIFY_WITH_TIMEOUT(petWindow->isVisible(), 1000);
        QVERIFY(petModel->visible());

        shell->showLocalChat();
        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("local-chat"));
        tray->invokeCommand(companion::TrayIconHost::Command::ToggleCompanionMenu);

        QTRY_VERIFY_WITH_TIMEOUT(menuWindow->isVisible(), 1000);
        QVERIFY(petModel->menuOpen());
        QCOMPARE(
            shell->routeMode(),
            QStringLiteral("processes"));

        tray->invokeCommand(companion::TrayIconHost::Command::ToggleCompanionMenu);

        QTRY_VERIFY_WITH_TIMEOUT(!menuWindow->isVisible(), 1000);
        QVERIFY(!petModel->menuOpen());
    }

    void trayCommandsReuseCompanionWindowForProcessesAndLocalChat()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* tray = application.findChild<companion::TrayIconHost*>();
        auto* companionWindow =
            topLevelWindowNamed(QStringLiteral("companionMenuWindow"));
        QVERIFY(tray != nullptr);
        QVERIFY(companionWindow != nullptr);
        QVERIFY(!companionWindow->isVisible());

        tray->invokeCommand(companion::TrayIconHost::Command::ShowProcesses);

        QTRY_VERIFY_WITH_TIMEOUT(companionWindow->isVisible(), 1000);
        QCOMPARE(companionWindow->property("routeMode").toString(),
                 QStringLiteral("processes"));
        const WId companionWindowId = companionWindow->winId();

        tray->invokeCommand(companion::TrayIconHost::Command::ShowChat);

        QTRY_VERIFY_WITH_TIMEOUT(companionWindow->isVisible(), 1000);
        QCOMPARE(companionWindow->winId(), companionWindowId);
        QCOMPARE(companionWindow->property("routeMode").toString(),
                 QStringLiteral("local-chat"));
    }

    void companionMenuCloseHidesReusableUtilityWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));
        QVERIFY(application.start().hasValue());
        auto* tray = application.findChild<companion::TrayIconHost*>();
        auto* shell =
            application.findChild<
                companion::CompanionShellViewModel*>();
        auto* companionWindow =
            topLevelWindowNamed(QStringLiteral("companionMenuWindow"));
        QVERIFY(tray != nullptr);
        QVERIFY(shell != nullptr);
        QVERIFY(companionWindow != nullptr);

        tray->invokeCommand(companion::TrayIconHost::Command::ShowProcesses);
        QTRY_VERIFY_WITH_TIMEOUT(companionWindow->isVisible(), 1000);
        shell->beginProcessAction(
            {
                {
                    QStringLiteral("id"),
                    QStringLiteral("thread-close"),
                },
                {
                    QStringLiteral("threadId"),
                    QStringLiteral("thread-close"),
                },
                {
                    QStringLiteral("title"),
                    QStringLiteral("Close parity"),
                },
                {
                    QStringLiteral("status"),
                    QStringLiteral("running"),
                },
                {
                    QStringLiteral("needsApproval"),
                    false,
                },
            },
            QStringLiteral("reply"));
        shell->setProcessDraft(
            QStringLiteral("Preserve this draft"));
        QVERIFY(shell->processTargetActive());

        const auto utilityStyle = companion::UtilityWindowPolicy::inspect(
            reinterpret_cast<HWND>(companionWindow->winId()));
        QVERIFY(utilityStyle.hasValue());
        QVERIFY(utilityStyle.value().isToolWindow);
        QVERIFY(!utilityStyle.value().isAppWindow);

        QVERIFY(QMetaObject::invokeMethod(
            companionWindow,
            "closeRequested",
            Qt::DirectConnection));

        QTRY_VERIFY_WITH_TIMEOUT(!companionWindow->isVisible(), 1000);
        QVERIFY(companionWindow->handle() != nullptr);
        QVERIFY(!application.explicitQuitRequested());
        QVERIFY(shell->processTargetActive());
        QCOMPARE(
            shell->processDraft(),
            QStringLiteral("Preserve this draft"));

        tray->invokeCommand(companion::TrayIconHost::Command::ShowChat);

        QTRY_VERIFY_WITH_TIMEOUT(companionWindow->isVisible(), 1000);
        QCOMPARE(companionWindow->property("routeMode").toString(),
                 QStringLiteral("local-chat"));
    }

    void usageButtonTogglesReusableTrayOnlyWindow()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            uniqueInstanceName(),
            temporarySettingsPath(
                directory,
                QStringLiteral("settings.ini")));

        QVERIFY(application.start().hasValue());
        auto* usageWindow =
            topLevelWindowNamed(
                QStringLiteral("usageWindow"));
        auto* companionWindow =
            topLevelWindowNamed(
                QStringLiteral(
                    "companionMenuWindow"));
        QVERIFY(usageWindow != nullptr);
        QVERIFY(companionWindow != nullptr);
        QVERIFY(!usageWindow->isVisible());

        application.showUsageFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            usageWindow->isVisible(),
            1000);
        QVERIFY(companionWindow->isVisible());
        QVERIFY(!QRect(
            usageWindow->position(),
            usageWindow->size())
                     .intersects(QRect(
                         companionWindow->position(),
                         companionWindow->size())));
        const WId usageWindowId =
            usageWindow->winId();
        const auto utilityStyle =
            companion::UtilityWindowPolicy::inspect(
                reinterpret_cast<HWND>(
                    usageWindowId));
        QVERIFY(utilityStyle.hasValue());
        QVERIFY(utilityStyle.value().isToolWindow);
        QVERIFY(!utilityStyle.value().isAppWindow);

        QVERIFY(QMetaObject::invokeMethod(
            usageWindow,
            "closeRequested",
            Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(
            !usageWindow->isVisible(),
            1000);
        QVERIFY(companionWindow->isVisible());
        QVERIFY(usageWindow->handle() != nullptr);
        QVERIFY(!application.explicitQuitRequested());

        application.showUsageFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            usageWindow->isVisible(),
            1000);
        QCOMPARE(
            usageWindow->winId(),
            usageWindowId);

        application.showUsageFromPet();

        QTRY_VERIFY_WITH_TIMEOUT(
            !usageWindow->isVisible(),
            1000);
        QVERIFY(companionWindow->isVisible());
        QVERIFY(!application.explicitQuitRequested());

        application.showUsageFromPet();
        QTRY_VERIFY_WITH_TIMEOUT(
            usageWindow->isVisible(),
            1000);
        QVERIFY(QMetaObject::invokeMethod(
            companionWindow,
            "closeRequested",
            Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(
            !companionWindow->isVisible(),
            1000);
        QTRY_VERIFY_WITH_TIMEOUT(
            !usageWindow->isVisible(),
            1000);
    }

    void quitExplicitlyReleasesOwnedServices()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = uniqueInstanceName();

        companion::CompanionApplication application(
            *QCoreApplication::instance(),
            name,
            temporarySettingsPath(directory, QStringLiteral("settings.ini")));

        QVERIFY(application.start().hasValue());
        QVERIFY(!application.explicitQuitRequested());

        application.quitExplicitly();

        QVERIFY(application.explicitQuitRequested());

        companion::SingleInstanceGate replacement(name);
        QVERIFY(replacement.startPrimary().hasValue());
    }

    void postUpdateStartupSignalsAcknowledgementWithoutOpeningSettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString requestId =
            QUuid::createUuid()
                .toString(
                    QUuid::WithoutBraces);
        const QString eventName =
            companion::UpdateInstallRequest::
                acknowledgementEventFor(
                    requestId);
        const std::wstring nativeName =
            eventName.toStdWString();
        HANDLE event =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                nativeName.c_str());
        QVERIFY(event != nullptr);

        {
            companion::CompanionApplication application(
                *QCoreApplication::instance(),
                uniqueInstanceName(),
                temporarySettingsPath(
                    directory,
                    QStringLiteral(
                        "settings.ini")),
                std::make_unique<
                    companion::
                        WindowCoordinator>(),
                requestId);

            const auto started =
                application.start();
            QVERIFY2(
                started.hasValue(),
                qPrintable(
                    started.hasValue()
                        ? QString()
                        : started.error()
                              .code
                          + QStringLiteral(
                              ": ")
                          + started.error()
                                .message));
            QCOMPARE(
                WaitForSingleObject(
                    event,
                    1'000),
                DWORD(WAIT_OBJECT_0));
            auto* settings =
                topLevelWindowNamed(
                    QStringLiteral(
                        "settingsWindow"));
            QVERIFY(settings != nullptr);
            QVERIFY(!settings->isVisible());
        }

        CloseHandle(event);
    }
};

int main(int argc, char* argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DaSilverFire"));
    QCoreApplication::setApplicationName(QStringLiteral("Codex Companion"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.3.4"));
    application.setQuitOnLastWindowClosed(false);

    CompanionApplicationTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "CompanionApplicationTests.moc"
