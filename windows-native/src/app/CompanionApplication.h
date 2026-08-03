#pragma once

#include "core/AppSettings.h"
#include "core/Result.h"
#include "core/SettingsRepository.h"
#include "platform/windows/WindowCoordinator.h"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

class QCoreApplication;
class QQmlApplicationEngine;
class QQuickWindow;
class QTimer;
class QVariantAnimation;

namespace companion {

namespace detail {
class CompanionApplicationTestAccess;
}

class SingleInstanceGate;
class CompanionRuntimeHost;
class CompanionShellViewModel;
class CodexAccountLoginService;
class CodexAccountProfileStore;
class CodexAccountRouter;
class CodexAccountRuntime;
struct CodexEnvironment;
class CodexThreadAccountBindingStore;
class CredentialStore;
class MobilePresencePetCatalogService;
struct MobilePresencePetCatalogSnapshot;
class PetCatalog;
class PetProcessReactionController;
class PetViewModel;
class PetWindowController;
class SettingsViewModel;
class TrayIconHost;
class UpdateService;
class UpdateViewModel;
class WindowBackdropState;
class WindowsPowerAvailabilityController;

class CompanionApplication final : public QObject {
    Q_OBJECT

public:
    explicit CompanionApplication(QCoreApplication& application);
    CompanionApplication(
        QCoreApplication& application,
        QString instanceName,
        QString settingsFilePath);
    CompanionApplication(
        QCoreApplication& application,
        QString instanceName,
        QString settingsFilePath,
        std::unique_ptr<WindowCoordinator> windowCoordinator,
        QString postUpdateAcknowledgementRequestId = {});
    ~CompanionApplication() override;

    static void configureStandardPathsForLaunch();
    static Result<QString> defaultSettingsFilePath();

    Result<void> start();
public slots:
    void showSettings();
    void closeSettings();
    void closeCompanionMenu();
    void closeUsage();
    void showProcessesFromPet();
    void showChatFromPet();
    void showModelPickerFromPet();
    void showUsageFromPet();
    void openAttentionFromPet();
    void dismissAttention();
    void hidePetFromWindow();
public:
    void quitExplicitly();
    bool explicitQuitRequested() const noexcept;

signals:
    void settingsShowRequested();
    void settingsCloseRequested();
    void runtimeErrorOccurred(CompanionError error);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void registerModelPickerWindow();
    void registerGoalWindow();

private:
    Result<void> initializeQmlEngine();
    Result<void> initializeTrayHost();
    Result<void> initializeRuntimeServices();
    Result<void> initializeCodexAccountServices(
        const CodexEnvironment& environment,
        QString codexExecutable);
    Result<void>
    signalPostUpdateAcknowledgementIfRequested();
    Result<void>
    applyIsolatedTestStartupRoute();
    void connectWindowCoordinator();
    void showCompanionMenu();
    void showCompanionMenuNearPet();
    std::optional<QPoint>
    companionMenuOriginForControls(
        bool controlsVisible) const;
    void
    scheduleCompanionMenuGeometryReconciliation();
    void positionCompanionMenuNearPet(
        bool animated = false,
        std::optional<bool>
            controlsVisibleOverride =
                std::nullopt,
        bool reconcileHover = true);
    void animateCompanionMenuOrigin(
        QPoint targetOrigin);
    void stopCompanionMenuOriginAnimation();
    void freezeCompanionUtilityAnchor();
    void reconcileCompanionProcessHover(
        bool force = false);
    void reconcileCompanionUtilityPointerHover();
    void setCompanionUtilityPointerHovered(
        bool hovered);
    bool companionUtilityContainsCursor() const;
    void showUsageNearPet();
    void positionUsageNearPet();
    void positionAttentionWindow();
    void positionSettingsWindowForShow();
    void syncAttentionWindow();
    void syncPetWindowVisibility();
    void syncProcessSurfaceVisibility();
    void refreshTrayRouteState();
    void refreshChatAccentColor();
    void publishMobilePresencePetSnapshot();
    MobilePresencePetCatalogSnapshot
    mobilePresencePetSnapshot() const;
    void reloadAndApplyMobileSettings();
    void reportRuntimeError(const CompanionError& error);
    void destroyCompanionPopupWindows() noexcept;
    Result<void> registerCompanionPopupWindow(
        const char* propertyName,
        WindowRole role,
        qreal radius,
        QPointer<QQuickWindow>& destination);

    QCoreApplication& application_;
    QString instanceName_;
    std::unique_ptr<SingleInstanceGate> singleInstanceGate_;
    SettingsRepository settingsRepository_;
    AppSettings loadedSettings_;
    std::unique_ptr<CompanionShellViewModel> shellViewModel_;
    std::unique_ptr<PetCatalog> petCatalog_;
    std::unique_ptr<PetViewModel> petViewModel_;
    std::unique_ptr<PetProcessReactionController>
        petProcessReactionController_;
    std::unique_ptr<PetWindowController>
        petWindowController_;
    std::unique_ptr<
        CodexAccountProfileStore>
        codexAccountProfileStore_;
    std::unique_ptr<
        CodexThreadAccountBindingStore>
        codexThreadAccountBindingStore_;
    std::unique_ptr<
        CodexAccountRuntime>
        codexAccountRuntime_;
    std::unique_ptr<
        CodexAccountRouter>
        codexAccountRouter_;
    std::unique_ptr<
        CodexAccountLoginService>
        codexAccountLoginService_;
    std::unique_ptr<SettingsViewModel> settingsViewModel_;
    std::unique_ptr<WindowBackdropState> windowBackdropState_;
    std::unique_ptr<UpdateService> updateService_;
    std::unique_ptr<UpdateViewModel> updateViewModel_;
    std::shared_ptr<CredentialStore>
        credentialStore_;
    std::shared_ptr<
        MobilePresencePetCatalogService>
        mobilePresencePetCatalogService_;
    std::unique_ptr<QQmlApplicationEngine> qmlEngine_;
    std::unique_ptr<WindowCoordinator> windowCoordinator_;
    std::unique_ptr<QVariantAnimation>
        companionMenuOriginAnimation_;
    std::unique_ptr<QTimer>
        companionUtilityHoverTimer_;
    std::function<bool()>
        companionUtilityCursorPresenceSource_;
    std::function<QPoint()>
        companionProcessCursorPositionSource_;
    std::unique_ptr<QTimer>
        chatAccentRefreshTimer_;
    std::unique_ptr<TrayIconHost> trayIconHost_;
    std::unique_ptr<CompanionRuntimeHost> runtimeHost_;
    std::unique_ptr<
        WindowsPowerAvailabilityController>
        powerAvailabilityController_;
    QPointer<QQuickWindow> petWindow_;
    QPointer<QQuickWindow> settingsWindow_;
    QPointer<QQuickWindow> companionMenuWindow_;
    QPointer<QQuickWindow> modelPickerWindow_;
    QPointer<QQuickWindow> goalWindow_;
    QPointer<QQuickWindow> usageWindow_;
    QPointer<QQuickWindow> attentionWindow_;
    QPoint companionMenuOriginAnimationStart_;
    QPoint companionMenuOriginAnimationTarget_;
    std::optional<bool>
        companionMenuAnchorControlsVisible_;
    std::optional<QPoint>
        lastCompanionProcessHoverCursor_;
    quint64 companionMenuOriginAnimationGeneration_ = 0;
    quint64
        companionMenuGeometryReconciliationCount_ = 0;
    std::optional<CompanionError> startupError_;
    QString
        postUpdateAcknowledgementRequestId_;
    std::optional<QUrl>
        updateManifestUrlOverride_;
    bool companionUtilityPointerHovered_ = false;
    bool
        companionMenuGeometryReconciliationPending_ =
            false;
    bool productionRuntimeEnabled_ = false;
    bool explicitQuit_ = false;
    bool settingsWindowPositionInitialized_ = false;

    friend class detail::CompanionApplicationTestAccess;
};

} // namespace companion
