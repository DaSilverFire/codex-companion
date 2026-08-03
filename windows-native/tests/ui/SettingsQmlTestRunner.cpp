#include "core/AppSettings.h"
#include "core/SettingsRepository.h"
#include "platform/windows/DpapiCredentialStore.h"
#include "ui/SettingsViewModel.h"

#include <QQmlContext>
#include <QQmlEngine>
#include <QPointer>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>
#include <memory>
#include <vector>

class SettingsQmlTestSupport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString lastPersistedBackdropMode READ lastPersistedBackdropMode NOTIFY persistedSettingsChanged)

public:
    explicit SettingsQmlTestSupport(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    Q_INVOKABLE QObject* createViewModel()
    {
        auto bundle = std::make_unique<ModelBundle>();
        bundle->directory = std::make_unique<QTemporaryDir>();
        if (!bundle->directory->isValid()) {
            return nullptr;
        }

        bundle->repository = std::make_unique<companion::SettingsRepository>(
            bundle->directory->filePath(QStringLiteral("settings.ini")));
        companion::AppSettings loaded;
        const auto saved = bundle->repository->save(loaded);
        if (!saved.hasValue()) {
            return nullptr;
        }
        bundle->credentialStore =
            std::make_shared<
                companion::DpapiCredentialStore>(
                    bundle->directory->filePath(
                        QStringLiteral(
                            "Credentials")),
                    [](const QString&, bool) {
                        return companion::Result<
                            void>::success();
                    });

        bundle->viewModel = std::make_unique<companion::SettingsViewModel>(
            loaded,
            *bundle->repository,
            [](companion::BackdropMode requested) {
                return companion::Result<companion::BackdropMode>::success(requested);
            },
            bundle->credentialStore);
        bundle->viewModel->setParent(this);

        QObject* viewModel = bundle->viewModel.get();
        bundles_.push_back(std::move(bundle));
        return viewModel;
    }

    Q_INVOKABLE void watchModel(QObject* viewModel)
    {
        watchedModel_ = viewModel;
        emit persistedSettingsChanged();
    }

    QString lastPersistedBackdropMode() const
    {
        return persistedBackdropMode(watchedModel_);
    }

    Q_INVOKABLE QString persistedBackdropMode(QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return {};
        }
        const auto loaded = bundle->repository->load();
        if (!loaded.hasValue()) {
            return {};
        }
        switch (loaded.value().backdrop) {
        case companion::BackdropMode::Mica:
            return QStringLiteral("mica");
        case companion::BackdropMode::WindowsGlass:
            return QStringLiteral("windows-glass");
        case companion::BackdropMode::SolidBlack:
            return QStringLiteral("solid-black");
        }
        return {};
    }

    Q_INVOKABLE double persistedAnimationSpeedScale(QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return -1.0;
        }
        const auto loaded = bundle->repository->load();
        return loaded.hasValue() ? loaded.value().animationSpeedScale : -1.0;
    }

    Q_INVOKABLE bool persistedHideControlsUntilHover(QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return false;
        }
        const auto loaded = bundle->repository->load();
        return loaded.hasValue() && loaded.value().hideControlsUntilHover;
    }

    Q_INVOKABLE bool persistedAllowAutonomousMovement(QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return false;
        }
        const auto loaded = bundle->repository->load();
        return loaded.hasValue() && loaded.value().allowAutonomousMovement;
    }

    Q_INVOKABLE bool persistedAllowNearbyOnPublicNetworks(
        QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return false;
        }
        const auto loaded =
            bundle->repository->load();
        return loaded.hasValue()
            && loaded.value()
                   .allowNearbyOnPublicNetworks;
    }

    Q_INVOKABLE bool
    persistedAutomaticCodexAccountContinuation(
        QObject* viewModel) const
    {
        const auto* bundle = bundleFor(viewModel);
        if (bundle == nullptr) {
            return false;
        }
        const auto loaded =
            bundle->repository->load();
        return loaded.hasValue()
            && loaded.value()
                   .automaticallyContinuesAcrossCodexAccounts;
    }

signals:
    void persistedSettingsChanged();

private:
    struct ModelBundle final {
        std::unique_ptr<QTemporaryDir> directory;
        std::unique_ptr<companion::SettingsRepository> repository;
        std::shared_ptr<
            companion::DpapiCredentialStore>
            credentialStore;
        std::unique_ptr<companion::SettingsViewModel> viewModel;
    };

    const ModelBundle* bundleFor(QObject* viewModel) const
    {
        for (const auto& bundle : bundles_) {
            if (bundle->viewModel.get() == viewModel) {
                return bundle.get();
            }
        }
        return nullptr;
    }

    std::vector<std::unique_ptr<ModelBundle>> bundles_;
    QPointer<QObject> watchedModel_;
};

class SettingsQmlSetup final : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine* engine)
    {
        engine->rootContext()->setContextProperty(
            QStringLiteral("settingsTestSupport"),
            &support_);
    }

private:
    SettingsQmlTestSupport support_;
};

QUICK_TEST_MAIN_WITH_SETUP(companion_ui_settings_qml, SettingsQmlSetup)

#include "SettingsQmlTestRunner.moc"
