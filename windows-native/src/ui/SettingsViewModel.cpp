#include "ui/SettingsViewModel.h"

#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "mobile/relay/RelayPairingBootstrap.h"
#include "mobile/security/PairingCoordinator.h"
#include "ui/PairingQrCode.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaType>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {

using companion::AppSettings;
using companion::BackdropMode;
using companion::CompanionError;

const QString& currentCodexAccountProfileId()
{
    static const QString value =
        QStringLiteral(
            "current-codex-account");
    return value;
}

QString backdropName(BackdropMode mode)
{
    switch (mode) {
    case BackdropMode::Mica:
        return QStringLiteral("mica");
    case BackdropMode::WindowsGlass:
        return QStringLiteral("windows-glass");
    case BackdropMode::SolidBlack:
        return QStringLiteral("solid-black");
    }
    return QStringLiteral("solid-black");
}

bool parseBackdropName(const QString& value, BackdropMode& mode)
{
    if (value == QStringLiteral("mica")) {
        mode = BackdropMode::Mica;
        return true;
    }
    if (value == QStringLiteral("windows-glass")) {
        mode = BackdropMode::WindowsGlass;
        return true;
    }
    if (value == QStringLiteral("solid-black")) {
        mode = BackdropMode::SolidBlack;
        return true;
    }
    return false;
}

CompanionError settingsCommandError(QString code, QString message, QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

} // namespace

namespace companion {

SettingsViewModel::SettingsViewModel(
    AppSettings loadedSettings,
    const SettingsRepository& settingsRepository,
    MaterialReapplyCommand reapplyMaterial,
    QObject* parent)
    : SettingsViewModel(
          std::move(loadedSettings),
          settingsRepository,
          std::move(reapplyMaterial),
          {},
          parent)
{
}

SettingsViewModel::SettingsViewModel(
    AppSettings loadedSettings,
    const SettingsRepository& settingsRepository,
    MaterialReapplyCommand reapplyMaterial,
    std::shared_ptr<CredentialStore>
        credentialStore,
    QObject* parent)
    : QObject(parent),
      settings_(std::move(loadedSettings)),
      effectiveBackdropMode_(settings_.backdrop),
      settingsRepository_(settingsRepository),
      reapplyMaterial_(std::move(reapplyMaterial)),
      credentialStore_(
          std::move(credentialStore)),
      relaySettings_(
          RelaySettings::
              fromBundledConfiguration())
{
    qRegisterMetaType<CompanionError>("companion::CompanionError");
    hasOpenAIAPIKey_ = credentialStore_
        && ChatCredentialService::
            hasUsableCredential(
                *credentialStore_,
                ChatCredentialKind::OpenAI);
    openAIAPIKeyStatus_ = hasOpenAIAPIKey_
        ? QStringLiteral(
              "OpenAI API key saved for this Windows PC.")
        : QStringLiteral(
              "No OpenAI API key saved.");
    hasLumoAPIKey_ = credentialStore_
        && ChatCredentialService::
            hasUsableCredential(
                *credentialStore_,
                ChatCredentialKind::Lumo);
    lumoAPIKeyStatus_ = hasLumoAPIKey_
        ? QStringLiteral(
              "Lumo API key saved for this Windows PC.")
        : QStringLiteral(
              "No Lumo API key saved.");
    relayStatusText_ =
        defaultRelayStatusText();
    pairingRefreshTimer_.setInterval(1000);
    pairingRefreshTimer_.setTimerType(
        Qt::VeryCoarseTimer);
    connect(
        &pairingRefreshTimer_,
        &QTimer::timeout,
        this,
        &SettingsViewModel::
            refreshMobilePairingState);
    connect(
        &codexAccountStatusWatcher_,
        &QFutureWatcherBase::finished,
        this,
        &SettingsViewModel::
            finishCodexAccountRefresh);
    copyMobilePairingLinkCommand_ =
        [](const QString& value) {
            auto* application =
                qobject_cast<QGuiApplication*>(
                    QCoreApplication::instance());
            QClipboard* clipboard =
                application
                ? QGuiApplication::clipboard()
                : nullptr;
            if (!clipboard) {
                return Result<void>::failure(
                    settingsCommandError(
                        QStringLiteral(
                            "clipboard.unavailable"),
                        QStringLiteral(
                            "The Windows clipboard is unavailable.")));
            }
            clipboard->setText(
                value,
                QClipboard::Clipboard);
            return Result<void>::success();
        };
}

SettingsViewModel::~SettingsViewModel()
{
    codexAccountStatusWatcher_.cancel();
    codexAccountStatusWatcher_
        .waitForFinished();
}

QString SettingsViewModel::backdropMode() const
{
    return backdropName(settings_.backdrop);
}

QString SettingsViewModel::effectiveBackdropMode() const
{
    return backdropName(effectiveBackdropMode_);
}

void SettingsViewModel::setEffectiveBackdropMode(BackdropMode mode)
{
    if (effectiveBackdropMode_ == mode) {
        return;
    }

    effectiveBackdropMode_ = mode;
    emit effectiveBackdropModeChanged();
}

void SettingsViewModel::setBackdropMode(const QString& value)
{
    BackdropMode requested = BackdropMode::SolidBlack;
    if (!parseBackdropName(value, requested)) {
        reportRuntimeError(settingsCommandError(
            QStringLiteral("settings.backdrop-mode-invalid"),
            QStringLiteral("The requested Companion material is not supported."),
            {{QStringLiteral("backdropMode"), value}}));
        return;
    }

    if (settings_.backdrop == requested) {
        return;
    }

    const BackdropMode previousBackdrop =
        settings_.backdrop;
    if (!saveAndReload([requested](AppSettings& settings) {
            settings.backdrop = requested;
        })) {
        return;
    }

    const auto reapplied = reapplyMaterial_
        ? reapplyMaterial_(settings_.backdrop)
        : Result<BackdropMode>::success(settings_.backdrop);
    if (!reapplied.hasValue()) {
        const auto rolledBack =
            settingsRepository_.update(
                [previousBackdrop](
                    AppSettings& settings) {
                    settings.backdrop =
                        previousBackdrop;
                });
        if (rolledBack.hasValue()) {
            publishSettings(
                rolledBack.value());
        } else {
            reportRuntimeError(rolledBack.error());
        }
        reportRuntimeError(reapplied.error());
        return;
    }

    setEffectiveBackdropMode(reapplied.value());
}

double SettingsViewModel::animationSpeedScale() const noexcept
{
    return settings_.animationSpeedScale;
}

void SettingsViewModel::setAnimationSpeedScale(double value)
{
    if (qFuzzyCompare(settings_.animationSpeedScale, value)) {
        return;
    }

    saveAndReload([value](AppSettings& settings) {
        settings.animationSpeedScale = value;
    });
}

bool SettingsViewModel::hideControlsUntilHover() const noexcept
{
    return settings_.hideControlsUntilHover;
}

void SettingsViewModel::setHideControlsUntilHover(bool value)
{
    if (settings_.hideControlsUntilHover == value) {
        return;
    }

    saveAndReload([value](AppSettings& settings) {
        settings.hideControlsUntilHover = value;
    });
}

bool SettingsViewModel::allowAutonomousMovement() const noexcept
{
    return settings_.allowAutonomousMovement;
}

void SettingsViewModel::setAllowAutonomousMovement(bool value)
{
    if (settings_.allowAutonomousMovement == value) {
        return;
    }

    saveAndReload([value](AppSettings& settings) {
        settings.allowAutonomousMovement = value;
    });
}

bool SettingsViewModel::hasOpenAIAPIKey() const
    noexcept
{
    return hasOpenAIAPIKey_;
}

QString SettingsViewModel::openAIAPIKeyStatus()
    const
{
    return openAIAPIKeyStatus_;
}

bool SettingsViewModel::hasLumoAPIKey() const
    noexcept
{
    return hasLumoAPIKey_;
}

QString SettingsViewModel::lumoAPIKeyStatus()
    const
{
    return lumoAPIKeyStatus_;
}

bool SettingsViewModel::codexAccountsAvailable()
    const noexcept
{
    return codexAccountProfileStore_ != nullptr
        && codexAccountRouter_ != nullptr
        && codexAccountLoginService_ != nullptr;
}

QVariantList
SettingsViewModel::codexAccountProfiles() const
{
    return codexAccountProfiles_;
}

QString SettingsViewModel::
selectedCodexAccountProfileId() const
{
    return selectedCodexAccountProfileId_;
}

void SettingsViewModel::
setSelectedCodexAccountProfileId(
    const QString& value)
{
    if (!codexAccountProfileStore_) {
        return;
    }
    if (value == currentCodexAccountProfileId()) {
        const auto selected =
            codexAccountProfileStore_
                ->selectCurrentAccount();
        if (!selected.hasValue()) {
            setCodexAccountStatus(
                selected.error().message);
            reportRuntimeError(
                selected.error());
            return;
        }
        setCodexAccountStatus(
            QStringLiteral(
                "The current Codex account is selected for new Codex work."));
        refreshCodexAccountState();
        return;
    }
    const auto profileId =
        parseCodexAccountProfileId(value);
    if (!profileId.has_value()) {
        setCodexAccountStatus(
            QStringLiteral(
                "Select a valid Codex account profile."));
        return;
    }
    const auto selected =
        codexAccountProfileStore_->select(
            *profileId);
    if (!selected.hasValue()) {
        setCodexAccountStatus(
            selected.error().message);
        reportRuntimeError(
            selected.error());
        return;
    }
    setCodexAccountStatus(
        QStringLiteral(
            "%1 is selected for new Codex work.")
            .arg(
                codexAccountProfileStore_
                    ->profile(*profileId)
                    .value_or(
                        CodexAccountProfile{
                            *profileId,
                            QStringLiteral(
                                "This profile"),
                        })
                    .label));
    refreshCodexAccountState();
}

QString SettingsViewModel::
codexAccountSelectionSummary() const
{
    const auto selected =
        selectedCodexAccountProfile();
    if (!selected.has_value()) {
        return QStringLiteral(
            "Current Codex account applies to new Codex work. Active and approval-pending tasks stay with their original account.");
    }
    return QStringLiteral(
        "%1 applies to new Codex work. Active and approval-pending tasks stay with their original account.")
        .arg(selected->label);
}

QString SettingsViewModel::
codexAccountStatus() const
{
    return codexAccountStatus_;
}

bool SettingsViewModel::
codexAccountRefreshInProgress() const
    noexcept
{
    return codexAccountRefreshInProgress_;
}

bool SettingsViewModel::
automaticallyContinuesAcrossCodexAccounts()
    const noexcept
{
    return settings_
        .automaticallyContinuesAcrossCodexAccounts;
}

void SettingsViewModel::
setAutomaticallyContinuesAcrossCodexAccounts(
    bool value)
{
    if (settings_
            .automaticallyContinuesAcrossCodexAccounts
        == value) {
        return;
    }
    (void)saveAndReload(
        [value](AppSettings& settings) {
            settings
                .automaticallyContinuesAcrossCodexAccounts =
                value;
        });
}

void SettingsViewModel::
setCodexAccountServices(
    CodexAccountProfileStore* profileStore,
    CodexAccountRouter* router,
    CodexAccountLoginService* loginService)
{
    codexAccountProfileStore_ =
        profileStore;
    codexAccountRouter_ = router;
    codexAccountLoginService_ =
        loginService;
    refreshCodexAccountState();
}

bool SettingsViewModel::mobileEnabled() const
    noexcept
{
    return settings_.mobileEnabled;
}

void SettingsViewModel::setMobileEnabled(
    bool value)
{
    if (settings_.mobileEnabled == value) {
        return;
    }
    const AppSettings previous =
        settings_;
    if (!saveAndReload(
            [value](AppSettings& settings) {
                settings.mobileEnabled = value;
            })
        || !applyMobileAvailability(
            previous)) {
        return;
    }
    if (!value) {
        cancelMobilePairing();
    }
}

bool SettingsViewModel::
keepAvailableWhileDisplayOff() const noexcept
{
    return settings_
        .keepAvailableWhileDisplayOff;
}

void SettingsViewModel::
setKeepAvailableWhileDisplayOff(bool value)
{
    if (settings_
            .keepAvailableWhileDisplayOff
        == value) {
        return;
    }
    const AppSettings previous =
        settings_;
    if (!saveAndReload(
        [value](AppSettings& settings) {
            settings
                .keepAvailableWhileDisplayOff =
                value;
        })) {
        return;
    }
    (void)applyMobileAvailability(
        previous);
}

bool SettingsViewModel::
allowNearbyOnPublicNetworks() const noexcept
{
    return settings_
        .allowNearbyOnPublicNetworks;
}

void SettingsViewModel::
setAllowNearbyOnPublicNetworks(bool value)
{
    if (settings_
            .allowNearbyOnPublicNetworks
        == value) {
        return;
    }
    (void)saveAndReload(
        [value](AppSettings& settings) {
            settings
                .allowNearbyOnPublicNetworks =
                value;
        });
}

bool SettingsViewModel::
mobilePairingAvailable() const noexcept
{
    return mobilePairingAvailable_;
}

bool SettingsViewModel::
mobilePairingActive() const noexcept
{
    return !mobilePairingCode_.isEmpty()
        || !mobilePairingLink_.isEmpty();
}

QString SettingsViewModel::
mobilePairingCode() const
{
    return mobilePairingCode_;
}

qint64 SettingsViewModel::
mobilePairingExpiresAtMilliseconds() const
    noexcept
{
    return mobilePairingExpiresAtMilliseconds_;
}

QString SettingsViewModel::
mobilePairingLink() const
{
    return mobilePairingLink_;
}

QString SettingsViewModel::
mobilePairingQrSource() const
{
    return mobilePairingQrSource_;
}

bool SettingsViewModel::
hasMobilePairingLink() const noexcept
{
    return !mobilePairingLink_.isEmpty();
}

QString SettingsViewModel::
mobilePairingLinkStatus() const
{
    return mobilePairingLinkStatus_;
}

QString SettingsViewModel::
mobilePairingStatusText() const
{
    return mobilePairingStatusText_;
}

QVariantList SettingsViewModel::
pairedMobileDevices() const
{
    return pairedMobileDevices_;
}

bool SettingsViewModel::
mobileNearbyAccessAvailable() const
    noexcept
{
    return mobileNearbyAccessAvailable_;
}

QString SettingsViewModel::
mobileNearbyAccessStatusText() const
{
    return mobileNearbyAccessStatusText_;
}

void SettingsViewModel::
setMobilePairingCoordinator(
    PairingCoordinator* coordinator)
{
    if (pairingCoordinator_ == coordinator) {
        return;
    }
    if (pairingCoordinator_) {
        disconnect(
            pairingCoordinator_,
            nullptr,
            this,
            nullptr);
    }
    pairingCoordinator_ = coordinator;
    if (pairingCoordinator_) {
        connect(
            pairingCoordinator_,
            &PairingCoordinator::
                pairingStateChanged,
            this,
            &SettingsViewModel::
                refreshMobilePairingState,
            Qt::UniqueConnection);
    }
    refreshMobilePairingState();
}

void SettingsViewModel::
setRelayPairingBootstrap(
    RelayPairingBootstrap* bootstrap)
{
    if (relayPairingBootstrap_
        == bootstrap) {
        return;
    }
    if (relayPairingBootstrap_) {
        disconnect(
            relayPairingBootstrap_,
            nullptr,
            this,
            nullptr);
    }
    relayPairingBootstrap_ = bootstrap;
    if (relayPairingBootstrap_) {
        connect(
            relayPairingBootstrap_,
            &RelayPairingBootstrap::
                stateChanged,
            this,
            &SettingsViewModel::
                refreshMobilePairingState,
            Qt::UniqueConnection);
    }
    refreshMobilePairingState();
}

void SettingsViewModel::
setMobileNearbyAccessState(
    bool available,
    QString statusText)
{
    if (mobileNearbyAccessAvailable_
            == available
        && mobileNearbyAccessStatusText_
            == statusText) {
        return;
    }
    mobileNearbyAccessAvailable_ =
        available;
    mobileNearbyAccessStatusText_ =
        std::move(statusText);
    emit mobileNearbyAccessChanged();
}

void SettingsViewModel::
setMobileAvailabilityCommand(
    MobileAvailabilityCommand command)
{
    mobileAvailabilityCommand_ =
        std::move(command);
}

void SettingsViewModel::
setCopyMobilePairingLinkCommand(
    CopyTextCommand command)
{
    copyMobilePairingLinkCommand_ =
        std::move(command);
}

QString SettingsViewModel::relayUrl() const
{
    return configuredRelayUrl();
}

QString SettingsViewModel::relayStatusText()
    const
{
    return relayStatusText_;
}

bool SettingsViewModel::
relayAutomaticAvailable() const
{
    return RelaySettings::validatedUrl(
               relaySettings_.bundledUrl())
        .hasValue();
}

bool SettingsViewModel::
relayDisableAvailable() const
{
    return !configuredRelayUrl().isEmpty();
}

bool SettingsViewModel::saveOpenAIAPIKey(
    const QString& secret)
{
    return saveCredential(
        ChatCredentialKind::OpenAI,
        secret);
}

bool SettingsViewModel::removeOpenAIAPIKey()
{
    return removeCredential(
        ChatCredentialKind::OpenAI);
}

bool SettingsViewModel::saveLumoAPIKey(
    const QString& secret)
{
    return saveCredential(
        ChatCredentialKind::Lumo,
        secret);
}

bool SettingsViewModel::removeLumoAPIKey()
{
    return removeCredential(
        ChatCredentialKind::Lumo);
}

bool SettingsViewModel::addCodexAccount(
    const QString& label)
{
    if (!codexAccountProfileStore_) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex account profiles are unavailable."));
        return false;
    }
    const auto added =
        codexAccountProfileStore_->add(label);
    if (!added.hasValue()) {
        setCodexAccountStatus(
            QStringLiteral(
                "The Codex profile could not be added: %1")
                .arg(added.error().message));
        reportRuntimeError(added.error());
        return false;
    }
    const auto selected =
        codexAccountProfileStore_->select(
            added.value().id);
    if (!selected.hasValue()) {
        setCodexAccountStatus(
            selected.error().message);
        reportRuntimeError(
            selected.error());
        return false;
    }
    setCodexAccountStatus(
        QStringLiteral(
            "%1 was added. Sign in with the official Codex CLI before using it.")
            .arg(added.value().label));
    refreshCodexAccountState();
    return true;
}

bool SettingsViewModel::
removeSelectedCodexAccount()
{
    if (!codexAccountRouter_) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex account profiles are unavailable."));
        return false;
    }
    const auto selected =
        selectedCodexAccountProfile();
    if (!selected.has_value()) {
        setCodexAccountStatus(
            QStringLiteral(
                "Select a Codex account profile first."));
        return false;
    }
    const auto removed =
        codexAccountRouter_->removeProfile(
            selected->id);
    if (!removed.hasValue()) {
        setCodexAccountStatus(
            removed.error().message);
        reportRuntimeError(
            removed.error());
        return false;
    }
    if (!removed.value()) {
        setCodexAccountStatus(
            QStringLiteral(
                "%1 no longer exists.")
                .arg(selected->label));
        refreshCodexAccountState();
        return false;
    }
    setCodexAccountStatus(
        QStringLiteral(
            "%1 was removed. No account credentials were deleted or copied.")
            .arg(selected->label));
    refreshCodexAccountState();
    return true;
}

bool SettingsViewModel::
beginSelectedCodexAccountLogin()
{
    if (!codexAccountLoginService_) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex account sign-in is unavailable."));
        return false;
    }
    const auto selected =
        selectedCodexAccountProfile();
    const bool currentAccount =
        codexAccountProfileStore_
        && !codexAccountProfileStore_
                ->selectedProfileId()
                .has_value();
    const auto started = currentAccount
        ? codexAccountLoginService_
              ->beginLoginCurrentAccount()
        : selected.has_value()
            ? codexAccountLoginService_
                  ->beginLogin(*selected)
            : Result<void>::failure(
                  settingsCommandError(
                      QStringLiteral(
                          "codex.account_profile_missing"),
                      QStringLiteral(
                          "Select a Codex account profile first.")));
    if (!started.hasValue()) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex sign-in could not start: %1")
                .arg(started.error().message));
        reportRuntimeError(
            started.error());
        return false;
    }
    setCodexAccountStatus(
        currentAccount
            ? QStringLiteral(
                  "Official Codex sign-in opened for the current Codex account.")
            : QStringLiteral(
                  "Official Codex sign-in opened for %1.")
                  .arg(selected->label));
    return true;
}

bool SettingsViewModel::
refreshSelectedCodexAccount()
{
    if (codexAccountRefreshInProgress_) {
        return false;
    }
    if (!codexAccountLoginService_) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex account status is unavailable."));
        return false;
    }
    const auto selected =
        selectedCodexAccountProfile();
    const bool currentAccount =
        codexAccountProfileStore_
        && !codexAccountProfileStore_
                ->selectedProfileId()
                .has_value();
    if (!currentAccount
        && !selected.has_value()) {
        setCodexAccountStatus(
            QStringLiteral(
                "Select a Codex account profile first."));
        return false;
    }

    codexAccountRefreshInProgress_ =
        true;
    setCodexAccountStatus(
        QStringLiteral(
            "Checking %1...")
            .arg(
                currentAccount
                    ? QStringLiteral(
                          "current Codex account")
                    : selected->label));
    emit codexAccountsChanged();
    CodexAccountLoginService* service =
        codexAccountLoginService_;
    const std::optional<CodexAccountProfile>
        profile = selected;
    codexAccountStatusWatcher_.setFuture(
        QtConcurrent::run(
            [service, profile, currentAccount] {
                return currentAccount
                    ? service
                          ->readCurrentAccountStatus()
                    : service->readStatus(
                          *profile);
            }));
    return true;
}

bool SettingsViewModel::beginMobilePairing()
{
    if (!settings_.mobileEnabled) {
        reportRuntimeError(
            settingsCommandError(
                QStringLiteral(
                    "settings.mobile-disabled"),
                QStringLiteral(
                    "Enable mobile access before pairing an iPhone.")));
        return false;
    }
    if (!pairingCoordinator_) {
        reportRuntimeError(
            settingsCommandError(
                QStringLiteral(
                    "settings.mobile-unavailable"),
                QStringLiteral(
                    "The Companion mobile bridge is unavailable.")));
        return false;
    }

    mobilePairingCommandError_.clear();
    mobilePairingLinkStatus_.clear();
    if (relayPairingBootstrap_) {
        const auto pairing =
            relayPairingBootstrap_
                ->beginPairing();
        if (!pairing.hasValue()) {
            mobilePairingCommandError_ =
                QStringLiteral(
                    "Pairing could not start: %1")
                    .arg(
                        pairing.error()
                            .message);
            refreshMobilePairingState();
            reportRuntimeError(
                pairing.error());
            return false;
        }
        refreshMobilePairingState();
        return true;
    }

    const auto pairing =
        pairingCoordinator_->beginPairing();
    if (!pairing.hasValue()) {
        mobilePairingCommandError_ =
            QStringLiteral(
                "Pairing could not start: %1")
                .arg(
                    pairing.error()
                        .message);
        refreshMobilePairingState();
        reportRuntimeError(pairing.error());
        return false;
    }
    refreshMobilePairingState();
    return true;
}

void SettingsViewModel::cancelMobilePairing()
{
    mobilePairingCommandError_.clear();
    mobilePairingLinkStatus_.clear();
    if (relayPairingBootstrap_) {
        relayPairingBootstrap_
            ->cancelPairing();
    } else if (pairingCoordinator_) {
        pairingCoordinator_
            ->cancelPairing();
    }
    refreshMobilePairingState();
}

bool SettingsViewModel::
copyMobilePairingLink()
{
    if (mobilePairingLink_.isEmpty()) {
        return false;
    }
    const auto copied =
        copyMobilePairingLinkCommand_
        ? copyMobilePairingLinkCommand_(
              mobilePairingLink_)
        : Result<void>::failure(
              settingsCommandError(
                  QStringLiteral(
                      "clipboard.unavailable"),
                  QStringLiteral(
                      "The Windows clipboard is unavailable.")));
    if (copied.hasValue()) {
        mobilePairingLinkStatus_ =
            QStringLiteral(
                "Pairing link copied.");
        emit mobilePairingChanged();
        return true;
    }

    mobilePairingLinkStatus_ =
        QStringLiteral(
            "The pairing link could not be copied: %1")
            .arg(copied.error().message);
    emit mobilePairingChanged();
    return false;
}

bool SettingsViewModel::forgetMobileDevice(
    const QString& deviceId)
{
    if (!pairingCoordinator_) {
        reportRuntimeError(
            settingsCommandError(
                QStringLiteral(
                    "settings.mobile-unavailable"),
                QStringLiteral(
                    "The Companion mobile bridge is unavailable.")));
        return false;
    }
    const QString normalized =
        deviceId.trimmed();
    if (normalized.isEmpty()) {
        reportRuntimeError(
            settingsCommandError(
                QStringLiteral(
                    "settings.mobile-device-invalid"),
                QStringLiteral(
                    "The paired mobile device is invalid.")));
        return false;
    }
    const auto forgotten =
        pairingCoordinator_->forget(
            normalized);
    if (!forgotten.hasValue()) {
        reportRuntimeError(
            forgotten.error());
        return false;
    }
    refreshMobilePairingState();
    return true;
}

bool SettingsViewModel::saveRelayUrl(
    const QString& value)
{
    const auto candidate =
        relaySettings_.withCustomUrl(
            settings_,
            value);
    if (!candidate.hasValue()) {
        relayStatusText_ =
            QStringLiteral(
                "Enter a valid wss:// URL. Unencrypted ws:// is limited to localhost testing.");
        emit relayConfigurationChanged();
        reportRuntimeError(candidate.error());
        return false;
    }
    const AppSettings requested =
        candidate.value();
    if (!saveAndReload(
            [&requested](
                AppSettings& settings) {
                settings.relayMode =
                    requested.relayMode;
                settings.customRelayUrl =
                    requested.customRelayUrl;
            })) {
        return false;
    }
    relayStatusText_ =
        QStringLiteral(
            "Remote access saved. Reconnect the paired phone nearby once to synchronize it.");
    emit relayConfigurationChanged();
    return true;
}

bool SettingsViewModel::useAutomaticRelay()
{
    const AppSettings requested =
        relaySettings_.useAutomatic(
            settings_);
    const auto configured =
        relaySettings_.configuredUrl(
            requested);
    if (!configured.hasValue()) {
        relayStatusText_ =
            QStringLiteral(
                "The bundled secure relay is unavailable in this build.");
        emit relayConfigurationChanged();
        reportRuntimeError(
            configured.error());
        return false;
    }
    if (!saveAndReload(
            [&requested](
                AppSettings& settings) {
                settings.relayMode =
                    requested.relayMode;
                settings.customRelayUrl =
                    requested.customRelayUrl;
            })) {
        return false;
    }
    relayStatusText_ =
        QStringLiteral(
            "Automatic secure relay restored. Reconnect the paired phone nearby once to synchronize it.");
    emit relayConfigurationChanged();
    return true;
}

bool SettingsViewModel::disableRelay()
{
    if (settings_.relayMode
            == RelayMode::Disabled
        && settings_
               .customRelayUrl.isEmpty()) {
        return true;
    }
    if (!saveAndReload(
            [](AppSettings& settings) {
                settings.relayMode =
                    RelayMode::Disabled;
                settings.customRelayUrl
                    .clear();
            })) {
        return false;
    }
    relayStatusText_ =
        QStringLiteral(
            "Remote access is disabled. Nearby access still works.");
    emit relayConfigurationChanged();
    return true;
}

bool SettingsViewModel::saveAndReload(SettingsMutation mutation)
{
    const auto updated =
        settingsRepository_.update(
            std::move(mutation));
    if (!updated.hasValue()) {
        reportRuntimeError(updated.error());
        return false;
    }

    publishSettings(updated.value());
    return true;
}

void SettingsViewModel::publishSettings(const AppSettings& settings)
{
    const bool backdropChanged = settings_.backdrop != settings.backdrop;
    const bool animationSpeedChanged =
        !qFuzzyCompare(settings_.animationSpeedScale, settings.animationSpeedScale);
    const bool hideControlsChanged =
        settings_.hideControlsUntilHover != settings.hideControlsUntilHover;
    const bool autonomousMovementChanged =
        settings_.allowAutonomousMovement != settings.allowAutonomousMovement;
    const bool mobileEnabledValueChanged =
        settings_.mobileEnabled
        != settings.mobileEnabled;
    const bool keepAvailableChanged =
        settings_
            .keepAvailableWhileDisplayOff
        != settings
               .keepAvailableWhileDisplayOff;
    const bool publicNetworkAccessChanged =
        settings_
            .allowNearbyOnPublicNetworks
        != settings
               .allowNearbyOnPublicNetworks;
    const bool relayChanged =
        settings_.relayMode
            != settings.relayMode
        || settings_.customRelayUrl
            != settings.customRelayUrl;
    const bool accountContinuationChanged =
        settings_
            .automaticallyContinuesAcrossCodexAccounts
        != settings
               .automaticallyContinuesAcrossCodexAccounts;

    settings_ = settings;

    if (backdropChanged) {
        emit backdropModeChanged();
    }
    if (animationSpeedChanged) {
        emit animationSpeedScaleChanged();
    }
    if (hideControlsChanged) {
        emit hideControlsUntilHoverChanged();
    }
    if (autonomousMovementChanged) {
        emit allowAutonomousMovementChanged();
    }
    if (mobileEnabledValueChanged) {
        emit mobileEnabledChanged();
    }
    if (keepAvailableChanged) {
        emit keepAvailableWhileDisplayOffChanged();
    }
    if (publicNetworkAccessChanged) {
        emit allowNearbyOnPublicNetworksChanged();
    }
    if (relayChanged) {
        relayStatusText_ =
            defaultRelayStatusText();
        emit relayConfigurationChanged();
    }
    if (accountContinuationChanged) {
        emit
            automaticallyContinuesAcrossCodexAccountsChanged();
    }
    if (mobileEnabledValueChanged
        || keepAvailableChanged
        || publicNetworkAccessChanged
        || relayChanged) {
        emit mobileConfigurationChanged();
    }
}

void SettingsViewModel::
refreshCodexAccountState()
{
    QVariantList nextProfiles;
    QString nextSelectedId;
    if (codexAccountProfileStore_) {
        const auto profiles =
            codexAccountProfileStore_
                ->profiles();
        const auto selectedId =
            codexAccountProfileStore_
                ->selectedProfileId();
        if (selectedId.has_value()) {
            nextSelectedId =
                codexAccountProfileIdString(
                    *selectedId);
        } else {
            nextSelectedId =
                currentCodexAccountProfileId();
        }
        nextProfiles.reserve(
            profiles.size() + 1);
        QVariantMap currentAccount;
        currentAccount.insert(
            QStringLiteral("id"),
            currentCodexAccountProfileId());
        currentAccount.insert(
            QStringLiteral("label"),
            QStringLiteral(
                "Current Codex account"));
        currentAccount.insert(
            QStringLiteral("selected"),
            !selectedId.has_value());
        currentAccount.insert(
            QStringLiteral("removable"),
            false);
        currentAccount.insert(
            QStringLiteral("currentAccount"),
            true);
        nextProfiles.append(currentAccount);
        for (const CodexAccountProfile&
                 profile : profiles) {
            QVariantMap item;
            item.insert(
                QStringLiteral("id"),
                codexAccountProfileIdString(
                    profile.id));
            item.insert(
                QStringLiteral("label"),
                profile.label);
            item.insert(
                QStringLiteral("selected"),
                selectedId == profile.id);
            item.insert(
                QStringLiteral("removable"),
                true);
            item.insert(
                QStringLiteral("currentAccount"),
                false);
            nextProfiles.append(item);
        }
    }

    const bool changed =
        codexAccountProfiles_
            != nextProfiles
        || selectedCodexAccountProfileId_
            != nextSelectedId;
    codexAccountProfiles_ =
        std::move(nextProfiles);
    selectedCodexAccountProfileId_ =
        std::move(nextSelectedId);
    if (changed) {
        emit codexAccountsChanged();
    }
}

void SettingsViewModel::
finishCodexAccountRefresh()
{
    const auto result =
        codexAccountStatusWatcher_.result();
    codexAccountRefreshInProgress_ =
        false;
    if (!result.hasValue()) {
        setCodexAccountStatus(
            QStringLiteral(
                "Codex account status is unavailable: %1")
                .arg(result.error().message));
        reportRuntimeError(
            result.error());
        emit codexAccountsChanged();
        return;
    }
    setCodexAccountStatus(
        result.value().message);
    emit codexAccountsChanged();
}

void SettingsViewModel::
setCodexAccountStatus(QString status)
{
    if (codexAccountStatus_ == status) {
        return;
    }
    codexAccountStatus_ =
        std::move(status);
    emit codexAccountsChanged();
}

std::optional<CodexAccountProfile>
SettingsViewModel::
selectedCodexAccountProfile() const
{
    if (!codexAccountProfileStore_) {
        return std::nullopt;
    }
    return codexAccountProfileStore_
        ->selectedProfile();
}

void SettingsViewModel::
refreshMobilePairingState()
{
    if (refreshingMobilePairing_) {
        return;
    }
    refreshingMobilePairing_ = true;

    QString nextCode;
    QString nextLink;
    QString nextQrSource;
    QString qrStatus;
    QString nextStatus;
    qint64 nextExpiresAt = 0;
    QVariantList nextDevices;
    const bool nextAvailable =
        !pairingCoordinator_.isNull()
        || !relayPairingBootstrap_
                .isNull();
    if (pairingCoordinator_) {
        const auto active =
            pairingCoordinator_
                ->activePairing();
        if (active.has_value()) {
            nextCode = active->code;
            if (nextCode.size() == 6) {
                nextCode.insert(
                    3,
                    QLatin1Char(' '));
            }
            nextExpiresAt =
                active->expiresAt
                    .toMSecsSinceEpoch();
        }

        auto records =
            pairingCoordinator_
                ->trustedRecords();
        std::sort(
            records.begin(),
            records.end(),
            [](const PairingRecord& left,
               const PairingRecord& right) {
                const int byName =
                    QString::compare(
                        left.displayName,
                        right.displayName,
                        Qt::CaseInsensitive);
                return byName == 0
                    ? left.deviceId
                          < right.deviceId
                    : byName < 0;
            });
        nextDevices.reserve(
            records.size());
        for (const PairingRecord& record :
             records) {
            QVariantMap device;
            device.insert(
                QStringLiteral("deviceId"),
                record.deviceId);
            device.insert(
                QStringLiteral(
                    "displayName"),
                record.displayName);
            device.insert(
                QStringLiteral(
                    "pairedAtMilliseconds"),
                record.pairedAt
                    .toMSecsSinceEpoch());
            nextDevices.append(device);
        }
    }

    if (relayPairingBootstrap_) {
        const auto offer =
            relayPairingBootstrap_
                ->activeOffer();
        if (offer.has_value()) {
            if (nextCode.isEmpty()) {
                nextCode =
                    offer->pairingCode;
                if (nextCode.size()
                    == 6) {
                    nextCode.insert(
                        3,
                        QLatin1Char(' '));
                }
            }
            if (nextExpiresAt <= 0) {
                nextExpiresAt =
                    offer
                        ->expiresAtMilliseconds;
            }
            const auto link =
                offer->pairingLink();
            if (link.hasValue()) {
                nextLink =
                    link.value();
            } else {
                nextStatus =
                    QStringLiteral(
                        "Secure mobile pairing failed: %1")
                        .arg(
                            link.error()
                                .message);
            }
        }
    }

    if (!mobilePairingCommandError_
             .isEmpty()) {
        nextStatus =
            mobilePairingCommandError_;
    } else if (relayPairingBootstrap_
               && relayPairingBootstrap_
                      ->lastError()
                      .has_value()) {
        nextStatus =
            relayPairingBootstrap_
                ->lastError()
                ->message;
    } else if (!pairingCoordinator_) {
        nextStatus =
            QStringLiteral(
                "Mobile pairing security is unavailable in this process.");
    } else if (!relayPairingBootstrap_) {
        nextStatus =
            QStringLiteral(
                "%1 Secure relay bootstrap is unavailable in this process.")
                .arg(
                    mobileNearbyAccessStatusText_);
    } else if (!nextLink.isEmpty()) {
        nextStatus =
            QStringLiteral(
                "Open or paste the secure short-lived pairing link in a compatible Companion Mobile build. The link contains a temporary bootstrap secret; keep it private.");
    } else {
        nextStatus =
            QStringLiteral(
                "Secure relay pairing is ready.");
    }

    if (!nextLink.isEmpty()) {
        if (nextLink == mobilePairingLink_
            && !mobilePairingQrSource_
                    .isEmpty()) {
            nextQrSource =
                mobilePairingQrSource_;
        } else {
            const auto qr =
                pairingQrCodeDataUrl(nextLink);
            if (qr.hasValue()) {
                nextQrSource = qr.value();
            } else {
                qrStatus = QStringLiteral(
                    "QR code unavailable; use the pairing link or code. %1")
                               .arg(
                                   qr.error()
                                       .message);
            }
        }
    }
    if (!qrStatus.isEmpty()) {
        if (!nextStatus.isEmpty()) {
            nextStatus.append(
                QLatin1Char(' '));
        }
        nextStatus.append(qrStatus);
    }

    const bool changed =
        mobilePairingAvailable_
            != nextAvailable
        || mobilePairingCode_ != nextCode
        || mobilePairingLink_
            != nextLink
        || mobilePairingQrSource_
            != nextQrSource
        || mobilePairingStatusText_
            != nextStatus
        || mobilePairingExpiresAtMilliseconds_
            != nextExpiresAt
        || pairedMobileDevices_
            != nextDevices;
    mobilePairingCode_ =
        std::move(nextCode);
    mobilePairingLink_ =
        std::move(nextLink);
    mobilePairingQrSource_ =
        std::move(nextQrSource);
    mobilePairingStatusText_ =
        std::move(nextStatus);
    mobilePairingAvailable_ =
        nextAvailable;
    mobilePairingExpiresAtMilliseconds_ =
        nextExpiresAt;
    pairedMobileDevices_ =
        std::move(nextDevices);
    if (mobilePairingCode_.isEmpty()
        && mobilePairingLink_.isEmpty()) {
        pairingRefreshTimer_.stop();
    } else if (!pairingRefreshTimer_
                    .isActive()) {
        pairingRefreshTimer_.start();
    }
    refreshingMobilePairing_ = false;
    if (changed) {
        emit mobilePairingChanged();
    }
}

bool SettingsViewModel::
applyMobileAvailability(
    const AppSettings& previousSettings)
{
    if (!mobileAvailabilityCommand_) {
        return true;
    }
    const auto applied =
        mobileAvailabilityCommand_(
            settings_.mobileEnabled,
            settings_
                .keepAvailableWhileDisplayOff);
    if (applied.hasValue()) {
        return true;
    }

    const auto rolledBack =
        settingsRepository_.update(
            [&previousSettings](
                AppSettings& settings) {
                settings.mobileEnabled =
                    previousSettings
                        .mobileEnabled;
                settings
                    .keepAvailableWhileDisplayOff =
                    previousSettings
                        .keepAvailableWhileDisplayOff;
            });
    if (rolledBack.hasValue()) {
        publishSettings(
            rolledBack.value());
    } else {
        reportRuntimeError(
            rolledBack.error());
    }
    reportRuntimeError(applied.error());
    return false;
}

QString SettingsViewModel::
configuredRelayUrl() const
{
    const auto configured =
        relaySettings_.configuredUrl(
            settings_);
    if (!configured.hasValue()
        || !configured.value()
                .has_value()) {
        return {};
    }
    return configured.value()
        ->toString(QUrl::FullyEncoded);
}

QString SettingsViewModel::
defaultRelayStatusText() const
{
    if (settings_.relayMode
        == RelayMode::Disabled) {
        return QStringLiteral(
            "Remote access is disabled. Nearby access still works.");
    }
    if (settings_.relayMode
        == RelayMode::Custom) {
        return QStringLiteral(
            "Remote access is configured for paired devices.");
    }
    if (!configuredRelayUrl().isEmpty()) {
        return QStringLiteral(
            "Secure remote access uses the bundled Companion relay.");
    }
    return QStringLiteral(
        "No secure relay is configured. Nearby access still works.");
}

bool SettingsViewModel::saveCredential(
    ChatCredentialKind kind,
    const QString& secret)
{
    const bool openAI =
        kind == ChatCredentialKind::OpenAI;
    if (secret.trimmed().isEmpty()) {
        publishCredentialStatus(
            kind,
            openAI
                ? hasOpenAIAPIKey_
                : hasLumoAPIKey_,
            openAI
                ? QStringLiteral(
                      "Enter an OpenAI API key before saving.")
                : QStringLiteral(
                      "Enter a Lumo API key before saving."));
        return false;
    }
    if (!credentialStore_) {
        const auto error =
            settingsCommandError(
                QStringLiteral(
                    "settings.credential-store-unavailable"),
                QStringLiteral(
                    "Secure Windows credential storage is unavailable."));
        publishCredentialStatus(
            kind,
            false,
            error.message);
        reportRuntimeError(error);
        return false;
    }

    const auto saved =
        ChatCredentialService::save(
            *credentialStore_,
            kind,
            secret);
    if (!saved.hasValue()) {
        publishCredentialStatus(
            kind,
            openAI
                ? hasOpenAIAPIKey_
                : hasLumoAPIKey_,
            saved.error().message);
        reportRuntimeError(saved.error());
        return false;
    }

    const bool available =
        ChatCredentialService::
            hasUsableCredential(
                *credentialStore_,
                kind);
    publishCredentialStatus(
        kind,
        available,
        openAI
            ? available
                ? QStringLiteral(
                      "OpenAI API key saved for this Windows PC.")
                : QStringLiteral(
                      "No OpenAI API key saved.")
            : available
                ? QStringLiteral(
                      "Lumo API key saved for this Windows PC.")
                : QStringLiteral(
                      "No Lumo API key saved."));
    emit chatCredentialsChanged();
    return available;
}

bool SettingsViewModel::removeCredential(
    ChatCredentialKind kind)
{
    const bool openAI =
        kind == ChatCredentialKind::OpenAI;
    if (!credentialStore_) {
        const auto error =
            settingsCommandError(
                QStringLiteral(
                    "settings.credential-store-unavailable"),
                QStringLiteral(
                    "Secure Windows credential storage is unavailable."));
        publishCredentialStatus(
            kind,
            false,
            error.message);
        reportRuntimeError(error);
        return false;
    }

    const auto removed =
        ChatCredentialService::remove(
            *credentialStore_,
            kind);
    if (!removed.hasValue()) {
        publishCredentialStatus(
            kind,
            openAI
                ? hasOpenAIAPIKey_
                : hasLumoAPIKey_,
            removed.error().message);
        reportRuntimeError(removed.error());
        return false;
    }

    publishCredentialStatus(
        kind,
        false,
        openAI
            ? QStringLiteral(
                  "OpenAI API key removed.")
            : QStringLiteral(
                  "Lumo API key removed."));
    emit chatCredentialsChanged();
    return true;
}

void SettingsViewModel::publishCredentialStatus(
    ChatCredentialKind kind,
    bool available,
    QString status)
{
    if (kind == ChatCredentialKind::OpenAI) {
        if (hasOpenAIAPIKey_ == available
            && openAIAPIKeyStatus_ == status) {
            return;
        }
        hasOpenAIAPIKey_ = available;
        openAIAPIKeyStatus_ =
            std::move(status);
        emit openAICredentialChanged();
        return;
    }

    if (hasLumoAPIKey_ == available
        && lumoAPIKeyStatus_ == status) {
        return;
    }
    hasLumoAPIKey_ = available;
    lumoAPIKeyStatus_ = std::move(status);
    emit lumoCredentialChanged();
}

void SettingsViewModel::reportRuntimeError(const CompanionError& error)
{
    emit runtimeErrorOccurred(error);
}

} // namespace companion
