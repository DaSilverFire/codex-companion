#include "codex/accounts/CodexAccountLoginService.h"
#include "codex/accounts/CodexAccountProfileStore.h"
#include "codex/accounts/CodexAccountRouter.h"
#include "codex/accounts/CodexAccountRuntime.h"
#include "codex/accounts/CodexThreadAccountBindingStore.h"
#include "core/ChatCredentialService.h"
#include "mobile/relay/RelayPairingBootstrap.h"
#include "mobile/security/PairingCoordinator.h"
#include "mobile/security/PairingRecordStore.h"
#include "mobile/security/RelayStateStore.h"
#include "mobile/security/SecretProtector.h"
#include "platform/windows/DpapiCredentialStore.h"
#include "ui/SettingsViewModel.h"

#include <QDateTime>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>

#include <memory>

namespace {

class TestSecretProtector final
    : public companion::SecretProtector {
public:
    companion::Result<QByteArray> protect(
        QByteArrayView plaintext,
        QByteArrayView) const override
    {
        return companion::Result<QByteArray>::success(
            QByteArray(plaintext.data(), plaintext.size()));
    }

    companion::Result<QByteArray> unprotect(
        QByteArrayView protectedData,
        QByteArrayView) const override
    {
        return companion::Result<QByteArray>::success(
            QByteArray(
                protectedData.data(),
                protectedData.size()));
    }
};

QDateTime pairingNow()
{
    return QDateTime::fromMSecsSinceEpoch(
        1'770'000'000'123,
        QTimeZone::UTC);
}

QFuture<companion::Result<void>>
readyResult(
    companion::Result<void> result)
{
    QPromise<companion::Result<void>>
        promise;
    promise.start();
    auto future = promise.future();
    promise.addResult(
        std::move(result));
    promise.finish();
    return future;
}

class SettingsBootstrapEndpoint final
    : public companion::
          RelayPairingBootstrapEndpoint {
public:
    void start() override
    {
    }

    void stop() override
    {
    }

    QFuture<companion::Result<void>>
    send(
        const companion::
            EncryptedEnvelope&) override
    {
        return readyResult(
            companion::Result<void>::
                success());
    }
};

} // namespace

class SettingsViewModelTests final : public QObject {
    Q_OBJECT

private slots:
    void exactBackdropModesPersistThroughRepository()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());

        QVector<companion::BackdropMode> reappliedModes;
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [&reappliedModes](companion::BackdropMode mode) {
                reappliedModes.append(mode);
                return companion::Result<companion::BackdropMode>::success(mode);
            });

        const struct {
            QString value;
            companion::BackdropMode mode;
        } cases[] = {
            {QStringLiteral("windows-glass"), companion::BackdropMode::WindowsGlass},
            {QStringLiteral("solid-black"), companion::BackdropMode::SolidBlack},
            {QStringLiteral("mica"), companion::BackdropMode::Mica},
        };

        for (const auto& testCase : cases) {
            viewModel.setBackdropMode(testCase.value);
            QCOMPARE(viewModel.backdropMode(), testCase.value);
            const auto reloaded = repository.load();
            QVERIFY(reloaded.hasValue());
            QCOMPARE(reloaded.value().backdrop, testCase.mode);
            QCOMPARE(reappliedModes.last(), testCase.mode);
        }
    }

    void settingsMutationPreservesNewerChatModelSelection()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());

        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(
                    requested);
            });
        const auto chatSelectionSaved =
            repository.update(
                [](companion::AppSettings& settings) {
                    settings.selectedChatModelId =
                        QStringLiteral(
                            "openai:gpt56Sol");
                });
        QVERIFY(chatSelectionSaved.hasValue());

        viewModel.setAnimationSpeedScale(1.5);

        const auto reloaded = repository.load();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(
            reloaded.value().selectedChatModelId,
            QStringLiteral("openai:gpt56Sol"));
        QCOMPARE(
            reloaded.value().animationSpeedScale,
            1.5);
    }

    void unknownBackdropModeEmitsTypedCommandErrorWithoutSaving()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        loaded.backdrop = companion::BackdropMode::Mica;
        QVERIFY(repository.save(loaded).hasValue());

        bool reapplyCalled = false;
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [&reapplyCalled](companion::BackdropMode) {
                reapplyCalled = true;
                return companion::Result<companion::BackdropMode>::success(
                    companion::BackdropMode::Mica);
            });
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        viewModel.setBackdropMode(QStringLiteral("liquid-glass"));

        QVERIFY(!reapplyCalled);
        QCOMPARE(viewModel.backdropMode(), QStringLiteral("mica"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("settings.backdrop-mode-invalid"));
        const auto reloaded = repository.load();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(reloaded.value().backdrop, companion::BackdropMode::Mica);
    }

    void materialReapplyFailureRollsBackPersistedAndExposedBackdrop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        loaded.backdrop = companion::BackdropMode::Mica;
        QVERIFY(repository.save(loaded).hasValue());

        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode) {
                return companion::Result<companion::BackdropMode>::failure({
                    QStringLiteral("window.backdrop-reapply-failed"),
                    QStringLiteral("Could not reapply Companion materials."),
                    false,
                    {},
                });
            });
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        viewModel.setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(viewModel.backdropMode(), QStringLiteral("mica"));
        QCOMPARE(viewModel.effectiveBackdropMode(), QStringLiteral("mica"));
        QCOMPARE(errorSpy.count(), 1);
        const auto reloaded = repository.load();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(reloaded.value().backdrop, companion::BackdropMode::Mica);
    }

    void materialFailureRetainsEffectiveModeDeliveredDuringCoordinatorAttempt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        loaded.backdrop = companion::BackdropMode::Mica;
        QVERIFY(repository.save(loaded).hasValue());

        companion::SettingsViewModel* viewModelPointer = nullptr;
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [&viewModelPointer](companion::BackdropMode) {
                viewModelPointer->setEffectiveBackdropMode(
                    companion::BackdropMode::WindowsGlass);
                return companion::Result<companion::BackdropMode>::failure({
                    QStringLiteral("window.backdrop-rollback-failed"),
                    QStringLiteral("Could not restore every Companion material."),
                    false,
                    {},
                });
            });
        viewModelPointer = &viewModel;
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        viewModel.setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(viewModel.backdropMode(), QStringLiteral("mica"));
        QCOMPARE(viewModel.effectiveBackdropMode(),
                 QStringLiteral("windows-glass"));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(qvariant_cast<companion::CompanionError>(
                     errorSpy.takeFirst().at(0))
                     .code,
                 QStringLiteral("window.backdrop-rollback-failed"));
        const auto reloaded = repository.load();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(reloaded.value().backdrop, companion::BackdropMode::Mica);
    }

    void successfulMaterialFallbackExposesEffectiveBackdropSeparatelyFromRequest()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());

        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                Q_UNUSED(requested);
                return companion::Result<companion::BackdropMode>::success(
                    companion::BackdropMode::SolidBlack);
            });

        viewModel.setBackdropMode(QStringLiteral("windows-glass"));

        QCOMPARE(viewModel.backdropMode(), QStringLiteral("windows-glass"));
        QCOMPARE(viewModel.effectiveBackdropMode(), QStringLiteral("solid-black"));
        const auto reloaded = repository.load();
        QVERIFY(reloaded.hasValue());
        QCOMPARE(reloaded.value().backdrop, companion::BackdropMode::WindowsGlass);
    }

    void credentialSettingsSaveReplaceRemoveAndNotifyImmediately()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        const auto credentials =
            std::make_shared<companion::DpapiCredentialStore>(
                directory.filePath(QStringLiteral("Credentials")),
                [](const QString&, bool) {
                    return companion::Result<void>::success();
                });
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(requested);
            },
            credentials);
        QSignalSpy changedSpy(
            &viewModel,
            &companion::SettingsViewModel::chatCredentialsChanged);
        QVERIFY(changedSpy.isValid());

        QVERIFY(!viewModel.hasOpenAIAPIKey());
        QVERIFY(!viewModel.hasLumoAPIKey());
        QCOMPARE(
            viewModel.openAIAPIKeyStatus(),
            QStringLiteral("No OpenAI API key saved."));
        QCOMPARE(
            viewModel.lumoAPIKeyStatus(),
            QStringLiteral("No Lumo API key saved."));

        QVERIFY(viewModel.saveOpenAIAPIKey(
            QStringLiteral("  sk-openai-first  \n")));
        QVERIFY(viewModel.hasOpenAIAPIKey());
        QCOMPARE(changedSpy.count(), 1);
        const auto firstOpenAI = credentials->read(
            companion::ChatCredentialService::serviceName(
                companion::ChatCredentialKind::OpenAI));
        QVERIFY(firstOpenAI.hasValue());
        QCOMPARE(firstOpenAI.value(), QByteArray("sk-openai-first"));

        QVERIFY(viewModel.saveOpenAIAPIKey(
            QStringLiteral("sk-openai-replaced")));
        QCOMPARE(changedSpy.count(), 2);
        const auto replacedOpenAI = credentials->read(
            companion::ChatCredentialService::serviceName(
                companion::ChatCredentialKind::OpenAI));
        QVERIFY(replacedOpenAI.hasValue());
        QCOMPARE(
            replacedOpenAI.value(),
            QByteArray("sk-openai-replaced"));

        QVERIFY(viewModel.saveLumoAPIKey(
            QStringLiteral(" lumo-secret ")));
        QVERIFY(viewModel.hasLumoAPIKey());
        QCOMPARE(changedSpy.count(), 3);

        QVERIFY(viewModel.removeOpenAIAPIKey());
        QVERIFY(!viewModel.hasOpenAIAPIKey());
        QCOMPARE(
            viewModel.openAIAPIKeyStatus(),
            QStringLiteral("OpenAI API key removed."));
        QCOMPARE(changedSpy.count(), 4);

        QVERIFY(viewModel.removeLumoAPIKey());
        QVERIFY(!viewModel.hasLumoAPIKey());
        QCOMPARE(
            viewModel.lumoAPIKeyStatus(),
            QStringLiteral("Lumo API key removed."));
        QCOMPARE(changedSpy.count(), 5);
    }

    void credentialSettingsPersistAcrossViewModelRestartWithoutEnteringIni()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString settingsPath =
            directory.filePath(QStringLiteral("settings.ini"));
        const QString credentialPath =
            directory.filePath(QStringLiteral("Credentials"));
        companion::SettingsRepository repository(settingsPath);
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());

        {
            const auto credentials =
                std::make_shared<companion::DpapiCredentialStore>(
                    credentialPath,
                    [](const QString&, bool) {
                        return companion::Result<void>::success();
                    });
            companion::SettingsViewModel firstRun(
                loaded,
                repository,
                [](companion::BackdropMode requested) {
                    return companion::Result<
                        companion::BackdropMode>::success(requested);
                },
                credentials);
            QVERIFY(firstRun.saveOpenAIAPIKey(
                QStringLiteral("restart-openai-secret")));
            QVERIFY(firstRun.saveLumoAPIKey(
                QStringLiteral("restart-lumo-secret")));
        }

        const auto credentials =
            std::make_shared<companion::DpapiCredentialStore>(
                credentialPath,
                [](const QString&, bool) {
                    return companion::Result<void>::success();
                });
        companion::SettingsViewModel secondRun(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(requested);
            },
            credentials);

        QVERIFY(secondRun.hasOpenAIAPIKey());
        QVERIFY(secondRun.hasLumoAPIKey());
        QFile settingsFile(settingsPath);
        QVERIFY(settingsFile.open(QIODevice::ReadOnly));
        const QByteArray persistedSettings =
            settingsFile.readAll();
        QVERIFY(!persistedSettings.contains(
            "restart-openai-secret"));
        QVERIFY(!persistedSettings.contains(
            "restart-lumo-secret"));
    }

    void emptyCredentialInputIsRejectedWithoutRemovingSavedKey()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        const auto credentials =
            std::make_shared<companion::DpapiCredentialStore>(
                directory.filePath(QStringLiteral("Credentials")),
                [](const QString&, bool) {
                    return companion::Result<void>::success();
                });
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(requested);
            },
            credentials);
        QVERIFY(viewModel.saveOpenAIAPIKey(
            QStringLiteral("existing-secret")));
        QSignalSpy changedSpy(
            &viewModel,
            &companion::SettingsViewModel::chatCredentialsChanged);
        QVERIFY(changedSpy.isValid());

        QVERIFY(!viewModel.saveOpenAIAPIKey(
            QStringLiteral("  \r\n  ")));

        QVERIFY(viewModel.hasOpenAIAPIKey());
        QCOMPARE(changedSpy.count(), 0);
        QCOMPARE(
            viewModel.openAIAPIKeyStatus(),
            QStringLiteral(
                "Enter an OpenAI API key before saving."));
    }

    void codexAccountsAddSelectLoginRefreshAndPersistOptIn()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        companion::CodexAccountProfileStore profiles(
            directory.filePath(
                QStringLiteral("profiles.json")));
        companion::CodexThreadAccountBindingStore bindings(
            directory.filePath(
                QStringLiteral("bindings.json")));
        companion::CodexAccountRuntime runtime(
            directory.filePath(
                QStringLiteral("homes")),
            directory.filePath(
                QStringLiteral("shared")));
        companion::CodexAccountRouter router(
            QProcessEnvironment::systemEnvironment(),
            profiles,
            runtime,
            bindings);
        int loginStarts = 0;
        int statusChecks = 0;
        companion::CodexAccountLoginService login(
            QStringLiteral("codex.exe"),
            QProcessEnvironment::systemEnvironment(),
            runtime,
            [&loginStarts](
                const companion::CodexLoginProcessRequest&) {
                ++loginStarts;
                return companion::Result<void>::success();
            },
            [&statusChecks](
                const companion::CodexLoginProcessRequest&) {
                ++statusChecks;
                return companion::Result<
                    companion::CodexLoginProcessResult>::success({
                    0,
                    QByteArray("Logged in using ChatGPT"),
                    {},
                });
            });
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(
                    requested);
            });

        viewModel.setCodexAccountServices(
            &profiles,
            &router,
            &login);

        QVERIFY(viewModel.codexAccountsAvailable());
        QCOMPARE(
            viewModel.codexAccountProfiles().size(),
            1);
        const QVariantMap currentAccount =
            viewModel.codexAccountProfiles()
                .constFirst()
                .toMap();
        QCOMPARE(
            currentAccount.value(
                              QStringLiteral("label"))
                .toString(),
            QStringLiteral("Current Codex account"));
        QCOMPARE(
            viewModel
                .selectedCodexAccountProfileId(),
            currentAccount.value(
                              QStringLiteral("id"))
                .toString());
        QVERIFY(currentAccount.value(
                                  QStringLiteral("selected"))
                    .toBool());
        QVERIFY(viewModel.addCodexAccount(
            QStringLiteral(" Main ")));
        QCOMPARE(
            viewModel.codexAccountProfiles().size(),
            2);
        const QVariantMap profile =
            viewModel.codexAccountProfiles()
                .at(1)
                .toMap();
        QCOMPARE(
            profile.value(
                       QStringLiteral("label"))
                .toString(),
            QStringLiteral("Main"));
        QCOMPARE(
            viewModel
                .selectedCodexAccountProfileId(),
            profile.value(
                       QStringLiteral("id"))
                .toString());
        QVERIFY(
            viewModel
                .codexAccountSelectionSummary()
                .contains(
                    QStringLiteral(
                        "new Codex work"),
                    Qt::CaseInsensitive));

        viewModel.setSelectedCodexAccountProfileId(
            currentAccount.value(
                              QStringLiteral("id"))
                .toString());
        QVERIFY(!profiles.selectedProfileId()
                     .has_value());
        QCOMPARE(
            router.routeNewWork().profileId,
            std::optional<QUuid>{});
        QVERIFY(
            viewModel
                .codexAccountSelectionSummary()
                .contains(
                    QStringLiteral(
                        "Current Codex account")));

        viewModel.setSelectedCodexAccountProfileId(
            profile.value(
                       QStringLiteral("id"))
                .toString());

        QVERIFY(
            viewModel
                .beginSelectedCodexAccountLogin());
        QCOMPARE(loginStarts, 1);
        QVERIFY(
            viewModel
                .refreshSelectedCodexAccount());
        QTRY_VERIFY_WITH_TIMEOUT(
            !viewModel
                 .codexAccountRefreshInProgress(),
            3000);
        QCOMPARE(statusChecks, 1);
        QVERIFY(
            viewModel
                .codexAccountStatus()
                .contains(
                    QStringLiteral("ChatGPT"),
                    Qt::CaseInsensitive));

        viewModel
            .setAutomaticallyContinuesAcrossCodexAccounts(
                true);
        QVERIFY(
            viewModel
                .automaticallyContinuesAcrossCodexAccounts());
        const auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(
            persisted.value()
                .automaticallyContinuesAcrossCodexAccounts);
    }

    void codexAccountRemovalRefusesExistingTaskBindings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        companion::CodexAccountProfileStore profiles(
            directory.filePath(
                QStringLiteral("profiles.json")));
        const auto added = profiles.add(
            QStringLiteral("Main"));
        QVERIFY(added.hasValue());
        companion::CodexThreadAccountBindingStore bindings(
            directory.filePath(
                QStringLiteral("bindings.json")));
        QVERIFY(
            bindings
                .bind(
                    QStringLiteral("thread-main"),
                    added.value().id)
                .hasValue());
        companion::CodexAccountRuntime runtime(
            directory.filePath(
                QStringLiteral("homes")),
            directory.filePath(
                QStringLiteral("shared")));
        companion::CodexAccountRouter router(
            QProcessEnvironment::systemEnvironment(),
            profiles,
            runtime,
            bindings);
        companion::CodexAccountLoginService login(
            QStringLiteral("codex.exe"),
            QProcessEnvironment::systemEnvironment(),
            runtime);
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::success(
                    requested);
            });
        viewModel.setCodexAccountServices(
            &profiles,
            &router,
            &login);

        QVERIFY(
            !viewModel
                 .removeSelectedCodexAccount());

        QCOMPARE(profiles.profiles().size(), 1);
        QVERIFY(
            viewModel
                .codexAccountStatus()
                .contains(
                    QStringLiteral("existing"),
                    Qt::CaseInsensitive));
    }

    void mobilePairingActionsExposeOnlyNonSecretDeviceState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        TestSecretProtector protector;
        companion::PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        companion::PairingCoordinator pairing(
            pairingStore,
            pairingNow,
            [] {
                return companion::Result<QString>::
                    success(
                        QStringLiteral("123456"));
            },
            [] {
                return companion::Result<QByteArray>::
                    success(
                        QByteArray(32, '\x2a'));
            });
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        QSignalSpy pairingSpy(
            &viewModel,
            &companion::SettingsViewModel::
                mobilePairingChanged);
        QVERIFY(pairingSpy.isValid());

        viewModel.setMobilePairingCoordinator(
            &pairing);
        QCOMPARE(pairingSpy.count(), 1);
        QVERIFY(viewModel.mobilePairingAvailable());
        QVERIFY(!viewModel.mobilePairingActive());
        QVERIFY(viewModel.pairedMobileDevices().isEmpty());

        viewModel.setMobileNearbyAccessState(
            true,
            QStringLiteral(
                "Nearby Wi-Fi is available on this trusted Windows network."));
        QVERIFY(viewModel.beginMobilePairing());
        QVERIFY(viewModel.mobilePairingActive());
        QCOMPARE(
            viewModel.mobilePairingCode(),
            QStringLiteral("123 456"));
        QCOMPARE(
            viewModel.mobilePairingExpiresAtMilliseconds(),
            pairingNow().addSecs(300)
                .toMSecsSinceEpoch());

        QVERIFY(pairing.remember({
            QStringLiteral("iphone-alpha"),
            QStringLiteral("Harlin iPhone"),
            QByteArray(32, '\x37'),
            pairingNow(),
            std::nullopt,
        }).hasValue());
        const QVariantList paired =
            viewModel.pairedMobileDevices();
        QCOMPARE(paired.size(), 1);
        const QVariantMap device =
            paired.constFirst().toMap();
        QCOMPARE(
            device.value(QStringLiteral("deviceId"))
                .toString(),
            QStringLiteral("iphone-alpha"));
        QCOMPARE(
            device.value(QStringLiteral("displayName"))
                .toString(),
            QStringLiteral("Harlin iPhone"));
        QVERIFY(
            !device.contains(
                QStringLiteral("secret")));

        QVERIFY(viewModel.forgetMobileDevice(
            QStringLiteral("iphone-alpha")));
        QVERIFY(viewModel.pairedMobileDevices().isEmpty());
        viewModel.cancelMobilePairing();
        QVERIFY(!viewModel.mobilePairingActive());
        QVERIFY(pairingSpy.count() >= 4);
    }

    void mobilePairingBeginsWhileNearbyAccessIsUnavailable()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        TestSecretProtector protector;
        companion::PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        companion::PairingCoordinator pairing(
            pairingStore,
            pairingNow,
            [] {
                return companion::Result<QString>::
                    success(
                        QStringLiteral("123456"));
            });
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        viewModel.setMobilePairingCoordinator(
            &pairing);
        viewModel.setMobileNearbyAccessState(
            false,
            QStringLiteral(
                "Nearby Wi-Fi is blocked on this Public network."));
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::
                runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        QVERIFY(viewModel.beginMobilePairing());
        QCOMPARE(errorSpy.count(), 0);
        QVERIFY(viewModel.mobilePairingActive());
        QCOMPARE(
            viewModel.mobilePairingCode(),
            QStringLiteral("123 456"));
        QVERIFY(pairing.activePairing().has_value());
    }

    void secureRelayPairingLinkCopiesAndClears()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral(
                    "settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        TestSecretProtector protector;
        companion::PairingRecordStore pairingStore(
            directory.filePath(
                QStringLiteral(
                    "paired-devices.v1.json")),
            protector);
        companion::PairingCoordinator pairing(
            pairingStore,
            pairingNow,
            [] {
                return companion::
                    Result<QString>::success(
                        QStringLiteral(
                            "123456"));
            });
        companion::RelayStateStore relayState(
            directory.filePath(
                QStringLiteral(
                    "relay-state.v1.json")),
            protector);
        companion::
            RelayPairingBootstrapDependencies
                dependencies;
        dependencies.endpointFactory =
            [](
                QUrl,
                QString,
                QString) {
                return std::make_unique<
                    SettingsBootstrapEndpoint>();
            };
        dependencies.clock = pairingNow;
        dependencies.secretGenerator = [] {
            return companion::
                Result<QByteArray>::success(
                    QByteArray(32, '\x42'));
        };
        companion::RelayPairingBootstrap
            bootstrap(
                pairing,
                relayState,
                QStringLiteral(
                    "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"),
                QStringLiteral(
                    "Windows workstation"),
                std::move(dependencies));
        bootstrap.setRelayUrl(
            QUrl(QStringLiteral(
                "wss://relay.example.test/socket")));

        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        viewModel.setMobilePairingCoordinator(
            &pairing);
        viewModel.setRelayPairingBootstrap(
            &bootstrap);
        QString copied;
        viewModel
            .setCopyMobilePairingLinkCommand(
                [&copied](
                    const QString& value) {
                    copied = value;
                    return companion::
                        Result<void>::success();
                });

        QVERIFY(viewModel.beginMobilePairing());
        QVERIFY(viewModel.mobilePairingActive());
        QVERIFY(viewModel.hasMobilePairingLink());
        QVERIFY(
            !viewModel
                 .mobilePairingLink()
                 .isEmpty());
        const QString qrSource =
            viewModel.mobilePairingQrSource();
        const QString qrPrefix =
            QStringLiteral(
                "data:image/png;base64,");
        QVERIFY(qrSource.startsWith(qrPrefix));
        QImage qrImage;
        QVERIFY(
            qrImage.loadFromData(
                QByteArray::fromBase64(
                    qrSource
                        .sliced(qrPrefix.size())
                        .toLatin1()),
                "PNG"));
        QVERIFY(!qrImage.isNull());
        QCOMPARE(qrImage.width(), qrImage.height());
        QVERIFY(qrImage.width() >= 180);
        QVERIFY(viewModel.copyMobilePairingLink());
        QCOMPARE(
            copied,
            viewModel.mobilePairingLink());
        QCOMPARE(
            viewModel
                .mobilePairingLinkStatus(),
            QStringLiteral(
                "Pairing link copied."));
        QVERIFY(
            viewModel
                .mobilePairingStatusText()
                .contains(
                    QStringLiteral(
                        "short-lived pairing link"),
                    Qt::CaseInsensitive));

        viewModel
            .setCopyMobilePairingLinkCommand(
                [](
                    const QString&) {
                    return companion::
                        Result<void>::failure({
                            QStringLiteral(
                                "clipboard.unavailable"),
                            QStringLiteral(
                                "clipboard unavailable"),
                            false,
                            {},
                        });
                });
        QVERIFY(
            !viewModel
                 .copyMobilePairingLink());
        QVERIFY(
            viewModel
                .mobilePairingLinkStatus()
                .startsWith(
                    QStringLiteral(
                        "The pairing link could not be copied: ")));

        viewModel.cancelMobilePairing();
        QVERIFY(
            !viewModel
                 .hasMobilePairingLink());
        QVERIFY(
            viewModel
                .mobilePairingLink()
                .isEmpty());
        QVERIFY(
            viewModel
                .mobilePairingQrSource()
                .isEmpty());
        QVERIFY(
            viewModel
                .mobilePairingLinkStatus()
                .isEmpty());
    }

    void nearbyAccessStatePublishesLiveWindowsAvailability()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        QSignalSpy changedSpy(
            &viewModel,
            &companion::SettingsViewModel::
                mobileNearbyAccessChanged);
        QVERIFY(changedSpy.isValid());

        viewModel.setMobileNearbyAccessState(
            false,
            QStringLiteral(
                "Nearby Wi-Fi is blocked on this Public Windows network."));

        QVERIFY(
            !viewModel
                 .mobileNearbyAccessAvailable());
        QCOMPARE(
            viewModel
                .mobileNearbyAccessStatusText(),
            QStringLiteral(
                "Nearby Wi-Fi is blocked on this Public Windows network."));
        QCOMPARE(changedSpy.count(), 1);

        viewModel.setMobileNearbyAccessState(
            true,
            QStringLiteral(
                "Nearby Wi-Fi is available on this trusted Windows network."));

        QVERIFY(
            viewModel
                .mobileNearbyAccessAvailable());
        QCOMPARE(changedSpy.count(), 2);
    }

    void mobileAndRelaySettingsPersistAndRejectUnsafeRelayUrls()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        QSignalSpy settingsSpy(
            &viewModel,
            &companion::SettingsViewModel::
                mobileConfigurationChanged);
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::
                runtimeErrorOccurred);
        QVERIFY(settingsSpy.isValid());
        QVERIFY(errorSpy.isValid());

        viewModel.setMobileEnabled(false);
        viewModel.setKeepAvailableWhileDisplayOff(
            false);
        viewModel
            .setAllowNearbyOnPublicNetworks(
                true);
        QVERIFY(!viewModel.mobileEnabled());
        QVERIFY(
            !viewModel
                 .keepAvailableWhileDisplayOff());
        QVERIFY(
            viewModel
                .allowNearbyOnPublicNetworks());

        QVERIFY(!viewModel.saveRelayUrl(
            QStringLiteral(
                "http://relay.example.test")));
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(
            viewModel.relayUrl(),
            QStringLiteral(
                "wss://codex-companion-relay."
                "silverfire-codex-companion."
                "workers.dev/relay"));

        QVERIFY(viewModel.saveRelayUrl(
            QStringLiteral(
                "wss://relay.example.test/socket")));
        QCOMPARE(
            viewModel.relayUrl(),
            QStringLiteral(
                "wss://relay.example.test/socket"));
        QVERIFY(viewModel.relayDisableAvailable());

        QVERIFY(viewModel.disableRelay());
        QVERIFY(viewModel.relayUrl().isEmpty());
        QVERIFY(!viewModel.relayDisableAvailable());

        const auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(!persisted.value().mobileEnabled);
        QVERIFY(
            !persisted.value()
                 .keepAvailableWhileDisplayOff);
        QVERIFY(
            persisted.value()
                .allowNearbyOnPublicNetworks);
        QCOMPARE(
            persisted.value().relayMode,
            companion::RelayMode::Disabled);
        QVERIFY(
            persisted.value()
                .customRelayUrl.isEmpty());
        QCOMPARE(settingsSpy.count(), 5);
    }

    void automaticRelayRestoresBundledEndpointAndClearsCustomOverride()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        loaded.relayMode =
            companion::RelayMode::Custom;
        loaded.customRelayUrl =
            QStringLiteral(
                "wss://relay.example.test/socket");
        QVERIFY(repository.save(loaded).hasValue());

        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });

        QVERIFY(
            viewModel
                .relayAutomaticAvailable());
        QVERIFY(viewModel.useAutomaticRelay());
        QCOMPARE(
            viewModel.relayUrl(),
            QStringLiteral(
                "wss://codex-companion-relay."
                "silverfire-codex-companion."
                "workers.dev/relay"));
        QCOMPARE(
            viewModel.relayStatusText(),
            QStringLiteral(
                "Automatic secure relay restored. Reconnect the paired phone nearby once to synchronize it."));

        const auto persisted =
            repository.load();
        QVERIFY(persisted.hasValue());
        QCOMPARE(
            persisted.value().relayMode,
            companion::RelayMode::Automatic);
        QVERIFY(
            persisted.value()
                .customRelayUrl.isEmpty());
    }

    void failedWindowsAvailabilityRequestRollsBackTheSetting()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        companion::SettingsRepository repository(
            directory.filePath(
                QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        QVERIFY(repository.save(loaded).hasValue());
        companion::SettingsViewModel viewModel(
            loaded,
            repository,
            [](companion::BackdropMode requested) {
                return companion::Result<
                    companion::BackdropMode>::
                    success(requested);
            });
        int commandCalls = 0;
        bool requestedMobileEnabled = false;
        bool requestedKeepAvailable = true;
        viewModel.setMobileAvailabilityCommand(
            [&commandCalls,
             &requestedMobileEnabled,
             &requestedKeepAvailable](
                bool mobileEnabled,
                bool keepAvailable) {
                ++commandCalls;
                requestedMobileEnabled =
                    mobileEnabled;
                requestedKeepAvailable =
                    keepAvailable;
                return companion::Result<void>::
                    failure({
                        QStringLiteral(
                            "power.request-set-failed"),
                        QStringLiteral(
                            "Windows rejected the availability request."),
                        false,
                        {},
                    });
            });
        QSignalSpy errorSpy(
            &viewModel,
            &companion::SettingsViewModel::
                runtimeErrorOccurred);
        QVERIFY(errorSpy.isValid());

        viewModel.setKeepAvailableWhileDisplayOff(
            false);

        QCOMPARE(commandCalls, 1);
        QVERIFY(requestedMobileEnabled);
        QVERIFY(!requestedKeepAvailable);
        QVERIFY(
            viewModel
                .keepAvailableWhileDisplayOff());
        QCOMPARE(errorSpy.count(), 1);
        const auto persisted = repository.load();
        QVERIFY(persisted.hasValue());
        QVERIFY(
            persisted.value()
                .keepAvailableWhileDisplayOff);
    }
};

QTEST_GUILESS_MAIN(SettingsViewModelTests)
#include "SettingsViewModelTests.moc"
