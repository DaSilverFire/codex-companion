#pragma once

#include "codex/accounts/CodexAccountLoginService.h"
#include "core/AppSettings.h"
#include "core/ChatCredentialService.h"
#include "core/CompanionError.h"
#include "core/SettingsRepository.h"
#include "mobile/relay/RelaySettings.h"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <memory>

namespace companion {

class PairingCoordinator;
class RelayPairingBootstrap;
class CodexAccountProfileStore;
class CodexAccountRouter;

class SettingsViewModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString backdropMode READ backdropMode WRITE setBackdropMode NOTIFY backdropModeChanged)
    Q_PROPERTY(QString effectiveBackdropMode READ effectiveBackdropMode NOTIFY effectiveBackdropModeChanged)
    Q_PROPERTY(double animationSpeedScale READ animationSpeedScale WRITE setAnimationSpeedScale NOTIFY animationSpeedScaleChanged)
    Q_PROPERTY(bool hideControlsUntilHover READ hideControlsUntilHover WRITE setHideControlsUntilHover NOTIFY hideControlsUntilHoverChanged)
    Q_PROPERTY(bool allowAutonomousMovement READ allowAutonomousMovement WRITE setAllowAutonomousMovement NOTIFY allowAutonomousMovementChanged)
    Q_PROPERTY(bool hasOpenAIAPIKey READ hasOpenAIAPIKey NOTIFY openAICredentialChanged)
    Q_PROPERTY(QString openAIAPIKeyStatus READ openAIAPIKeyStatus NOTIFY openAICredentialChanged)
    Q_PROPERTY(bool hasLumoAPIKey READ hasLumoAPIKey NOTIFY lumoCredentialChanged)
    Q_PROPERTY(QString lumoAPIKeyStatus READ lumoAPIKeyStatus NOTIFY lumoCredentialChanged)
    Q_PROPERTY(bool codexAccountsAvailable READ codexAccountsAvailable NOTIFY codexAccountsChanged)
    Q_PROPERTY(QVariantList codexAccountProfiles READ codexAccountProfiles NOTIFY codexAccountsChanged)
    Q_PROPERTY(QString selectedCodexAccountProfileId READ selectedCodexAccountProfileId WRITE setSelectedCodexAccountProfileId NOTIFY codexAccountsChanged)
    Q_PROPERTY(QString codexAccountSelectionSummary READ codexAccountSelectionSummary NOTIFY codexAccountsChanged)
    Q_PROPERTY(QString codexAccountStatus READ codexAccountStatus NOTIFY codexAccountsChanged)
    Q_PROPERTY(bool codexAccountRefreshInProgress READ codexAccountRefreshInProgress NOTIFY codexAccountsChanged)
    Q_PROPERTY(bool automaticallyContinuesAcrossCodexAccounts READ automaticallyContinuesAcrossCodexAccounts WRITE setAutomaticallyContinuesAcrossCodexAccounts NOTIFY automaticallyContinuesAcrossCodexAccountsChanged)
    Q_PROPERTY(bool mobileEnabled READ mobileEnabled WRITE setMobileEnabled NOTIFY mobileEnabledChanged)
    Q_PROPERTY(bool keepAvailableWhileDisplayOff READ keepAvailableWhileDisplayOff WRITE setKeepAvailableWhileDisplayOff NOTIFY keepAvailableWhileDisplayOffChanged)
    Q_PROPERTY(bool allowNearbyOnPublicNetworks READ allowNearbyOnPublicNetworks WRITE setAllowNearbyOnPublicNetworks NOTIFY allowNearbyOnPublicNetworksChanged)
    Q_PROPERTY(bool mobilePairingAvailable READ mobilePairingAvailable NOTIFY mobilePairingChanged)
    Q_PROPERTY(bool mobilePairingActive READ mobilePairingActive NOTIFY mobilePairingChanged)
    Q_PROPERTY(QString mobilePairingCode READ mobilePairingCode NOTIFY mobilePairingChanged)
    Q_PROPERTY(qint64 mobilePairingExpiresAtMilliseconds READ mobilePairingExpiresAtMilliseconds NOTIFY mobilePairingChanged)
    Q_PROPERTY(QString mobilePairingLink READ mobilePairingLink NOTIFY mobilePairingChanged)
    Q_PROPERTY(QString mobilePairingQrSource READ mobilePairingQrSource NOTIFY mobilePairingChanged)
    Q_PROPERTY(bool hasMobilePairingLink READ hasMobilePairingLink NOTIFY mobilePairingChanged)
    Q_PROPERTY(QString mobilePairingLinkStatus READ mobilePairingLinkStatus NOTIFY mobilePairingChanged)
    Q_PROPERTY(QString mobilePairingStatusText READ mobilePairingStatusText NOTIFY mobilePairingChanged)
    Q_PROPERTY(QVariantList pairedMobileDevices READ pairedMobileDevices NOTIFY mobilePairingChanged)
    Q_PROPERTY(bool mobileNearbyAccessAvailable READ mobileNearbyAccessAvailable NOTIFY mobileNearbyAccessChanged)
    Q_PROPERTY(QString mobileNearbyAccessStatusText READ mobileNearbyAccessStatusText NOTIFY mobileNearbyAccessChanged)
    Q_PROPERTY(QString relayUrl READ relayUrl NOTIFY relayConfigurationChanged)
    Q_PROPERTY(QString relayStatusText READ relayStatusText NOTIFY relayConfigurationChanged)
    Q_PROPERTY(bool relayAutomaticAvailable READ relayAutomaticAvailable NOTIFY relayConfigurationChanged)
    Q_PROPERTY(bool relayDisableAvailable READ relayDisableAvailable NOTIFY relayConfigurationChanged)

public:
    using MaterialReapplyCommand = std::function<Result<BackdropMode>(BackdropMode)>;
    using MobileAvailabilityCommand =
        std::function<Result<void>(
            bool mobileEnabled,
            bool keepAvailableWhileDisplayOff)>;
    using CopyTextCommand =
        std::function<Result<void>(
            const QString& value)>;

    SettingsViewModel(
        AppSettings loadedSettings,
        const SettingsRepository& settingsRepository,
        MaterialReapplyCommand reapplyMaterial,
        QObject* parent = nullptr);
    SettingsViewModel(
        AppSettings loadedSettings,
        const SettingsRepository& settingsRepository,
        MaterialReapplyCommand reapplyMaterial,
        std::shared_ptr<CredentialStore> credentialStore,
        QObject* parent = nullptr);
    ~SettingsViewModel() override;

    QString backdropMode() const;
    void setBackdropMode(const QString& value);
    QString effectiveBackdropMode() const;
    void setEffectiveBackdropMode(BackdropMode mode);

    double animationSpeedScale() const noexcept;
    void setAnimationSpeedScale(double value);

    bool hideControlsUntilHover() const noexcept;
    void setHideControlsUntilHover(bool value);

    bool allowAutonomousMovement() const noexcept;
    void setAllowAutonomousMovement(bool value);

    bool hasOpenAIAPIKey() const noexcept;
    QString openAIAPIKeyStatus() const;
    bool hasLumoAPIKey() const noexcept;
    QString lumoAPIKeyStatus() const;

    bool codexAccountsAvailable() const noexcept;
    QVariantList codexAccountProfiles() const;
    QString selectedCodexAccountProfileId() const;
    void setSelectedCodexAccountProfileId(
        const QString& value);
    QString codexAccountSelectionSummary() const;
    QString codexAccountStatus() const;
    bool codexAccountRefreshInProgress() const
        noexcept;
    bool automaticallyContinuesAcrossCodexAccounts()
        const noexcept;
    void setAutomaticallyContinuesAcrossCodexAccounts(
        bool value);
    void setCodexAccountServices(
        CodexAccountProfileStore* profileStore,
        CodexAccountRouter* router,
        CodexAccountLoginService* loginService);

    bool mobileEnabled() const noexcept;
    void setMobileEnabled(bool value);
    bool keepAvailableWhileDisplayOff() const
        noexcept;
    void setKeepAvailableWhileDisplayOff(
        bool value);
    bool allowNearbyOnPublicNetworks() const
        noexcept;
    void setAllowNearbyOnPublicNetworks(
        bool value);

    bool mobilePairingAvailable() const
        noexcept;
    bool mobilePairingActive() const noexcept;
    QString mobilePairingCode() const;
    qint64
    mobilePairingExpiresAtMilliseconds() const
        noexcept;
    QString mobilePairingLink() const;
    QString mobilePairingQrSource() const;
    bool hasMobilePairingLink() const
        noexcept;
    QString mobilePairingLinkStatus() const;
    QString mobilePairingStatusText() const;
    QVariantList pairedMobileDevices() const;
    bool mobileNearbyAccessAvailable() const
        noexcept;
    QString mobileNearbyAccessStatusText()
        const;
    void setMobilePairingCoordinator(
        PairingCoordinator* coordinator);
    void setRelayPairingBootstrap(
        RelayPairingBootstrap* bootstrap);
    void setMobileNearbyAccessState(
        bool available,
        QString statusText);
    void setMobileAvailabilityCommand(
        MobileAvailabilityCommand command);
    void setCopyMobilePairingLinkCommand(
        CopyTextCommand command);

    QString relayUrl() const;
    QString relayStatusText() const;
    bool relayAutomaticAvailable() const;
    bool relayDisableAvailable() const;

    Q_INVOKABLE bool saveOpenAIAPIKey(
        const QString& secret);
    Q_INVOKABLE bool removeOpenAIAPIKey();
    Q_INVOKABLE bool saveLumoAPIKey(
        const QString& secret);
    Q_INVOKABLE bool removeLumoAPIKey();
    Q_INVOKABLE bool addCodexAccount(
        const QString& label);
    Q_INVOKABLE bool removeSelectedCodexAccount();
    Q_INVOKABLE bool
    beginSelectedCodexAccountLogin();
    Q_INVOKABLE bool
    refreshSelectedCodexAccount();
    Q_INVOKABLE bool beginMobilePairing();
    Q_INVOKABLE void cancelMobilePairing();
    Q_INVOKABLE bool copyMobilePairingLink();
    Q_INVOKABLE bool forgetMobileDevice(
        const QString& deviceId);
    Q_INVOKABLE bool saveRelayUrl(
        const QString& value);
    Q_INVOKABLE bool useAutomaticRelay();
    Q_INVOKABLE bool disableRelay();

signals:
    void backdropModeChanged();
    void effectiveBackdropModeChanged();
    void animationSpeedScaleChanged();
    void hideControlsUntilHoverChanged();
    void allowAutonomousMovementChanged();
    void openAICredentialChanged();
    void lumoCredentialChanged();
    void chatCredentialsChanged();
    void codexAccountsChanged();
    void
    automaticallyContinuesAcrossCodexAccountsChanged();
    void mobileEnabledChanged();
    void keepAvailableWhileDisplayOffChanged();
    void allowNearbyOnPublicNetworksChanged();
    void mobilePairingChanged();
    void mobileNearbyAccessChanged();
    void relayConfigurationChanged();
    void mobileConfigurationChanged();
    void closeRequested();
    void runtimeErrorOccurred(companion::CompanionError error);

private:
    using SettingsMutation = std::function<void(AppSettings&)>;

    bool saveAndReload(SettingsMutation mutation);
    void publishSettings(const AppSettings& settings);
    bool saveCredential(
        ChatCredentialKind kind,
        const QString& secret);
    bool removeCredential(
        ChatCredentialKind kind);
    void publishCredentialStatus(
        ChatCredentialKind kind,
        bool available,
        QString status);
    void refreshCodexAccountState();
    void finishCodexAccountRefresh();
    void setCodexAccountStatus(QString status);
    std::optional<CodexAccountProfile>
    selectedCodexAccountProfile() const;
    void refreshMobilePairingState();
    bool applyMobileAvailability(
        const AppSettings& previousSettings);
    QString configuredRelayUrl() const;
    QString defaultRelayStatusText() const;
    void reportRuntimeError(const CompanionError& error);

    AppSettings settings_;
    BackdropMode effectiveBackdropMode_ = BackdropMode::Mica;
    const SettingsRepository& settingsRepository_;
    MaterialReapplyCommand reapplyMaterial_;
    std::shared_ptr<CredentialStore>
        credentialStore_;
    bool hasOpenAIAPIKey_ = false;
    QString openAIAPIKeyStatus_;
    bool hasLumoAPIKey_ = false;
    QString lumoAPIKeyStatus_;
    CodexAccountProfileStore*
        codexAccountProfileStore_ = nullptr;
    CodexAccountRouter*
        codexAccountRouter_ = nullptr;
    CodexAccountLoginService*
        codexAccountLoginService_ = nullptr;
    QVariantList codexAccountProfiles_;
    QString selectedCodexAccountProfileId_;
    QString codexAccountStatus_ =
        QStringLiteral(
            "Add a Codex account profile to get started.");
    bool codexAccountRefreshInProgress_ =
        false;
    QFutureWatcher<
        Result<
            CodexAccountAuthenticationStatus>>
        codexAccountStatusWatcher_;
    RelaySettings relaySettings_;
    QString relayStatusText_;
    QPointer<PairingCoordinator>
        pairingCoordinator_;
    QPointer<RelayPairingBootstrap>
        relayPairingBootstrap_;
    QTimer pairingRefreshTimer_;
    QString mobilePairingCode_;
    QString mobilePairingLink_;
    QString mobilePairingQrSource_;
    QString mobilePairingLinkStatus_;
    QString mobilePairingStatusText_ =
        QStringLiteral(
            "Mobile pairing security is unavailable in this process.");
    QString mobilePairingCommandError_;
    qint64
        mobilePairingExpiresAtMilliseconds_ =
            0;
    QVariantList pairedMobileDevices_;
    bool mobilePairingAvailable_ = false;
    bool mobileNearbyAccessAvailable_ =
        false;
    QString mobileNearbyAccessStatusText_ =
        QStringLiteral(
            "Nearby Wi-Fi status is loading.");
    bool refreshingMobilePairing_ = false;
    MobileAvailabilityCommand
        mobileAvailabilityCommand_;
    CopyTextCommand
        copyMobilePairingLinkCommand_;
};

} // namespace companion
