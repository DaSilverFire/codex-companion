#include "app/CompanionApplication.h"
#include "app/CompanionRuntimeHost.h"
#include "app/PetProcessReactionController.h"

#include "codex/accounts/CodexAccountLoginService.h"
#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/discovery/CodexInstallationDiscovery.h"
#include "platform/windows/DpapiCredentialStore.h"
#include "platform/windows/ChatGPTAccentThemeReader.h"
#include "platform/windows/SingleInstanceGate.h"
#include "platform/windows/TrayIconHost.h"
#include "platform/windows/TrayWindowPlacement.h"
#include "platform/windows/mobile/WindowsPowerAvailabilityController.h"
#include "mobile/presence/MobilePresencePetCatalogService.h"
#include "ui/CompanionShellViewModel.h"
#include "ui/CompanionPresentationPolicy.h"
#include "ui/PetViewModel.h"
#include "ui/PetWindowController.h"
#include "ui/SettingsViewModel.h"
#include "ui/WindowBackdropState.h"
#include "ui/pet/PetCatalog.h"
#include "ui/pet/PetMenuPlacement.h"
#include "app/UpdateViewModel.h"
#include "update/UpdateBuildConfiguration.h"
#include "update/UpdateService.h"
#include "updater-helper/UpdateInstallerHandoff.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMetaType>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantAnimation>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

const QString kPrimaryInstanceName = QStringLiteral("CodexCompanion");
constexpr int kCompanionIconResourceId = 101;
constexpr int kMenuMotionDurationMilliseconds = 620;
constexpr int kMenuMotionPrimingMilliseconds = 32;
constexpr int kCompanionUtilityHoverPollMilliseconds = 40;
constexpr double kMenuMotionResponseSeconds = 0.42;
constexpr double kMenuMotionDampingFraction = 0.88;

double menuMotionProgress(double normalizedProgress)
{
    const double elapsedSeconds =
        std::clamp(normalizedProgress, 0.0, 1.0)
        * kMenuMotionDurationMilliseconds
        / 1000.0;
    const double angularFrequency =
        2.0 * std::numbers::pi
        / kMenuMotionResponseSeconds;
    const double dampedScale = std::sqrt(
        1.0
        - kMenuMotionDampingFraction
            * kMenuMotionDampingFraction);
    const double dampedFrequency =
        angularFrequency * dampedScale;
    const double decay = std::exp(
        -kMenuMotionDampingFraction
        * angularFrequency
        * elapsedSeconds);
    const double displacement =
        decay
        * (std::cos(
               dampedFrequency
               * elapsedSeconds)
           + kMenuMotionDampingFraction
                 / dampedScale
                 * std::sin(
                     dampedFrequency
                     * elapsedSeconds));
    return 1.0 - displacement;
}

QPoint interpolatedOrigin(
    QPoint start,
    QPoint target,
    double progress)
{
    return {
        qRound(
            start.x()
            + (target.x() - start.x())
                * progress),
        qRound(
            start.y()
            + (target.y() - start.y())
                * progress),
    };
}

bool nativeWindowContainsCursor(
    const QQuickWindow& window,
    POINT cursor)
{
    const auto hwnd =
        reinterpret_cast<HWND>(
            window.winId());
    RECT frame {};
    return hwnd != nullptr
        && IsWindowVisible(hwnd) != FALSE
        && GetWindowRect(hwnd, &frame) != FALSE
        && PtInRect(&frame, cursor) != FALSE;
}

companion::CompanionError trayStartupError(QString code, QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

companion::CompanionError qmlStartupError(QString code, QString message)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {},
    };
}

companion::CompanionError
postUpdateError(
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

companion::Result<
    std::optional<QString>>
postUpdateRequestIdFromArguments(
    const QStringList& arguments)
{
    std::optional<QString> requestId;
    for (qsizetype index = 1;
         index < arguments.size();
         ++index) {
        if (arguments.at(index)
            != QStringLiteral(
                "--post-update-ack")) {
            continue;
        }
        if (requestId.has_value()
            || index + 1
                >= arguments.size()) {
            return companion::Result<
                std::optional<QString>>::
                failure(
                    postUpdateError(
                        QStringLiteral(
                            "update.ack_argument_invalid"),
                        QStringLiteral(
                            "The post-update acknowledgement argument is invalid.")));
        }
        requestId =
            arguments.at(++index);
    }
    if (!requestId.has_value()) {
        return companion::Result<
            std::optional<QString>>::
            success(std::nullopt);
    }

    const QUuid parsed(*requestId);
    const QString canonical =
        parsed.toString(
            QUuid::WithoutBraces);
    if (parsed.isNull()
        || *requestId != canonical) {
        return companion::Result<
            std::optional<QString>>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_argument_invalid"),
                    QStringLiteral(
                        "The post-update acknowledgement identifier is invalid.")));
    }
    return companion::Result<
        std::optional<QString>>::
        success(requestId);
}

std::optional<std::array<quint16, 3>>
compiledCoreVersion()
{
    const QStringList parts =
        QStringLiteral(
            COMPANION_WINDOWS_VERSION)
            .split(
                QLatin1Char('.'),
                Qt::KeepEmptyParts);
    if (parts.size() != 3) {
        return std::nullopt;
    }
    std::array<quint16, 3> version{};
    for (qsizetype index = 0;
         index < parts.size();
         ++index) {
        bool valid = false;
        const uint value =
            parts.at(index)
                .toUInt(&valid, 10);
        if (!valid
            || value
                > std::numeric_limits<
                    quint16>::max()) {
            return std::nullopt;
        }
        version.at(
            static_cast<size_t>(
                index)) =
            static_cast<quint16>(
                value);
    }
    return version;
}

companion::Result<void>
verifyCurrentExecutableBuild()
{
    const auto expectedCore =
        compiledCoreVersion();
    if (!expectedCore.has_value()
        || COMPANION_WINDOWS_BUILD <= 0
        || COMPANION_WINDOWS_BUILD
            > std::numeric_limits<
                quint16>::max()) {
        return companion::Result<void>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_build_invalid"),
                    QStringLiteral(
                        "The running Codex Companion build identity is invalid.")));
    }

    const QString executable =
        QCoreApplication::
            applicationFilePath();
    const std::wstring native =
        executable.toStdWString();
    DWORD ignored = 0;
    const DWORD bytes =
        GetFileVersionInfoSizeW(
            native.c_str(),
            &ignored);
    if (bytes == 0) {
        return companion::Result<void>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_version_unavailable"),
                    QStringLiteral(
                        "The running Codex Companion version could not be read."),
                    {
                        {
                            QStringLiteral(
                                "win32Error"),
                            QVariant::fromValue<
                                qulonglong>(
                                GetLastError()),
                        },
                    }));
    }
    std::vector<BYTE> buffer(bytes);
    if (!GetFileVersionInfoW(
            native.c_str(),
            0,
            bytes,
            buffer.data())) {
        return companion::Result<void>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_version_unavailable"),
                    QStringLiteral(
                        "The running Codex Companion version could not be read.")));
    }

    void* rawInfo = nullptr;
    UINT infoBytes = 0;
    if (!VerQueryValueW(
            buffer.data(),
            L"\\",
            &rawInfo,
            &infoBytes)
        || rawInfo == nullptr
        || infoBytes
            < sizeof(VS_FIXEDFILEINFO)) {
        return companion::Result<void>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_version_unavailable"),
                    QStringLiteral(
                        "The running Codex Companion version metadata is incomplete.")));
    }

    const auto* fixed =
        static_cast<
            const VS_FIXEDFILEINFO*>(
            rawInfo);
    const std::array<quint16, 4>
        actual{
            static_cast<quint16>(
                HIWORD(
                    fixed
                        ->dwFileVersionMS)),
            static_cast<quint16>(
                LOWORD(
                    fixed
                        ->dwFileVersionMS)),
            static_cast<quint16>(
                HIWORD(
                    fixed
                        ->dwFileVersionLS)),
            static_cast<quint16>(
                LOWORD(
                    fixed
                        ->dwFileVersionLS)),
        };
    const std::array<quint16, 4>
        expected{
            expectedCore->at(0),
            expectedCore->at(1),
            expectedCore->at(2),
            static_cast<quint16>(
                COMPANION_WINDOWS_BUILD),
        };
    if (actual != expected) {
        return companion::Result<void>::
            failure(
                postUpdateError(
                    QStringLiteral(
                        "update.ack_version_mismatch"),
                    QStringLiteral(
                        "The running Codex Companion build does not match its compiled update identity.")));
    }
    return companion::Result<void>::
        success();
}

} // namespace

namespace companion {

CompanionApplication::CompanionApplication(QCoreApplication& application)
    : QObject(&application),
      application_(application),
      instanceName_(kPrimaryInstanceName),
      settingsRepository_(QString()),
      qmlEngine_(std::make_unique<QQmlApplicationEngine>()),
      windowCoordinator_(std::make_unique<WindowCoordinator>(this)),
      productionRuntimeEnabled_(true)
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    connectWindowCoordinator();

    const auto settingsPath = defaultSettingsFilePath();
    if (!settingsPath.hasValue()) {
        startupError_ = settingsPath.error();
        return;
    }

    settingsRepository_ = SettingsRepository(settingsPath.value());
    const auto postUpdateRequest =
        postUpdateRequestIdFromArguments(
            application.arguments());
    if (!postUpdateRequest.hasValue()) {
        startupError_ =
            postUpdateRequest.error();
        return;
    }
    if (postUpdateRequest.value()
            .has_value()) {
        postUpdateAcknowledgementRequestId_ =
            *postUpdateRequest.value();
    }
    const auto updateFeedOverride =
        updateManifestUrlOverrideFromArguments(
            application.arguments());
    if (!updateFeedOverride.hasValue()) {
        startupError_ =
            updateFeedOverride.error();
        return;
    }
    updateManifestUrlOverride_ =
        updateFeedOverride.value();
}

CompanionApplication::CompanionApplication(
    QCoreApplication& application,
    QString instanceName,
    QString settingsFilePath)
    : QObject(&application),
      application_(application),
      instanceName_(std::move(instanceName)),
      settingsRepository_(std::move(settingsFilePath)),
      qmlEngine_(std::make_unique<QQmlApplicationEngine>()),
      windowCoordinator_(std::make_unique<WindowCoordinator>(this))
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    connectWindowCoordinator();
}

CompanionApplication::CompanionApplication(
    QCoreApplication& application,
    QString instanceName,
    QString settingsFilePath,
    std::unique_ptr<WindowCoordinator> windowCoordinator,
    QString postUpdateAcknowledgementRequestId)
    : QObject(&application),
      application_(application),
      instanceName_(std::move(instanceName)),
      settingsRepository_(std::move(settingsFilePath)),
      qmlEngine_(std::make_unique<QQmlApplicationEngine>()),
      windowCoordinator_(std::move(windowCoordinator)),
      postUpdateAcknowledgementRequestId_(
          std::move(
              postUpdateAcknowledgementRequestId))
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    if (windowCoordinator_) {
        windowCoordinator_->setParent(this);
    }
    connectWindowCoordinator();
}

CompanionApplication::~CompanionApplication()
{
    stopCompanionMenuOriginAnimation();
    if (trayIconHost_) {
        trayIconHost_->hide();
        trayIconHost_.reset();
    }
    runtimeHost_.reset();
    chatAccentRefreshTimer_.reset();
    powerAvailabilityController_.reset();
    petProcessReactionController_.reset();
    petWindowController_.reset();
    destroyCompanionPopupWindows();
    qmlEngine_.reset();
    windowBackdropState_.reset();
    updateViewModel_.reset();
    updateService_.reset();
    settingsViewModel_.reset();
    codexAccountLoginService_.reset();
    codexAccountRouter_.reset();
    codexAccountRuntime_.reset();
    codexThreadAccountBindingStore_.reset();
    codexAccountProfileStore_.reset();
    petViewModel_.reset();
    petCatalog_.reset();
    shellViewModel_.reset();
    windowCoordinator_.reset();
    singleInstanceGate_.reset();
}

void CompanionApplication::configureStandardPathsForLaunch()
{
    bool parsed = false;
    const int enabled =
        qEnvironmentVariableIntValue(
            "CODEX_COMPANION_TEST_STANDARD_PATHS",
            &parsed);
    if (parsed && enabled == 1) {
        QStandardPaths::setTestModeEnabled(true);
    }
}

Result<QString> CompanionApplication::defaultSettingsFilePath()
{
    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDirectory.isEmpty()) {
        return Result<QString>::failure({
            QStringLiteral("settings.path-unavailable"),
            QStringLiteral("The application configuration location is unavailable."),
            false,
            {},
        });
    }

    QDir directory(configDirectory);
    directory.mkpath(QStringLiteral("."));
    return Result<QString>::success(
        directory.filePath(QStringLiteral("CodexCompanion.ini")));
}

Result<void> CompanionApplication::start()
{
    if (startupError_.has_value()) {
        return Result<void>::failure(*startupError_);
    }

    if (!singleInstanceGate_) {
        singleInstanceGate_ =
            std::make_unique<SingleInstanceGate>(instanceName_, this);
    }

    const auto primaryStarted = singleInstanceGate_->startPrimary();
    if (!primaryStarted.hasValue()) {
        if (primaryStarted.error().code == QStringLiteral("app.already-running")) {
            const auto activated = singleInstanceGate_->sendActivation();
            if (!activated.hasValue()) {
                return Result<void>::failure(activated.error());
            }
        }
        return primaryStarted;
    }

    const auto loaded = settingsRepository_.load();
    if (!loaded.hasValue()) {
        return Result<void>::failure(loaded.error());
    }
    loadedSettings_ = loaded.value();
    if (productionRuntimeEnabled_) {
        powerAvailabilityController_ =
            std::make_unique<
                WindowsPowerAvailabilityController>();
        const auto availabilityApplied =
            powerAvailabilityController_
                ->setAvailable(
                    loadedSettings_
                        .mobileEnabled
                    && loadedSettings_
                           .keepAvailableWhileDisplayOff);
        if (!availabilityApplied
                 .hasValue()) {
            reportRuntimeError(
                availabilityApplied.error());
        }
    }

    const auto qmlInitialized = initializeQmlEngine();
    if (!qmlInitialized.hasValue()) {
        return qmlInitialized;
    }
    const auto trayStarted = initializeTrayHost();
    if (!trayStarted.hasValue()) {
        return trayStarted;
    }
    if (productionRuntimeEnabled_) {
        const auto runtimeStarted =
            initializeRuntimeServices();
        if (!runtimeStarted.hasValue()) {
            reportRuntimeError(runtimeStarted.error());
        }
    }

    connect(singleInstanceGate_.get(),
            &SingleInstanceGate::activationRequested,
            this,
            &CompanionApplication::showSettings,
            Qt::UniqueConnection);
    refreshTrayRouteState();
    const bool postUpdateStartup =
        !postUpdateAcknowledgementRequestId_
             .isEmpty();
    const auto acknowledged =
        signalPostUpdateAcknowledgementIfRequested();
    if (!acknowledged.hasValue()) {
        return acknowledged;
    }
    if (!postUpdateStartup) {
        showSettings();
        const auto startupRouteApplied =
            applyIsolatedTestStartupRoute();
        if (!startupRouteApplied.hasValue()) {
            return startupRouteApplied;
        }
    }
    return Result<void>::success();
}

Result<void>
CompanionApplication::
applyIsolatedTestStartupRoute()
{
    bool parsed = false;
    const int isolated =
        qEnvironmentVariableIntValue(
            "CODEX_COMPANION_TEST_STANDARD_PATHS",
            &parsed);
    if (!parsed || isolated != 1) {
        return Result<void>::success();
    }

    const QString route =
        qEnvironmentVariable(
            "CODEX_COMPANION_TEST_STARTUP_ROUTE")
            .trimmed();
    if (route.isEmpty()
        || route == QStringLiteral("none")) {
        return Result<void>::success();
    }
    if (route == QStringLiteral("processes")) {
        showProcessesFromPet();
        return Result<void>::success();
    }
    if (route == QStringLiteral("local-chat")) {
        showChatFromPet();
        return Result<void>::success();
    }
    return Result<void>::failure(
        qmlStartupError(
            QStringLiteral(
                "app.test_startup_route_invalid"),
            QStringLiteral(
                "The isolated Companion startup route is invalid.")));
}

Result<void>
CompanionApplication::
signalPostUpdateAcknowledgementIfRequested()
{
    if (postUpdateAcknowledgementRequestId_
            .isEmpty()) {
        return Result<void>::success();
    }
    const QUuid parsed(
        postUpdateAcknowledgementRequestId_);
    const QString canonical =
        parsed.toString(
            QUuid::WithoutBraces);
    if (parsed.isNull()
        || canonical
            != postUpdateAcknowledgementRequestId_) {
        return Result<void>::failure(
            postUpdateError(
                QStringLiteral(
                    "update.ack_argument_invalid"),
                QStringLiteral(
                    "The post-update acknowledgement identifier is invalid.")));
    }

    const auto versionReady =
        verifyCurrentExecutableBuild();
    if (!versionReady.hasValue()) {
        return versionReady;
    }

    const QString eventName =
        UpdateInstallRequest::
            acknowledgementEventFor(
                postUpdateAcknowledgementRequestId_);
    const std::wstring nativeName =
        eventName.toStdWString();
    HANDLE event =
        OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            nativeName.c_str());
    if (event == nullptr) {
        return Result<void>::failure(
            postUpdateError(
                QStringLiteral(
                    "update.ack_event_open_failed"),
                QStringLiteral(
                    "The post-update acknowledgement event could not be opened."),
                {
                    {
                        QStringLiteral(
                            "win32Error"),
                        QVariant::fromValue<
                            qulonglong>(
                            GetLastError()),
                    },
                }));
    }
    const BOOL signaled =
        SetEvent(event);
    const DWORD signalError =
        signaled
        ? ERROR_SUCCESS
        : GetLastError();
    CloseHandle(event);
    if (!signaled) {
        return Result<void>::failure(
            postUpdateError(
                QStringLiteral(
                    "update.ack_event_signal_failed"),
                QStringLiteral(
                    "Codex Companion could not confirm its updated startup."),
                {
                    {
                        QStringLiteral(
                            "win32Error"),
                        QVariant::fromValue<
                            qulonglong>(
                            signalError),
                    },
                }));
    }
    postUpdateAcknowledgementRequestId_
        .clear();
    return Result<void>::success();
}

void CompanionApplication::showSettings()
{
    emit settingsShowRequested();
}

void CompanionApplication::closeSettings()
{
    emit settingsCloseRequested();
}

void CompanionApplication::closeCompanionMenu()
{
    stopCompanionMenuOriginAnimation();
    closeUsage();
    if (petViewModel_
        && petViewModel_->menuOpen()) {
        petViewModel_->setMenuOpen(false);
        return;
    }
    windowCoordinator_->hide(WindowRole::CompanionMenu);
    refreshTrayRouteState();
}

void CompanionApplication::closeUsage()
{
    if (usageWindow_.isNull()) {
        return;
    }
    windowCoordinator_->hide(WindowRole::Usage);
}

void CompanionApplication::showProcessesFromPet()
{
    if (!shellViewModel_ || !petViewModel_) {
        return;
    }
    closeUsage();
    shellViewModel_->showProcesses();
    petViewModel_->setMenuOpen(true);
}

void CompanionApplication::showChatFromPet()
{
    if (!shellViewModel_ || !petViewModel_) {
        return;
    }
    closeUsage();
    shellViewModel_->showLocalChat();
    petViewModel_->setMenuOpen(true);
}

void CompanionApplication::showModelPickerFromPet()
{
    if (companionMenuWindow_.isNull()) {
        return;
    }
    showChatFromPet();
    if (!QMetaObject::invokeMethod(
            companionMenuWindow_,
            "openModelPicker",
            Qt::QueuedConnection)) {
        reportRuntimeError(qmlStartupError(
            QStringLiteral(
                "model-picker.open-unavailable"),
            QStringLiteral(
                "The Companion model picker could not be opened.")));
    }
}

void CompanionApplication::showUsageFromPet()
{
    if (usageWindow_.isNull()
        || !shellViewModel_
        || !petViewModel_) {
        return;
    }
    if (usageWindow_->isVisible()) {
        closeUsage();
        return;
    }
    shellViewModel_->showProcesses();
    petViewModel_->setMenuOpen(true);
    showUsageNearPet();
}

void CompanionApplication::openAttentionFromPet()
{
    dismissAttention();
    showProcessesFromPet();
}

void CompanionApplication::dismissAttention()
{
    if (petProcessReactionController_) {
        petProcessReactionController_->
            dismissAttention();
    }
    if (!attentionWindow_.isNull()) {
        windowCoordinator_->hide(
            WindowRole::Attention);
    }
}

void CompanionApplication::hidePetFromWindow()
{
    if (petViewModel_) {
        petViewModel_->setVisible(false);
    }
}

void CompanionApplication::registerModelPickerWindow()
{
    const auto registered =
        registerCompanionPopupWindow(
            "modelPickerWindow",
            WindowRole::ModelPicker,
            20,
            modelPickerWindow_);
    if (!registered.hasValue()) {
        reportRuntimeError(
            registered.error());
    }
}

void CompanionApplication::registerGoalWindow()
{
    const auto registered =
        registerCompanionPopupWindow(
            "goalWindow",
            WindowRole::Goal,
            22,
            goalWindow_);
    if (!registered.hasValue()) {
        reportRuntimeError(
            registered.error());
    }
}

void CompanionApplication::quitExplicitly()
{
    explicitQuit_ = true;
    stopCompanionMenuOriginAnimation();
    if (trayIconHost_) {
        trayIconHost_->hide();
        trayIconHost_.reset();
    }
    runtimeHost_.reset();
    powerAvailabilityController_.reset();
    destroyCompanionPopupWindows();
    qmlEngine_.reset();
    singleInstanceGate_.reset();
    application_.quit();
}

Result<void> CompanionApplication::initializeQmlEngine()
{
    shellViewModel_ =
        std::make_unique<
            CompanionShellViewModel>(
                loadedSettings_
                    .selectedChatModelId,
                [this](
                    const QString& modelId) {
                    const auto updated =
                        settingsRepository_.update(
                            [&modelId](
                                AppSettings&
                                    settings) {
                                settings
                                    .selectedChatModelId =
                                    modelId;
                            });
                    if (!updated.hasValue()) {
                        return Result<void>::
                            failure(
                                updated.error());
                    }
                    loadedSettings_ =
                        updated.value();
                    return Result<void>::
                        success();
                },
                this);
    refreshChatAccentColor();
    chatAccentRefreshTimer_ =
        std::make_unique<QTimer>();
    chatAccentRefreshTimer_->setInterval(15000);
    connect(
        chatAccentRefreshTimer_.get(),
        &QTimer::timeout,
        this,
        &CompanionApplication::
            refreshChatAccentColor);
    chatAccentRefreshTimer_->start();
    connect(
        shellViewModel_.get(),
        &CompanionShellViewModel::
            runtimeErrorOccurred,
        this,
        &CompanionApplication::
            reportRuntimeError,
        Qt::UniqueConnection);
    petViewModel_ =
        std::make_unique<PetViewModel>(
            loadedSettings_.petVisible,
            loadedSettings_.animationSpeedScale,
            loadedSettings_.hideControlsUntilHover,
            loadedSettings_.allowAutonomousMovement,
            [this](bool visible) {
                const auto updated =
                    settingsRepository_.update(
                        [visible](
                            AppSettings& settings) {
                            settings.petVisible =
                                visible;
                        });
                if (!updated.hasValue()) {
                    return Result<void>::failure(
                        updated.error());
                }
                loadedSettings_ =
                    updated.value();
                return Result<void>::success();
            },
            this);
    connect(
        petViewModel_.get(),
        &PetViewModel::runtimeErrorOccurred,
        this,
        &CompanionApplication::reportRuntimeError,
        Qt::UniqueConnection);
    petCatalog_ =
        std::make_unique<PetCatalog>(
            PetCatalog::liveRoots());
    mobilePresencePetCatalogService_ =
        std::make_shared<
            MobilePresencePetCatalogService>();
    const auto configuredPetCatalog =
        petViewModel_->configurePetCatalog(
            *petCatalog_,
            loadedSettings_.selectedPetId,
            [this](const QString& petId) {
                const auto updated =
                    settingsRepository_.update(
                        [&petId](
                            AppSettings& settings) {
                            settings.selectedPetId =
                                petId;
                        });
                if (!updated.hasValue()) {
                    return Result<void>::failure(
                        updated.error());
                }
                loadedSettings_ =
                    updated.value();
                return Result<void>::success();
            });
    if (!configuredPetCatalog.hasValue()) {
        return Result<void>::failure(
            configuredPetCatalog.error());
    }
    publishMobilePresencePetSnapshot();
    connect(
        petViewModel_.get(),
        &PetViewModel::selectedPetChanged,
        this,
        &CompanionApplication::
            publishMobilePresencePetSnapshot,
        Qt::UniqueConnection);
    petProcessReactionController_ =
        std::make_unique<
            PetProcessReactionController>(
            *petViewModel_,
            PetProcessReactionController::
                Timings{
                    2400,
                    9000,
                    6200,
                    180,
                },
            settingsRepository_.filePath(),
            this);
    petWindowController_ =
        std::make_unique<
            PetWindowController>(
            *petViewModel_,
            loadedSettings_
                .petWindowPosition,
            [this](QPoint position) {
                const auto updated =
                    settingsRepository_.update(
                        [position](
                            AppSettings& settings) {
                            settings
                                .petWindowPosition =
                                position;
                        });
                if (!updated.hasValue()) {
                    return Result<void>::
                        failure(
                            updated.error());
                }
                loadedSettings_ =
                    updated.value();
                return Result<void>::
                    success();
            },
            this);
    connect(
        petWindowController_.get(),
        &PetWindowController::
            runtimeErrorOccurred,
        this,
        &CompanionApplication::
            reportRuntimeError,
        Qt::UniqueConnection);
    if (!credentialStore_) {
        credentialStore_ = productionRuntimeEnabled_
            ? std::make_shared<
                  DpapiCredentialStore>()
            : std::make_shared<
                  DpapiCredentialStore>(
                      QFileInfo(
                          settingsRepository_
                              .filePath())
                          .absoluteDir()
                          .filePath(
                              QStringLiteral(
                                  "Credentials")));
    }
    settingsViewModel_ = std::make_unique<SettingsViewModel>(
        loadedSettings_,
        settingsRepository_,
        [this](BackdropMode mode) {
            const auto applied = windowCoordinator_->setBackdropMode(
                mode,
                WindowCoordinator::ErrorReportMode::ReturnOnly);
            if (!applied.hasValue()) {
                return Result<BackdropMode>::failure(applied.error());
            }
            return windowCoordinator_->effectiveBackdropMode(WindowRole::Settings);
        },
        credentialStore_,
        this);
    connect(settingsViewModel_.get(),
            &SettingsViewModel::runtimeErrorOccurred,
            this,
            &CompanionApplication::reportRuntimeError,
            Qt::UniqueConnection);
    if (productionRuntimeEnabled_) {
        const auto environment =
            CodexEnvironment::discover();
        if (!environment.hasValue()) {
            reportRuntimeError(
                environment.error());
        } else {
            const auto executable =
                CodexInstallationDiscovery::
                    firstRunnable(
                        environment.value());
            if (!executable.hasValue()) {
                reportRuntimeError(
                    executable.error());
            } else {
                const auto accountsInitialized =
                    initializeCodexAccountServices(
                        environment.value(),
                        executable.value());
                if (!accountsInitialized
                         .hasValue()) {
                    reportRuntimeError(
                        accountsInitialized
                            .error());
                }
            }
        }
    }
    windowBackdropState_ =
        std::make_unique<WindowBackdropState>(
            this);
    connect(
        windowCoordinator_.get(),
        &WindowCoordinator::
            effectiveBackdropModeChanged,
        this,
        [this](
            WindowRole role,
            BackdropMode mode) {
            if (windowBackdropState_) {
                windowBackdropState_->
                    setEffectiveMode(
                        role,
                        mode);
            }
            if (role == WindowRole::Settings
                && settingsViewModel_) {
                settingsViewModel_->
                    setEffectiveBackdropMode(
                        mode);
            }
        });
    const auto mobileConfigurationConnection =
        connect(
            settingsViewModel_.get(),
            &SettingsViewModel::
                mobileConfigurationChanged,
            this,
            &CompanionApplication::
                reloadAndApplyMobileSettings,
            Qt::UniqueConnection);
    if (!mobileConfigurationConnection) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "settings.mobile-connection-failed"),
                QStringLiteral(
                    "Could not connect live mobile settings to the Windows runtime.")));
    }
    connect(
        settingsViewModel_.get(),
        &SettingsViewModel::
            automaticallyContinuesAcrossCodexAccountsChanged,
        this,
        [this] {
            if (!settingsViewModel_) {
                return;
            }
            const bool enabled =
                settingsViewModel_->
                    automaticallyContinuesAcrossCodexAccounts();
            loadedSettings_
                .automaticallyContinuesAcrossCodexAccounts =
                enabled;
            if (runtimeHost_) {
                runtimeHost_->
                    setAutomaticAccountContinuationEnabled(
                        enabled);
            }
        });
    updateService_ =
        createProductionUpdateService(
            [](
                const UpdateManifest&
                    manifest,
                const VerifiedArtifact&
                    artifact) {
                auto handoff =
                    UpdateInstallerHandoff::
                        createProduction(
                            QCoreApplication::
                                applicationDirPath());
                const auto launched =
                    handoff.launch(
                        manifest,
                        artifact);
                return launched.hasValue()
                    ? Result<void>::success()
                    : Result<void>::failure(
                          launched.error());
            },
            this,
            updateManifestUrlOverride_);
    updateViewModel_ =
        std::make_unique<UpdateViewModel>(
            *updateService_,
            this);
    connect(
        updateViewModel_.get(),
        &UpdateViewModel::
            runtimeErrorOccurred,
        this,
        &CompanionApplication::
            reportRuntimeError,
        Qt::UniqueConnection);
    connect(
        updateService_.get(),
        &UpdateService::installLaunched,
        this,
        [this](const QString&) {
            quitExplicitly();
        },
        Qt::QueuedConnection);
    connect(
        settingsViewModel_.get(),
        &SettingsViewModel::
            animationSpeedScaleChanged,
        this,
        [this] {
            if (petViewModel_
                && settingsViewModel_) {
                petViewModel_->
                    setAnimationSpeedScale(
                        settingsViewModel_->
                            animationSpeedScale());
            }
        });
    connect(
        settingsViewModel_.get(),
        &SettingsViewModel::
            hideControlsUntilHoverChanged,
        this,
        [this] {
            if (petViewModel_
                && settingsViewModel_) {
                petViewModel_->
                    setHideControlsUntilHover(
                        settingsViewModel_->
                            hideControlsUntilHover());
            }
        });
    connect(
        settingsViewModel_.get(),
        &SettingsViewModel::
            allowAutonomousMovementChanged,
        this,
        [this] {
            if (petViewModel_
                && settingsViewModel_) {
                petViewModel_->
                    setAllowAutonomousMovement(
                        settingsViewModel_->
                            allowAutonomousMovement());
            }
        });
    if (powerAvailabilityController_) {
        settingsViewModel_->
            setMobileAvailabilityCommand(
                [this](
                    bool mobileEnabled,
                    bool keepAvailable) {
                    if (!powerAvailabilityController_) {
                        return Result<void>::failure(
                            {
                                QStringLiteral(
                                    "power.controller-unavailable"),
                                QStringLiteral(
                                    "The Windows Companion availability controller is unavailable."),
                                false,
                                {},
                            });
                    }
                    return powerAvailabilityController_
                        ->setAvailable(
                            mobileEnabled
                            && keepAvailable);
                });
    }

    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("companionSettings"),
        QVariant::fromValue(settingsRepository_.filePath()));
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("settingsViewModel"),
        settingsViewModel_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("windowBackdropState"),
        windowBackdropState_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("updateViewModel"),
        updateViewModel_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("petViewModel"),
        petViewModel_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral(
            "petWindowController"),
        petWindowController_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral(
            "petProcessReactions"),
        petProcessReactionController_.get());
    qmlEngine_->rootContext()->setContextProperty(
        QStringLiteral("companionShell"),
        shellViewModel_.get());
    qmlEngine_->loadFromModule(QStringLiteral("CodexCompanion"),
                               QStringLiteral("Main"));

    if (qmlEngine_->rootObjects().isEmpty()) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("settings.qml-load-failed"),
            QStringLiteral("Could not load the Companion Settings interface.")));
    }

    QObject* rootObject =
        qmlEngine_->rootObjects().constFirst();
    auto* petWindow =
        rootObject->findChild<QQuickWindow*>(
            QStringLiteral("petWindow"));
    if (petWindow == nullptr) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("pet.window-unavailable"),
            QStringLiteral("The Companion pet interface did not create a window.")));
    }
    petWindow_ = petWindow;
    auto* settingsWindow =
        rootObject->findChild<QQuickWindow*>(
            QStringLiteral("settingsWindow"));
    if (settingsWindow == nullptr) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("settings.window-unavailable"),
            QStringLiteral("The Companion Settings interface did not create a window.")));
    }
    settingsWindow_ = settingsWindow;
    auto* companionMenuWindow =
        rootObject->findChild<QQuickWindow*>(
            QStringLiteral("companionMenuWindow"));
    if (companionMenuWindow == nullptr) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("companion-menu.window-unavailable"),
            QStringLiteral("The Companion menu interface did not create a window.")));
    }
    companionMenuWindow_ = companionMenuWindow;
    companionMenuWindow->installEventFilter(this);
    connect(
        companionMenuWindow,
        SIGNAL(modelPickerWindowChanged()),
        this,
        SLOT(registerModelPickerWindow()),
        Qt::UniqueConnection);
    connect(
        companionMenuWindow,
        SIGNAL(goalWindowChanged()),
        this,
        SLOT(registerGoalWindow()),
        Qt::UniqueConnection);
    auto* usageWindow =
        rootObject->findChild<QQuickWindow*>(
            QStringLiteral("usageWindow"));
    if (usageWindow == nullptr) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("usage.window-unavailable"),
            QStringLiteral("The Companion usage interface did not create a window.")));
    }
    usageWindow_ = usageWindow;
    usageWindow->installEventFilter(this);
    auto* attentionWindow =
        rootObject->findChild<QQuickWindow*>(
            QStringLiteral("attentionWindow"));
    if (attentionWindow == nullptr) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "attention.window-unavailable"),
                QStringLiteral(
                    "The Companion attention interface did not create a window.")));
    }
    attentionWindow_ = attentionWindow;

    const auto registered =
        windowCoordinator_->registerWindow(WindowRole::Settings, *settingsWindow);
    if (!registered.hasValue()) {
        return registered;
    }
    const auto menuRegistered =
        windowCoordinator_->registerWindow(
            WindowRole::CompanionMenu,
            *companionMenuWindow);
    if (!menuRegistered.hasValue()) {
        return menuRegistered;
    }
    const auto usageRegistered =
        windowCoordinator_->registerWindow(
            WindowRole::Usage,
            *usageWindow);
    if (!usageRegistered.hasValue()) {
        return usageRegistered;
    }
    const auto attentionRegistered =
        windowCoordinator_->registerWindow(
            WindowRole::Attention,
            *attentionWindow);
    if (!attentionRegistered.hasValue()) {
        return attentionRegistered;
    }
    const auto petRegistered =
        windowCoordinator_->registerWindow(
            WindowRole::Pet,
            *petWindow);
    if (!petRegistered.hasValue()) {
        return petRegistered;
    }

    petWindow->create();
    settingsWindow->create();
    companionMenuWindow->create();
    usageWindow->create();
    attentionWindow->create();
    if (petWindow->handle() == nullptr
        || settingsWindow->handle() == nullptr
        || companionMenuWindow->handle() == nullptr
        || usageWindow->handle() == nullptr
        || attentionWindow->handle() == nullptr) {
        return Result<void>::failure(qmlStartupError(
            QStringLiteral("companion.window-surface-unavailable"),
            QStringLiteral("Could not create the Companion utility-window surfaces.")));
    }

    const auto materialSet =
        windowCoordinator_->setBackdropMode(loadedSettings_.backdrop);
    if (!materialSet.hasValue()) {
        return materialSet;
    }
    constexpr std::array backdropRoles = {
        WindowRole::Settings,
        WindowRole::CompanionMenu,
        WindowRole::ModelPicker,
        WindowRole::Goal,
        WindowRole::Usage,
        WindowRole::Attention,
    };
    for (const auto role : backdropRoles) {
        const auto effective =
            windowCoordinator_->
                effectiveBackdropMode(role);
        if (!effective.hasValue()) {
            return Result<void>::failure(
                effective.error());
        }
        windowBackdropState_->
            setEffectiveMode(
                role,
                effective.value());
        if (role == WindowRole::Settings) {
            settingsViewModel_->
                setEffectiveBackdropMode(
                    effective.value());
        }
    }

    connect(settingsWindow,
            SIGNAL(closeRequested()),
            this,
            SLOT(closeSettings()),
            Qt::UniqueConnection);
    connect(petWindow,
            SIGNAL(usageRequested()),
            this,
            SLOT(showUsageFromPet()),
            Qt::UniqueConnection);
    connect(petWindow,
            SIGNAL(modelPickerRequested()),
            this,
            SLOT(showModelPickerFromPet()),
            Qt::UniqueConnection);
    connect(petWindow,
            SIGNAL(processesRequested()),
            this,
            SLOT(showProcessesFromPet()),
            Qt::UniqueConnection);
    connect(petWindow,
            SIGNAL(chatRequested()),
            this,
            SLOT(showChatFromPet()),
            Qt::UniqueConnection);
    connect(petWindow,
            SIGNAL(hideRequested()),
            this,
            SLOT(hidePetFromWindow()),
            Qt::UniqueConnection);
    connect(companionMenuWindow,
            SIGNAL(closeRequested()),
            this,
            SLOT(closeCompanionMenu()),
            Qt::UniqueConnection);
    connect(usageWindow,
            SIGNAL(closeRequested()),
            this,
            SLOT(closeUsage()),
            Qt::UniqueConnection);
    connect(attentionWindow,
            SIGNAL(openRequested()),
            this,
            SLOT(openAttentionFromPet()),
            Qt::UniqueConnection);
    connect(attentionWindow,
            SIGNAL(dismissRequested()),
            this,
            SLOT(dismissAttention()),
            Qt::UniqueConnection);
    connect(companionMenuWindow,
            &QQuickWindow::visibleChanged,
            this,
            &CompanionApplication::refreshTrayRouteState,
            Qt::UniqueConnection);
    connect(companionMenuWindow,
            &QQuickWindow::visibleChanged,
            this,
            &CompanionApplication::syncProcessSurfaceVisibility,
            Qt::UniqueConnection);
    connect(companionMenuWindow,
            &QQuickWindow::widthChanged,
            this,
            &CompanionApplication::
                scheduleCompanionMenuGeometryReconciliation);
    connect(companionMenuWindow,
            &QQuickWindow::heightChanged,
            this,
            &CompanionApplication::
                scheduleCompanionMenuGeometryReconciliation);
    const auto repositionUsage =
        [this] {
            if (!usageWindow_.isNull()
                && usageWindow_->isVisible()) {
                positionUsageNearPet();
            }
        };
    connect(usageWindow,
            &QQuickWindow::widthChanged,
            this,
            repositionUsage);
    connect(usageWindow,
            &QQuickWindow::heightChanged,
            this,
            repositionUsage);
    connect(shellViewModel_.get(),
            &CompanionShellViewModel::routeModeChanged,
            this,
            &CompanionApplication::syncProcessSurfaceVisibility,
            Qt::UniqueConnection);
    connect(
        petViewModel_.get(),
        &PetViewModel::visibilityChanged,
        this,
        &CompanionApplication::
            syncPetWindowVisibility,
        Qt::UniqueConnection);
    connect(
        petProcessReactionController_.get(),
        &PetProcessReactionController::
            attentionChanged,
        this,
        &CompanionApplication::
            syncAttentionWindow,
        Qt::UniqueConnection);
    connect(
        petViewModel_.get(),
        &PetViewModel::menuOpenChanged,
        this,
        [this] {
            if (!petViewModel_) {
                return;
            }
            if (petViewModel_->menuOpen()) {
                showCompanionMenuNearPet();
            } else {
                stopCompanionMenuOriginAnimation();
                windowCoordinator_->hide(
                    WindowRole::CompanionMenu);
                closeUsage();
                refreshTrayRouteState();
            }
            syncAttentionWindow();
        });
    connect(
        petWindowController_.get(),
        &PetWindowController::
            windowPositionChanged,
        this,
        [this] {
            if (petViewModel_
                && petViewModel_->menuOpen()) {
                positionCompanionMenuNearPet();
            }
            if (!usageWindow_.isNull()
                && usageWindow_->isVisible()) {
                positionUsageNearPet();
            }
            if (!attentionWindow_.isNull()
                && attentionWindow_->isVisible()) {
                positionAttentionWindow();
            }
        });
    connect(
        petViewModel_.get(),
        &PetViewModel::controlsVisibleChanged,
        this,
        [this] {
            const bool pointerOverUtility =
                companionUtilityContainsCursor();
            companionUtilityPointerHovered_ =
                pointerOverUtility;
            if (pointerOverUtility) {
                freezeCompanionUtilityAnchor();
                return;
            }
            if (petViewModel_
                && petViewModel_->menuOpen()) {
                positionCompanionMenuNearPet(true);
            }
            if (!usageWindow_.isNull()
                && usageWindow_->isVisible()) {
                positionUsageNearPet();
            }
        });
    petWindowController_->attachWindow(
        *petWindow);
    petWindowController_->start();
    syncPetWindowVisibility();
    syncAttentionWindow();

    return Result<void>::success();
}

Result<void>
CompanionApplication::
registerCompanionPopupWindow(
    const char* propertyName,
    WindowRole role,
    qreal radius,
    QPointer<QQuickWindow>& destination)
{
    if (companionMenuWindow_.isNull()) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "popup.owner-unavailable"),
                QStringLiteral(
                    "The Companion popup owner is unavailable.")));
    }

    auto* popupWindow =
        qobject_cast<QQuickWindow*>(
            companionMenuWindow_->
                property(propertyName)
                .value<QObject*>());
    if (popupWindow == nullptr) {
        return Result<void>::success();
    }
    if (destination == popupWindow) {
        return Result<void>::success();
    }

    popupWindow->setProperty(
        "nativeBackdropRegionEnabled",
        true);
    popupWindow->setProperty(
        "nativeBackdropRegionInsetLeft",
        0);
    popupWindow->setProperty(
        "nativeBackdropRegionInsetTop",
        0);
    popupWindow->setProperty(
        "nativeBackdropRegionInsetRight",
        0);
    popupWindow->setProperty(
        "nativeBackdropRegionInsetBottom",
        0);
    popupWindow->setProperty(
        "nativeBackdropRegionRadius",
        radius);
    popupWindow->setTransientParent(
        companionMenuWindow_);

    const auto registered =
        windowCoordinator_->
            registerWindow(
                role,
                *popupWindow);
    if (!registered.hasValue()) {
        return registered;
    }
    const auto owned =
        windowCoordinator_->setOwner(
            role,
            WindowRole::CompanionMenu);
    if (!owned.hasValue()) {
        return owned;
    }

    destination = popupWindow;
    return Result<void>::success();
}

void CompanionApplication::
destroyCompanionPopupWindows() noexcept
{
    const auto destroyWindow =
        [](QPointer<QQuickWindow>& window) {
            if (window.isNull()) {
                return;
            }
            window->setTransientParent(
                nullptr);
            window->close();
            window->destroy();
            window.clear();
        };
    destroyWindow(goalWindow_);
    destroyWindow(modelPickerWindow_);
}

void CompanionApplication::refreshChatAccentColor()
{
    if (!shellViewModel_) {
        return;
    }
    shellViewModel_->setChatAccentColor(
        ChatGPTAccentThemeReader::colorName(
            ChatGPTAccentThemeReader::
                currentTheme()));
}

MobilePresencePetCatalogSnapshot
CompanionApplication::
mobilePresencePetSnapshot() const
{
    MobilePresencePetCatalogSnapshot
        snapshot;
    if (petViewModel_) {
        const QString selectedPetId =
            petViewModel_->
                selectedPetId()
                .trimmed();
        if (!selectedPetId.isEmpty()) {
            snapshot.selectedDesktopPetId =
                selectedPetId;
        }
    }
    if (!petCatalog_) {
        return snapshot;
    }

    for (const PetDefinition& pet :
         petCatalog_->pets()) {
        if (!pet.mobilePresence
                 .has_value()) {
            continue;
        }
        snapshot.packages.append({
            pet.id,
            pet.displayName,
            QDir(pet.sourceDirectory)
                .filePath(
                    pet.mobilePresence
                        ->directory),
            pet.mobilePresence->packageId,
            pet.mobilePresence
                ->contentHash,
        });
    }
    return snapshot;
}

void CompanionApplication::
publishMobilePresencePetSnapshot()
{
    if (!mobilePresencePetCatalogService_) {
        return;
    }
    const QVector<CompanionError>
        diagnostics =
            mobilePresencePetCatalogService_
                ->replaceSnapshot(
                    mobilePresencePetSnapshot());
    for (const CompanionError& error :
         diagnostics) {
        reportRuntimeError(error);
    }
}

void CompanionApplication::
reloadAndApplyMobileSettings()
{
    const auto loaded =
        settingsRepository_.load();
    if (!loaded.hasValue()) {
        reportRuntimeError(
            loaded.error());
        return;
    }
    loadedSettings_ =
        loaded.value();
    if (!runtimeHost_) {
        return;
    }

    const auto applied =
        runtimeHost_->
            applyMobileSettings(
                loadedSettings_);
    if (settingsViewModel_) {
        settingsViewModel_->
            setMobileNearbyAccessState(
                runtimeHost_->
                    mobileNearbyAccessAvailable(),
                runtimeHost_->
                    mobileNearbyAccessStatusText());
    }
    if (!applied.hasValue()) {
        reportRuntimeError(
            applied.error());
    }
}

Result<void> CompanionApplication::initializeTrayHost()
{
    auto trayIconHost = std::make_unique<TrayIconHost>(this);
    trayIconHost->setRouteStateProvider([this]() {
        return windowCoordinator_->trayRouteState();
    });
    connect(trayIconHost.get(),
            &TrayIconHost::showPetRequested,
            this,
            [this]() {
                if (petViewModel_) {
                    petViewModel_->toggleVisible();
                }
            });
    connect(trayIconHost.get(),
            &TrayIconHost::showMenuRequested,
            this,
            [this]() {
                if (!petViewModel_) {
                    return;
                }
                if (petViewModel_->menuOpen()) {
                    petViewModel_->setMenuOpen(false);
                } else {
                    showProcessesFromPet();
                }
            });
    connect(trayIconHost.get(),
            &TrayIconHost::showProcessesRequested,
            this,
            [this]() {
                shellViewModel_->showProcesses();
                showCompanionMenu();
            });
    connect(trayIconHost.get(),
            &TrayIconHost::showChatRequested,
            this,
            [this]() {
                shellViewModel_->showLocalChat();
                showCompanionMenu();
            });
    connect(trayIconHost.get(),
            &TrayIconHost::showSettingsRequested,
            this,
            &CompanionApplication::showSettings);
    connect(trayIconHost.get(),
            &TrayIconHost::hideSettingsRequested,
            this,
            &CompanionApplication::closeSettings);
    connect(trayIconHost.get(),
            &TrayIconHost::quitRequested,
            this,
            &CompanionApplication::quitExplicitly);
    connect(trayIconHost.get(),
            &TrayIconHost::runtimeErrorOccurred,
            this,
            &CompanionApplication::reportRuntimeError,
            Qt::UniqueConnection);

    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(kCompanionIconResourceId),
        IMAGE_ICON,
        0,
        0,
        LR_DEFAULTSIZE | LR_SHARED));
    if (icon == nullptr) {
        return Result<void>::failure(trayStartupError(
            QStringLiteral("tray.icon-resource-missing"),
            QStringLiteral("Could not load the Companion executable icon resource.")));
    }

    const auto shown = trayIconHost->show(
        icon,
        QStringLiteral("Codex Companion"));
    if (!shown.hasValue()) {
        return shown;
    }

    trayIconHost_ = std::move(trayIconHost);
    return Result<void>::success();
}

Result<void> CompanionApplication::initializeRuntimeServices()
{
    auto runtimeHost =
        CompanionRuntimeHost::createProduction(
            *shellViewModel_,
            loadedSettings_,
            credentialStore_,
            this,
            codexAccountRouter_.get(),
            codexAccountProfileStore_.get(),
            codexThreadAccountBindingStore_
                .get(),
            mobilePresencePetCatalogService_);
    if (!runtimeHost.hasValue()) {
        shellViewModel_->setProcessStatus(
            false,
            runtimeHost.error().message);
        shellViewModel_->setChatStatus(
            false,
            false,
            false,
            {},
            runtimeHost.error().message);
        return Result<void>::failure(
            runtimeHost.error());
    }

    runtimeHost_ = std::move(runtimeHost.value());
    connect(
        runtimeHost_.get(),
        &CompanionRuntimeHost::
            petAnimationRequested,
        petViewModel_.get(),
        &PetViewModel::setSelectedAnimation,
        Qt::UniqueConnection);
    if (petProcessReactionController_) {
        petProcessReactionController_->
            setProcessModel(
                runtimeHost_->processModel());
    }
    settingsViewModel_->
        setMobilePairingCoordinator(
            runtimeHost_->
                mobilePairingCoordinator());
    settingsViewModel_->
        setRelayPairingBootstrap(
            runtimeHost_->
                mobileRelayPairingBootstrap());
    settingsViewModel_->
        setMobileNearbyAccessState(
            runtimeHost_->
                mobileNearbyAccessAvailable(),
            runtimeHost_->
                mobileNearbyAccessStatusText());
    connect(
        runtimeHost_.get(),
        &CompanionRuntimeHost::
            mobileNearbyAccessChanged,
        settingsViewModel_.get(),
        &SettingsViewModel::
            setMobileNearbyAccessState,
        Qt::UniqueConnection);
    connect(
        settingsViewModel_.get(),
        &SettingsViewModel::chatCredentialsChanged,
        runtimeHost_.get(),
        &CompanionRuntimeHost::
            refreshChatAvailability,
        Qt::UniqueConnection);
    connect(runtimeHost_.get(),
            &CompanionRuntimeHost::runtimeErrorOccurred,
            this,
            &CompanionApplication::reportRuntimeError,
            Qt::UniqueConnection);
    const auto started = runtimeHost_->start();
    settingsViewModel_->
        setMobileNearbyAccessState(
            runtimeHost_->
                mobileNearbyAccessAvailable(),
            runtimeHost_->
                mobileNearbyAccessStatusText());
    syncProcessSurfaceVisibility();
    return started;
}

Result<void>
CompanionApplication::
initializeCodexAccountServices(
    const CodexEnvironment& environment,
    QString codexExecutable)
{
    if (!settingsViewModel_) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "settings.accounts-model-unavailable"),
                QStringLiteral(
                    "The Settings model is unavailable for Codex account profiles.")));
    }
    if (environment.localAppData
            .trimmed()
            .isEmpty()
        || environment.codexHome
               .trimmed()
               .isEmpty()
        || codexExecutable
               .trimmed()
               .isEmpty()) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "settings.accounts-runtime-unavailable"),
                QStringLiteral(
                    "The Codex account profile runtime paths are unavailable.")));
    }

    const QString dataRoot =
        QDir(environment.localAppData)
            .filePath(
                QStringLiteral(
                    "Codex Companion"));
    const QString profilesRoot =
        QDir(dataRoot).filePath(
            QStringLiteral(
                "Codex Profiles"));
    if (!QDir().mkpath(
            profilesRoot)) {
        return Result<void>::failure(
            qmlStartupError(
                QStringLiteral(
                    "settings.accounts-directory-failed"),
                QStringLiteral(
                    "Could not create the Codex account profile directory.")));
    }

    auto profileStore =
        std::make_unique<
            CodexAccountProfileStore>(
            QDir(dataRoot).filePath(
                QStringLiteral(
                    "codex-account-profiles.json")));
    if (const auto error =
            profileStore->loadError();
        error.has_value()) {
        return Result<void>::failure(
            *error);
    }
    auto bindingStore =
        std::make_unique<
            CodexThreadAccountBindingStore>(
            QDir(dataRoot).filePath(
                QStringLiteral(
                    "codex-thread-account-bindings.json")));
    if (const auto error =
            bindingStore->loadError();
        error.has_value()) {
        return Result<void>::failure(
            *error);
    }
    auto accountRuntime =
        std::make_unique<
            CodexAccountRuntime>(
            profilesRoot,
            environment.codexHome);
    auto accountRouter =
        std::make_unique<
            CodexAccountRouter>(
            QProcessEnvironment::
                systemEnvironment(),
            *profileStore,
            *accountRuntime,
            *bindingStore);
    auto loginService =
        std::make_unique<
            CodexAccountLoginService>(
            std::move(codexExecutable),
            QProcessEnvironment::
                systemEnvironment(),
            *accountRuntime);

    codexAccountProfileStore_ =
        std::move(profileStore);
    codexThreadAccountBindingStore_ =
        std::move(bindingStore);
    codexAccountRuntime_ =
        std::move(accountRuntime);
    codexAccountRouter_ =
        std::move(accountRouter);
    codexAccountLoginService_ =
        std::move(loginService);
    settingsViewModel_->
        setCodexAccountServices(
            codexAccountProfileStore_
                .get(),
            codexAccountRouter_.get(),
            codexAccountLoginService_
                .get());
    return Result<void>::success();
}

void CompanionApplication::showCompanionMenu()
{
    closeUsage();
    if (petViewModel_) {
        petViewModel_->setMenuOpen(true);
        return;
    }

    if (!companionMenuWindow_.isNull()) {
        const QPoint anchor =
            trayIconHost_
            && trayIconHost_->lastActivationPoint().has_value()
            ? *trayIconHost_->lastActivationPoint()
            : QCursor::pos();
        QScreen* screen = QGuiApplication::screenAt(anchor);
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen != nullptr) {
            windowCoordinator_->move(
                WindowRole::CompanionMenu,
                TrayWindowPlacement::nearAnchor(
                    anchor,
                    screen->availableGeometry(),
                    companionMenuWindow_->size(),
                    12));
        }
    }

    const auto shown =
        windowCoordinator_->show(WindowRole::CompanionMenu);
    if (shown.hasValue()) {
        freezeCompanionUtilityAnchor();
        windowCoordinator_->activate(WindowRole::CompanionMenu);
    }
    refreshTrayRouteState();
}

void CompanionApplication::showCompanionMenuNearPet()
{
    positionCompanionMenuNearPet();

    const auto shown =
        windowCoordinator_->show(
            WindowRole::CompanionMenu);
    if (shown.hasValue()) {
        freezeCompanionUtilityAnchor();
        windowCoordinator_->activate(
            WindowRole::CompanionMenu);
    }
    refreshTrayRouteState();
}

std::optional<QPoint>
CompanionApplication::
companionMenuOriginForControls(
    bool controlsVisible) const
{
    if (petWindow_.isNull()
        || companionMenuWindow_.isNull()) {
        return std::nullopt;
    }
    const QRect petFrame(
        petWindow_->position(),
        petWindow_->size());
    const QRect anchorFrame =
        PetMenuPlacement::anchorFrame(
            petFrame,
            controlsVisible);
    QScreen* screen =
        QGuiApplication::screenAt(
            anchorFrame.center());
    if (screen == nullptr) {
        screen =
            QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return std::nullopt;
    }
    return PetMenuPlacement::
        positionedOrigin(
            anchorFrame,
            companionMenuWindow_->size(),
            screen->availableGeometry());
}

void CompanionApplication::
scheduleCompanionMenuGeometryReconciliation()
{
    if (companionMenuGeometryReconciliationPending_) {
        return;
    }
    companionMenuGeometryReconciliationPending_ = true;
    QTimer::singleShot(
        0,
        this,
        [this] {
            companionMenuGeometryReconciliationPending_ =
                false;
            if (!petViewModel_
                || !petViewModel_->menuOpen()
                || companionMenuWindow_.isNull()
                || !companionMenuWindow_->isVisible()) {
                return;
            }
            const bool pointerOverUtility =
                companionUtilityContainsCursor();
            companionUtilityPointerHovered_ =
                pointerOverUtility;
            positionCompanionMenuNearPet(
                false,
                pointerOverUtility
                    ? companionMenuAnchorControlsVisible_
                    : std::nullopt,
                false);
            if (!usageWindow_.isNull()
                && usageWindow_->isVisible()) {
                positionUsageNearPet();
            }
            if (pointerOverUtility) {
                freezeCompanionUtilityAnchor();
            }
            reconcileCompanionProcessHover(true);
            ++companionMenuGeometryReconciliationCount_;
        });
}

void CompanionApplication::positionCompanionMenuNearPet(
    bool animated,
    std::optional<bool>
        controlsVisibleOverride,
    bool reconcileHover)
{
    if (petWindow_.isNull()
        || companionMenuWindow_.isNull()) {
        return;
    }
    if (animated
        && companionUtilityContainsCursor()) {
        stopCompanionMenuOriginAnimation();
        return;
    }

    const bool controlsVisible =
        controlsVisibleOverride.value_or(
            !petViewModel_
            || petViewModel_->controlsVisible());
    const auto targetOrigin =
        companionMenuOriginForControls(
            controlsVisible);
    if (!targetOrigin.has_value()) {
        return;
    }
    companionMenuAnchorControlsVisible_ =
        controlsVisible;
    if (animated
        && companionMenuWindow_->isVisible()
        && companionMenuWindow_->position()
            != *targetOrigin) {
        animateCompanionMenuOrigin(
            *targetOrigin);
        return;
    }
    stopCompanionMenuOriginAnimation();
    windowCoordinator_->move(
        WindowRole::CompanionMenu,
        *targetOrigin);
    if (reconcileHover) {
        reconcileCompanionProcessHover(true);
    }
}

void CompanionApplication::animateCompanionMenuOrigin(
    QPoint targetOrigin)
{
    if (companionMenuWindow_.isNull()
        || !companionMenuWindow_->isVisible()) {
        return;
    }

    if (!companionMenuOriginAnimation_) {
        companionMenuOriginAnimation_ =
            std::make_unique<QVariantAnimation>();
        companionMenuOriginAnimation_->setDuration(
            kMenuMotionDurationMilliseconds);
        companionMenuOriginAnimation_->setStartValue(
            0.0);
        companionMenuOriginAnimation_->setEndValue(
            1.0);
        connect(
            companionMenuOriginAnimation_.get(),
            &QVariantAnimation::valueChanged,
            this,
            [this](const QVariant& value) {
                if (companionMenuWindow_.isNull()
                    || !companionMenuWindow_
                            ->isVisible()) {
                    stopCompanionMenuOriginAnimation();
                    return;
                }
                windowCoordinator_->move(
                    WindowRole::CompanionMenu,
                    interpolatedOrigin(
                        companionMenuOriginAnimationStart_,
                        companionMenuOriginAnimationTarget_,
                        menuMotionProgress(
                            value.toDouble())));
                reconcileCompanionProcessHover(true);
                if (!usageWindow_.isNull()
                    && usageWindow_->isVisible()) {
                    positionUsageNearPet();
                }
            });
        connect(
            companionMenuOriginAnimation_.get(),
            &QVariantAnimation::finished,
            this,
            [this] {
                if (companionMenuWindow_.isNull()) {
                    return;
                }
                windowCoordinator_->move(
                    WindowRole::CompanionMenu,
                    companionMenuOriginAnimationTarget_);
                reconcileCompanionProcessHover(true);
                if (!usageWindow_.isNull()
                    && usageWindow_->isVisible()) {
                    positionUsageNearPet();
                }
            });
    }

    companionMenuOriginAnimation_->stop();
    companionMenuOriginAnimationStart_ =
        companionMenuWindow_->position();
    companionMenuOriginAnimationTarget_ =
        targetOrigin;
    const quint64 generation =
        ++companionMenuOriginAnimationGeneration_;
    QTimer::singleShot(
        0,
        this,
        [this, generation] {
            if (generation
                    != companionMenuOriginAnimationGeneration_
                || !companionMenuOriginAnimation_
                || companionMenuWindow_.isNull()
                || !companionMenuWindow_->isVisible()) {
                return;
            }
            companionMenuOriginAnimation_->start();
            companionMenuOriginAnimation_->setCurrentTime(
                kMenuMotionPrimingMilliseconds);
        });
}

void CompanionApplication::
    stopCompanionMenuOriginAnimation()
{
    ++companionMenuOriginAnimationGeneration_;
    if (companionMenuOriginAnimation_) {
        companionMenuOriginAnimation_->stop();
    }
}

void CompanionApplication::
reconcileCompanionProcessHover(
    bool force)
{
    if (companionMenuWindow_.isNull()
        || !companionMenuWindow_->isVisible()) {
        lastCompanionProcessHoverCursor_.reset();
        return;
    }
    const QPoint globalPointer =
        companionProcessCursorPositionSource_
        ? companionProcessCursorPositionSource_()
        : QCursor::pos();
    const QString hoveredProcessId =
        companionMenuWindow_
            ->property("hoveredProcessId")
            .toString();
    if (!force
        && lastCompanionProcessHoverCursor_
            .has_value()
        && *lastCompanionProcessHoverCursor_
            == globalPointer
        && !hoveredProcessId.isEmpty()) {
        return;
    }
    lastCompanionProcessHoverCursor_ =
        globalPointer;
    const QPoint localPointer =
        companionMenuWindow_->mapFromGlobal(
            globalPointer);
    QMetaObject::invokeMethod(
        companionMenuWindow_,
        "reconcileProcessHoverAt",
        Qt::DirectConnection,
        Q_ARG(
            QVariant,
            QVariant(localPointer.x())),
        Q_ARG(
            QVariant,
            QVariant(localPointer.y())));
}

void CompanionApplication::
freezeCompanionUtilityAnchor()
{
    stopCompanionMenuOriginAnimation();
    if (!companionUtilityHoverTimer_) {
        companionUtilityHoverTimer_ =
            std::make_unique<QTimer>();
        companionUtilityHoverTimer_
            ->setInterval(
                kCompanionUtilityHoverPollMilliseconds);
        companionUtilityHoverTimer_
            ->setTimerType(
                Qt::PreciseTimer);
        connect(
            companionUtilityHoverTimer_.get(),
            &QTimer::timeout,
            this,
            &CompanionApplication::
                reconcileCompanionUtilityPointerHover);
    }
    if (!companionUtilityHoverTimer_
             ->isActive()) {
        companionUtilityHoverTimer_
            ->start();
    }
}

void CompanionApplication::
    reconcileCompanionUtilityPointerHover()
{
    if (companionUtilityContainsCursor()) {
        companionUtilityPointerHovered_ =
            true;
        stopCompanionMenuOriginAnimation();
        reconcileCompanionProcessHover();
        return;
    }

    const bool wasHovered =
        companionUtilityPointerHovered_;
    companionUtilityPointerHovered_ =
        false;
    const bool utilityVisible =
        (!companionMenuWindow_.isNull()
         && companionMenuWindow_->isVisible())
        || (!usageWindow_.isNull()
            && usageWindow_->isVisible());
    if (!utilityVisible
        && companionUtilityHoverTimer_) {
        companionUtilityHoverTimer_
            ->stop();
    }
    const auto expectedOrigin =
        petViewModel_
        && petViewModel_->menuOpen()
        && !companionMenuWindow_.isNull()
        && companionMenuWindow_->isVisible()
        ? companionMenuOriginForControls(
              petViewModel_->
                  controlsVisible())
        : std::nullopt;
    const bool animationTargetsExpectedOrigin =
        expectedOrigin.has_value()
        && companionMenuOriginAnimation_
        && companionMenuOriginAnimation_
               ->state()
            == QAbstractAnimation::Running
        && companionMenuOriginAnimationTarget_
            == *expectedOrigin;
    const bool originNeedsReconciliation =
        expectedOrigin.has_value()
        && companionMenuWindow_->position()
            != *expectedOrigin
        && !animationTargetsExpectedOrigin;
    if (!wasHovered
        && !originNeedsReconciliation) {
        return;
    }
    reconcileCompanionProcessHover(true);
    if (petViewModel_
        && petViewModel_->menuOpen()
        && !companionMenuWindow_.isNull()
        && companionMenuWindow_->isVisible()) {
        positionCompanionMenuNearPet(true);
    }
    if (!usageWindow_.isNull()
        && usageWindow_->isVisible()) {
        positionUsageNearPet();
    }
}

void CompanionApplication::
    setCompanionUtilityPointerHovered(
        bool hovered)
{
    if (hovered) {
        companionUtilityPointerHovered_ =
            companionUtilityContainsCursor();
        if (!companionUtilityPointerHovered_) {
            return;
        }
        freezeCompanionUtilityAnchor();
        return;
    }

    reconcileCompanionUtilityPointerHover();
}

bool CompanionApplication::
    companionUtilityContainsCursor() const
{
    if (companionUtilityCursorPresenceSource_) {
        return companionUtilityCursorPresenceSource_();
    }

    POINT nativeCursor {};
    if (GetCursorPos(&nativeCursor) != FALSE) {
        const auto containsNativeCursor =
            [&nativeCursor](
                const QPointer<
                    QQuickWindow>& window) {
                return !window.isNull()
                    && window->isVisible()
                    && nativeWindowContainsCursor(
                        *window,
                        nativeCursor);
            };
        return containsNativeCursor(
                   companionMenuWindow_)
            || containsNativeCursor(
                   usageWindow_);
    }

    const QPoint cursor = QCursor::pos();
    const auto containsCursor =
        [&cursor](const QPointer<QQuickWindow>& window) {
            return !window.isNull()
                && window->isVisible()
                && QRect(
                       window->position(),
                       window->size())
                       .contains(cursor);
        };
    return containsCursor(
               companionMenuWindow_)
        || containsCursor(
               usageWindow_);
}

void CompanionApplication::showUsageNearPet()
{
    positionUsageNearPet();

    const auto shown =
        windowCoordinator_->show(
            WindowRole::Usage);
    if (shown.hasValue()) {
        freezeCompanionUtilityAnchor();
        windowCoordinator_->activate(
            WindowRole::Usage);
    }
}

void CompanionApplication::positionUsageNearPet()
{
    if (petWindow_.isNull()
        || usageWindow_.isNull()) {
        return;
    }

    const QRect petFrame(
        petWindow_->position(),
        petWindow_->size());
    const QRect anchorFrame =
        PetMenuPlacement::anchorFrame(
            petFrame,
            !petViewModel_
                || petViewModel_->controlsVisible());
    QScreen* screen =
        QGuiApplication::screenAt(
            anchorFrame.center());
    if (screen == nullptr) {
        screen =
            QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        QPoint origin;
        if (!companionMenuWindow_.isNull()
            && companionMenuWindow_->isVisible()) {
            origin =
                PetMenuPlacement::
                    positionedAuxiliaryOrigin(
                        QRect(
                            companionMenuWindow_
                                ->position(),
                            companionMenuWindow_
                                ->size()),
                        usageWindow_->size(),
                        screen
                            ->availableGeometry());
        } else {
            origin =
                PetMenuPlacement::
                    positionedOrigin(
                        anchorFrame,
                        usageWindow_->size(),
                        screen
                            ->availableGeometry());
        }
        windowCoordinator_->move(
            WindowRole::Usage,
            origin);
    }
}

void CompanionApplication::positionAttentionWindow()
{
    if (petWindow_.isNull()
        || attentionWindow_.isNull()) {
        return;
    }

    const QRect petFrame(
        petWindow_->position(),
        petWindow_->size());
    QScreen* screen =
        QGuiApplication::screenAt(
            petFrame.center());
    if (screen == nullptr) {
        screen =
            QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }

    windowCoordinator_->move(
        WindowRole::Attention,
        PetMenuPlacement::
            positionedAttentionOrigin(
                petFrame,
                attentionWindow_->size(),
                screen->availableGeometry()));
}

void CompanionApplication::syncAttentionWindow()
{
    if (!petProcessReactionController_
        || !petViewModel_
        || attentionWindow_.isNull()) {
        return;
    }

    const bool shouldShow =
        petProcessReactionController_->
            hasAttention()
        && petViewModel_->visible()
        && !petViewModel_->menuOpen();
    if (!shouldShow) {
        windowCoordinator_->hide(
            WindowRole::Attention);
        return;
    }

    positionAttentionWindow();
    windowCoordinator_->show(
        WindowRole::Attention);
}

void CompanionApplication::syncPetWindowVisibility()
{
    if (!petViewModel_) {
        return;
    }

    if (petViewModel_->visible()) {
        windowCoordinator_->show(
            WindowRole::Pet);
    } else {
        windowCoordinator_->hide(
            WindowRole::Pet);
        if (petViewModel_->menuOpen()) {
            petViewModel_->setMenuOpen(false);
        } else {
            windowCoordinator_->hide(
                WindowRole::CompanionMenu);
        }
        closeUsage();
    }
    syncAttentionWindow();
    refreshTrayRouteState();
}

void CompanionApplication::syncProcessSurfaceVisibility()
{
    if (companionMenuWindow_.isNull()
        || !shellViewModel_) {
        return;
    }

    const bool visible =
        companionMenuWindow_->isVisible()
        && shellViewModel_->routeMode()
            == QStringLiteral("processes");
    if (!visible) {
        lastCompanionProcessHoverCursor_.reset();
    }
    if (runtimeHost_) {
        runtimeHost_->setProcessSurfaceVisible(
            visible);
    }
    if (petProcessReactionController_) {
        petProcessReactionController_->
            setProcessSurfaceVisible(visible);
    }
}

void CompanionApplication::connectWindowCoordinator()
{
    connect(windowCoordinator_.get(),
            &WindowCoordinator::runtimeErrorOccurred,
            this,
            &CompanionApplication::reportRuntimeError);
    connect(this,
            &CompanionApplication::settingsShowRequested,
            this,
            [this]() {
                positionSettingsWindowForShow();
                const auto shown = windowCoordinator_->show(WindowRole::Settings);
                if (shown.hasValue()) {
                    windowCoordinator_->activate(WindowRole::Settings);
                }
                refreshTrayRouteState();
            });
    connect(this,
            &CompanionApplication::settingsCloseRequested,
            this,
            [this]() {
                windowCoordinator_->hide(WindowRole::Settings);
                refreshTrayRouteState();
            });
}

void CompanionApplication::positionSettingsWindowForShow()
{
    if (settingsWindow_.isNull()) {
        return;
    }

    const QRect petFrame = petWindow_.isNull()
        ? QRect()
        : petWindow_->frameGeometry();
    if (settingsWindowPositionInitialized_
        && (!petFrame.isValid()
            || !settingsWindow_->frameGeometry()
                    .intersects(petFrame))) {
        return;
    }
    const QPoint referencePoint = petFrame.isValid()
        ? petFrame.center()
        : QCursor::pos();
    QRect availableWorkArea =
        petWindowController_
        ? petWindowController_->availableWorkAreaAt(
              referencePoint)
        : QRect();
    if (!availableWorkArea.isValid()) {
        QScreen* screen =
            QGuiApplication::screenAt(referencePoint);
        if (screen == nullptr) {
            screen = QGuiApplication::primaryScreen();
        }
        if (screen != nullptr) {
            availableWorkArea =
                screen->availableGeometry();
        }
    }
    if (!availableWorkArea.isValid()) {
        return;
    }

    QSize settingsFrameSize =
        settingsWindow_->frameGeometry().size();
    if (!settingsFrameSize.isValid()
        || settingsFrameSize.isEmpty()) {
        settingsFrameSize = settingsWindow_->size();
    }
    if (!settingsFrameSize.isValid()
        || settingsFrameSize.isEmpty()) {
        return;
    }

    settingsWindow_->setFramePosition(
        CompanionPresentationPolicy::
            settingsWindowOrigin(
                availableWorkArea,
                settingsFrameSize,
                petFrame));
    settingsWindowPositionInitialized_ = true;
}

void CompanionApplication::refreshTrayRouteState()
{
    if (trayIconHost_) {
        trayIconHost_->setRouteState(windowCoordinator_->trayRouteState());
    }
}

void CompanionApplication::reportRuntimeError(const CompanionError& error)
{
    emit runtimeErrorOccurred(error);
}

bool CompanionApplication::eventFilter(
    QObject* watched,
    QEvent* event)
{
    const bool isCompanionUtility =
        watched == companionMenuWindow_
        || watched == usageWindow_;
    if (isCompanionUtility) {
        if (event->type() == QEvent::Enter) {
            setCompanionUtilityPointerHovered(
                true);
        } else if (event->type()
                   == QEvent::Leave) {
            setCompanionUtilityPointerHovered(
                false);
        }
    }

    return QObject::eventFilter(
        watched,
        event);
}

bool CompanionApplication::explicitQuitRequested() const noexcept
{
    return explicitQuit_;
}

} // namespace companion
