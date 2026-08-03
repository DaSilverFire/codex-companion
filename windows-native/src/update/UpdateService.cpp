#include "update/UpdateService.h"

#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/UpdateArtifactDownloader.h"
#include "update/UpdateBuildConfiguration.h"
#include "update/UpdateFeedClient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <utility>

namespace companion {
namespace {

CompanionError updateError(
    QString code,
    QString message,
    bool retryable = false,
    QVariantMap context = {})
{
    return {
        std::move(code),
        std::move(message),
        retryable,
        std::move(context),
    };
}

QString requiredString(
    const QJsonObject& object,
    QStringView name,
    bool& valid)
{
    const QString key =
        name.toString();
    const QJsonValue value =
        object.value(key);
    if (!value.isString()) {
        valid = false;
        return {};
    }
    return value.toString().trimmed();
}

bool isHttpsUrl(const QUrl& url)
{
    return url.isValid()
        && !url.isRelative()
        && url.scheme().compare(
               QStringLiteral("https"),
               Qt::CaseInsensitive) == 0
        && !url.host().isEmpty()
        && url.userInfo().isEmpty();
}

bool isHexDigest(QStringView value)
{
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](QChar character) {
            return (character
                        >= QLatin1Char('0')
                    && character
                        <= QLatin1Char('9'))
                || (character
                        >= QLatin1Char('a')
                    && character
                        <= QLatin1Char('f'))
                || (character
                        >= QLatin1Char('A')
                    && character
                        <= QLatin1Char('F'));
        });
}

bool phaseAllowsCheck(UpdatePhase phase)
{
    return phase == UpdatePhase::Idle
        || phase == UpdatePhase::UpToDate
        || phase == UpdatePhase::Failed
        || phase == UpdatePhase::Available
        || phase
            == UpdatePhase::ReadyToInstall;
}

QString availableDetail(
    const UpdateManifest& manifest)
{
    return QStringLiteral(
               "Version %1 (build %2) is available.")
        .arg(manifest.version)
        .arg(manifest.build);
}

template <typename T>
QFuture<Result<T>> readyFuture(
    Result<T> result)
{
    QPromise<Result<T>> promise;
    promise.start();
    QFuture<Result<T>> future =
        promise.future();
    promise.addResult(std::move(result));
    promise.finish();
    return future;
}

class ProductionUpdateBackend final {
public:
    explicit ProductionUpdateBackend(
        const UpdateServiceOptions& options,
        UpdateInstallerLaunch
            installerLaunch)
        : feedClient_(
              network_,
              UpdateFeedClientOptions{
                  options.channel
                      .publicKeyBase64,
                  options.userAgent,
              }),
          artifactDownloader_(
              network_,
              UpdateArtifactDownloaderOptions{
                  options.userAgent,
              }),
          installerLaunch_(
              std::move(
                  installerLaunch))
    {
        const auto currentWindows =
            WindowsVersionProvider().current();
        if (!currentWindows.hasValue()) {
            initializationError_ =
                currentWindows.error();
            return;
        }

        UpdateArtifactStoreOptions
            storeOptions;
        storeOptions.rootPath =
            options.artifactRoot;
        storeOptions.currentWindowsVersion =
            currentWindows.value();
        storeOptions.allowedSignerSha256 =
            options.channel
                .allowedSignerSha256;
        artifactStore_ =
            std::make_unique<
                UpdateArtifactStore>(
                std::move(
                    storeOptions));
    }

    UpdateServiceDependencies
    dependencies(
        const UpdateServiceOptions& options,
        const std::shared_ptr<
            ProductionUpdateBackend>& self)
    {
        UpdateServiceDependencies
            dependencies;
        dependencies.requestManifest =
            [self, url =
                       options.channel
                           .manifestUrl] {
                return self->feedClient_
                    .loadManifest(url);
            };
        dependencies.requestArtifact =
            [self](
                const UpdateManifest& manifest,
                UpdateDownloadProgress
                    progress) {
                if (self->initializationError_
                        .has_value()) {
                    return readyFuture<
                        VerifiedArtifact>(
                        Result<
                            VerifiedArtifact>::
                            failure(
                                *self
                                     ->initializationError_));
                }
                if (!self->artifactStore_) {
                    return readyFuture<
                        VerifiedArtifact>(
                        Result<
                            VerifiedArtifact>::
                            failure(
                                updateError(
                                    QStringLiteral(
                                        "update.artifact_store_unavailable"),
                                    QStringLiteral(
                                        "The Windows update staging area is unavailable."))));
                }
                return self
                    ->artifactDownloader_
                    .download(
                        manifest,
                        *self
                             ->artifactStore_,
                        std::move(
                            progress));
            };
        dependencies.launchInstaller =
            [self](
                const UpdateManifest&
                    manifest,
                const VerifiedArtifact&
                    artifact) {
                if (!self
                         ->installerLaunch_) {
                    return Result<void>::
                        failure(
                            updateError(
                                QStringLiteral(
                                    "update.installer_handoff_unavailable"),
                                QStringLiteral(
                                    "The detached Windows updater is unavailable.")));
                }
                return self
                    ->installerLaunch_(
                        manifest,
                        artifact);
            };
        dependencies.prune =
            [self](QStringView activePath) {
                if (self
                        ->initializationError_
                        .has_value()) {
                    return Result<void>::
                        failure(
                            *self
                                 ->initializationError_);
                }
                if (!self->artifactStore_) {
                    return Result<void>::
                        failure(
                            updateError(
                                QStringLiteral(
                                    "update.artifact_store_unavailable"),
                                QStringLiteral(
                                    "The Windows update staging area is unavailable.")));
                }
                return self
                    ->artifactStore_->prune(
                        activePath);
            };
        dependencies.cancel =
            [self] {
                self->feedClient_.cancel();
                self->artifactDownloader_
                    .cancel();
            };
        dependencies.lifetime = self;
        return dependencies;
    }

private:
    QNetworkAccessManager network_;
    UpdateFeedClient feedClient_;
    UpdateArtifactDownloader
        artifactDownloader_;
    std::unique_ptr<
        UpdateArtifactStore>
        artifactStore_;
    std::optional<CompanionError>
        initializationError_;
    UpdateInstallerLaunch
        installerLaunch_;
};

} // namespace

QString updatePhaseName(UpdatePhase phase)
{
    switch (phase) {
    case UpdatePhase::Idle:
        return QStringLiteral("idle");
    case UpdatePhase::Checking:
        return QStringLiteral("checking");
    case UpdatePhase::Unavailable:
        return QStringLiteral("unavailable");
    case UpdatePhase::UpToDate:
        return QStringLiteral("up-to-date");
    case UpdatePhase::Available:
        return QStringLiteral("available");
    case UpdatePhase::Downloading:
        return QStringLiteral("downloading");
    case UpdatePhase::ReadyToInstall:
        return QStringLiteral(
            "ready-to-install");
    case UpdatePhase::Installing:
        return QStringLiteral("installing");
    case UpdatePhase::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

Result<std::optional<QUrl>>
updateManifestUrlOverrideFromArguments(
    const QStringList& arguments)
{
    std::optional<QUrl> override;
    for (qsizetype index = 1;
         index < arguments.size();
         ++index) {
        if (arguments.at(index)
            != QStringLiteral(
                "--update-manifest-url")) {
            continue;
        }
        if (override.has_value()
            || index + 1
                >= arguments.size()) {
            return Result<
                std::optional<QUrl>>::
                failure(
                    updateError(
                        QStringLiteral(
                            "update.feed_override_argument_invalid"),
                        QStringLiteral(
                            "The update manifest URL override argument is invalid.")));
        }
        const QUrl candidate(
            arguments.at(++index)
                .trimmed());
        if (!isHttpsUrl(candidate)
            || !candidate.fragment()
                    .isEmpty()) {
            return Result<
                std::optional<QUrl>>::
                failure(
                    updateError(
                        QStringLiteral(
                            "update.feed_override_argument_invalid"),
                        QStringLiteral(
                            "The update manifest URL override must use HTTPS without credentials or a fragment.")));
        }
        override = candidate;
    }
    return Result<
        std::optional<QUrl>>::
        success(
            std::move(override));
}

Result<UpdateChannelConfiguration>
UpdateChannelConfiguration::decode(
    QByteArrayView bytes)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(
            QByteArray(
                bytes.data(),
                bytes.size()),
            &parseError);
    if (parseError.error
            != QJsonParseError::NoError
        || !document.isObject()) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_invalid"),
                    QStringLiteral(
                        "The bundled Windows update channel is invalid.")));
    }

    const QJsonObject object =
        document.object();
    const QSet<QString> expected{
        QStringLiteral("manifestUrl"),
        QStringLiteral("publicKeyBase64"),
        QStringLiteral("signerSha256"),
        QStringLiteral("platform"),
        QStringLiteral("architecture"),
        QStringLiteral(
            "minimumSystemVersion"),
    };
    for (auto entry = object.begin();
         entry != object.end();
         ++entry) {
        if (!expected.contains(
                entry.key())) {
            return Result<
                UpdateChannelConfiguration>::
                failure(
                    updateError(
                        QStringLiteral(
                            "update.channel_invalid"),
                        QStringLiteral(
                            "The bundled Windows update channel contains an unknown field."),
                        false,
                        {
                            {
                                QStringLiteral(
                                    "field"),
                                entry.key(),
                            },
                        }));
        }
    }

    bool valid = true;
    const QString manifestUrl =
        requiredString(
            object,
            u"manifestUrl",
            valid);
    const QString publicKey =
        requiredString(
            object,
            u"publicKeyBase64",
            valid);
    const QString signerList =
        requiredString(
            object,
            u"signerSha256",
            valid);
    const QString platform =
        requiredString(
            object,
            u"platform",
            valid);
    const QString architecture =
        requiredString(
            object,
            u"architecture",
            valid);
    const QString minimumSystemVersion =
        requiredString(
            object,
            u"minimumSystemVersion",
            valid);
    if (!valid) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_invalid"),
                    QStringLiteral(
                        "The bundled Windows update channel has an invalid field type.")));
    }

    const bool hasManifest =
        !manifestUrl.isEmpty();
    const bool hasKey =
        !publicKey.isEmpty();
    const bool hasSigner =
        !signerList.isEmpty();
    if (!hasManifest
        && !hasKey
        && !hasSigner) {
        UpdateChannelConfiguration
            configuration;
        configuration.platform =
            platform;
        configuration.architecture =
            architecture;
        configuration.minimumSystemVersion =
            minimumSystemVersion;
        return Result<
            UpdateChannelConfiguration>::
            success(
                std::move(
                    configuration));
    }
    if (!hasManifest
        || !hasKey
        || !hasSigner) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_incomplete"),
                    QStringLiteral(
                        "The Windows update channel trust configuration is incomplete.")));
    }

    const QUrl url(manifestUrl);
    if (!isHttpsUrl(url)) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_insecure_url"),
                    QStringLiteral(
                        "The Windows update channel must use HTTPS.")));
    }

    const QByteArray decodedKey =
        QByteArray::fromBase64(
            publicKey.toLatin1(),
            QByteArray::
                AbortOnBase64DecodingErrors);
    if (decodedKey.size() != 32) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_invalid_key"),
                    QStringLiteral(
                        "The Windows update channel public key is invalid.")));
    }
    if (platform
            != QStringLiteral("windows")
        || architecture
            != QStringLiteral("x64")
        || !WindowsVersion::parse(
                minimumSystemVersion)
                .has_value()) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_platform_mismatch"),
                    QStringLiteral(
                        "The bundled update channel does not target this Windows build.")));
    }

    QStringList signers;
    for (const QString& raw :
         signerList.split(
             QLatin1Char(','),
             Qt::SkipEmptyParts)) {
        const QString normalized =
            AuthenticodeVerifier::
                normalizeThumbprint(raw);
        if (!isHexDigest(normalized)
            || signers.contains(
                normalized,
                Qt::CaseInsensitive)) {
            return Result<
                UpdateChannelConfiguration>::
                failure(
                    updateError(
                        QStringLiteral(
                            "update.channel_invalid_signer"),
                        QStringLiteral(
                            "The Windows update channel signer allowlist is invalid.")));
        }
        signers.append(normalized);
    }
    if (signers.isEmpty()) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_invalid_signer"),
                    QStringLiteral(
                        "The Windows update channel signer allowlist is invalid.")));
    }

    UpdateChannelConfiguration
        configuration;
    configuration.configured = true;
    configuration.manifestUrl = url;
    configuration.publicKeyBase64 =
        publicKey;
    configuration.allowedSignerSha256 =
        std::move(signers);
    configuration.platform =
        platform;
    configuration.architecture =
        architecture;
    configuration.minimumSystemVersion =
        minimumSystemVersion;
    return Result<
        UpdateChannelConfiguration>::
        success(
            std::move(configuration));
}

Result<UpdateChannelConfiguration>
UpdateChannelConfiguration::
fromBundledResource()
{
    QFile file(
        QStringLiteral(
            ":/codex-companion/"
            "update-channel.json"));
    if (!file.open(
            QIODevice::ReadOnly)) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.channel_unavailable"),
                    QStringLiteral(
                        "The bundled Windows update channel is unavailable.")));
    }
    return decode(file.readAll());
}

Result<UpdateChannelConfiguration>
UpdateChannelConfiguration::
withManifestUrlOverride(
    QUrl manifestUrlOverride) const
{
    if (!configured
        || publicKeyBase64.isEmpty()
        || allowedSignerSha256
               .isEmpty()) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.feed_override_untrusted"),
                    QStringLiteral(
                        "The update manifest URL override requires bundled signing trust.")));
    }
    if (!isHttpsUrl(
            manifestUrlOverride)
        || !manifestUrlOverride
                .fragment()
                .isEmpty()) {
        return Result<
            UpdateChannelConfiguration>::
            failure(
                updateError(
                    QStringLiteral(
                        "update.feed_override_invalid"),
                    QStringLiteral(
                        "The update manifest URL override must use HTTPS without credentials or a fragment.")));
    }

    UpdateChannelConfiguration
        overridden = *this;
    overridden.manifestUrl =
        std::move(
            manifestUrlOverride);
    return Result<
        UpdateChannelConfiguration>::
        success(
            std::move(overridden));
}

UpdateService::UpdateService(
    UpdateServiceOptions options,
    UpdateServiceDependencies dependencies,
    QObject* parent)
    : QObject(parent),
      options_(std::move(options)),
      dependencies_(
          std::move(dependencies))
{
    qRegisterMetaType<CompanionError>(
        "companion::CompanionError");
    qRegisterMetaType<UpdatePhase>(
        "companion::UpdatePhase");

    snapshot_.installedVersion =
        options_.installedVersion;
    snapshot_.installedBuild =
        options_.installedBuild;
    if (!options_.channel.configured
        || !dependencies_.requestManifest
        || !dependencies_.requestArtifact
        || !dependencies_.launchInstaller) {
        snapshot_.phase =
            UpdatePhase::Unavailable;
        snapshot_.title =
            QStringLiteral(
                "Updates unavailable");
        snapshot_.detail =
            options_.unavailableDetail;
        snapshot_.errorCode =
            QStringLiteral(
                "update.channel_unconfigured");
        return;
    }

    snapshot_.phase =
        UpdatePhase::Idle;
    snapshot_.title =
        QStringLiteral(
            "Windows updates");
    snapshot_.detail =
        QStringLiteral(
            "Updates are checked against the configured signed Windows release channel.");
}

UpdateService::~UpdateService()
{
    cancelActiveOperation();
}

const UpdateServiceSnapshot&
UpdateService::snapshot() const noexcept
{
    return snapshot_;
}

Result<void>
UpdateService::invalidState(
    QStringView operation) const
{
    return Result<void>::failure(
        updateError(
            QStringLiteral(
                "update.invalid_state"),
            QStringLiteral(
                "The update action is not available in the current state."),
            false,
            {
                {
                    QStringLiteral(
                        "operation"),
                    operation.toString(),
                },
                {
                    QStringLiteral(
                        "phase"),
                    updatePhaseName(
                        snapshot_.phase),
                },
            }));
}

Result<void>
UpdateService::checkForUpdates()
{
    if (!phaseAllowsCheck(
            snapshot_.phase)) {
        return invalidState(
            u"check");
    }

    cancelActiveOperation();
    availableManifest_.reset();
    readyArtifact_.reset();
    if (dependencies_.prune) {
        const auto pruned =
            dependencies_.prune({});
        if (!pruned.hasValue()) {
            publishFailure(
                pruned.error());
            return Result<void>::failure(
                pruned.error());
        }
    }

    const quint64 generation =
        ++operationGeneration_;
    UpdateServiceSnapshot checking =
        snapshot_;
    checking.phase =
        UpdatePhase::Checking;
    checking.title =
        QStringLiteral(
            "Checking for updates");
    checking.detail =
        QStringLiteral(
            "Checking the signed Windows release channel...");
    checking.availableVersion.clear();
    checking.availableBuild = 0;
    checking.downloadProgress = 0;
    checking.errorCode.clear();
    publish(std::move(checking));

    QFuture<Result<UpdateManifest>>
        future;
    try {
        future =
            dependencies_.requestManifest();
    } catch (const std::exception&
                 exception) {
        const CompanionError failure =
            updateError(
                QStringLiteral(
                    "update.manifest_request_failed"),
                QString::fromUtf8(
                    exception.what()),
                true);
        publishFailure(failure);
        return Result<void>::failure(
            failure);
    } catch (...) {
        const CompanionError failure =
            updateError(
                QStringLiteral(
                    "update.manifest_request_failed"),
                QStringLiteral(
                    "The update manifest request could not be started."),
                true);
        publishFailure(failure);
        return Result<void>::failure(
            failure);
    }

    auto* watcher =
        new QFutureWatcher<
            Result<UpdateManifest>>(this);
    connect(
        watcher,
        &QFutureWatcher<
            Result<UpdateManifest>>::finished,
        this,
        [this, watcher, generation] {
            const auto result =
                watcher->result();
            watcher->deleteLater();
            finishManifestRequest(
                generation,
                result);
        });
    watcher->setFuture(future);
    return Result<void>::success();
}

Result<void>
UpdateService::downloadAvailableUpdate()
{
    if (snapshot_.phase
            != UpdatePhase::Available
        || !availableManifest_
                .has_value()) {
        return invalidState(
            u"download");
    }

    cancelActiveOperation();
    readyArtifact_.reset();
    const UpdateManifest manifest =
        *availableManifest_;
    const quint64 generation =
        ++operationGeneration_;
    UpdateServiceSnapshot downloading =
        snapshot_;
    downloading.phase =
        UpdatePhase::Downloading;
    downloading.title =
        QStringLiteral(
            "Downloading update");
    downloading.detail =
        QStringLiteral(
            "Downloading version %1...")
            .arg(manifest.version);
    downloading.downloadProgress = 0;
    downloading.errorCode.clear();
    publish(std::move(downloading));

    QPointer<UpdateService> self(this);
    const UpdateDownloadProgress progress =
        [self, generation](
            qint64 received,
            qint64 total) {
            if (self.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(
                self,
                [self,
                 generation,
                 received,
                 total] {
                    if (!self.isNull()) {
                        self
                            ->publishDownloadProgress(
                                generation,
                                received,
                                total);
                    }
                },
                Qt::QueuedConnection);
        };

    QFuture<Result<VerifiedArtifact>>
        future;
    try {
        future =
            dependencies_.requestArtifact(
                manifest,
                progress);
    } catch (const std::exception&
                 exception) {
        const CompanionError failure =
            updateError(
                QStringLiteral(
                    "update.artifact_request_failed"),
                QString::fromUtf8(
                    exception.what()),
                true);
        publishFailure(failure);
        return Result<void>::failure(
            failure);
    } catch (...) {
        const CompanionError failure =
            updateError(
                QStringLiteral(
                    "update.artifact_request_failed"),
                QStringLiteral(
                    "The update download could not be started."),
                true);
        publishFailure(failure);
        return Result<void>::failure(
            failure);
    }

    auto* watcher =
        new QFutureWatcher<
            Result<VerifiedArtifact>>(
                this);
    connect(
        watcher,
        &QFutureWatcher<
            Result<VerifiedArtifact>>::
            finished,
        this,
        [this, watcher, generation] {
            const auto result =
                watcher->result();
            watcher->deleteLater();
            finishArtifactRequest(
                generation,
                result);
        });
    watcher->setFuture(future);
    return Result<void>::success();
}

Result<void>
UpdateService::installReadyUpdate()
{
    if (snapshot_.phase
            != UpdatePhase::
                ReadyToInstall
        || !readyArtifact_.has_value()
        || !availableManifest_
                .has_value()) {
        return invalidState(
            u"install");
    }

    cancelActiveOperation();
    const VerifiedArtifact artifact =
        *readyArtifact_;
    UpdateServiceSnapshot installing =
        snapshot_;
    installing.phase =
        UpdatePhase::Installing;
    installing.title =
        QStringLiteral(
            "Installing update");
    installing.detail =
        QStringLiteral(
            "Launching the verified Windows installer...");
    installing.errorCode.clear();
    publish(std::move(installing));

    Result<void> launched =
        Result<void>::failure(
            updateError(
                QStringLiteral(
                    "update.installer_launch_failed"),
                QStringLiteral(
                    "The verified Windows installer could not be launched.")));
    try {
        launched =
            dependencies_.launchInstaller(
                *availableManifest_,
                artifact);
    } catch (const std::exception&
                 exception) {
        launched =
            Result<void>::failure(
                updateError(
                    QStringLiteral(
                        "update.installer_launch_failed"),
                    QString::fromUtf8(
                        exception.what())));
    } catch (...) {
    }
    if (!launched.hasValue()) {
        publishFailure(
            launched.error());
        return launched;
    }

    emit installLaunched(
        artifact.path);
    return Result<void>::success();
}

void UpdateService::publish(
    UpdateServiceSnapshot snapshot)
{
    if (snapshot_ == snapshot) {
        return;
    }
    snapshot_ =
        std::move(snapshot);
    emit stateChanged();
}

void UpdateService::publishFailure(
    const CompanionError& error)
{
    readyArtifact_.reset();
    UpdateServiceSnapshot failed =
        snapshot_;
    failed.phase =
        UpdatePhase::Failed;
    failed.title =
        QStringLiteral(
            "Update failed");
    failed.detail =
        error.message;
    failed.downloadProgress = 0;
    failed.errorCode =
        error.code;
    publish(std::move(failed));
    emit runtimeErrorOccurred(
        error);
}

void UpdateService::finishManifestRequest(
    quint64 generation,
    Result<UpdateManifest> result)
{
    if (generation
            != operationGeneration_
        || snapshot_.phase
            != UpdatePhase::Checking) {
        return;
    }
    if (!result.hasValue()) {
        publishFailure(
            result.error());
        return;
    }

    const UpdateManifest manifest =
        result.value();
    if (!manifest.isNewerThan(
            options_.installedVersion,
            options_.installedBuild)) {
        availableManifest_.reset();
        UpdateServiceSnapshot current =
            snapshot_;
        current.phase =
            UpdatePhase::UpToDate;
        current.title =
            QStringLiteral(
                "Codex Companion is up to date");
        current.detail =
            QStringLiteral(
                "Version %1 (build %2) is installed.")
                .arg(
                    options_
                        .installedVersion)
                .arg(
                    options_
                        .installedBuild);
        current.availableVersion.clear();
        current.availableBuild = 0;
        current.errorCode.clear();
        publish(std::move(current));
        return;
    }

    availableManifest_ =
        manifest;
    UpdateServiceSnapshot available =
        snapshot_;
    available.phase =
        UpdatePhase::Available;
    available.title =
        QStringLiteral(
            "Update available");
    available.detail =
        availableDetail(manifest);
    available.availableVersion =
        manifest.version;
    available.availableBuild =
        manifest.build;
    available.errorCode.clear();
    publish(std::move(available));
}

void UpdateService::finishArtifactRequest(
    quint64 generation,
    Result<VerifiedArtifact> result)
{
    if (generation
            != operationGeneration_
        || snapshot_.phase
            != UpdatePhase::Downloading) {
        return;
    }
    if (!result.hasValue()) {
        publishFailure(
            result.error());
        return;
    }

    readyArtifact_ =
        result.value();
    if (dependencies_.prune) {
        const auto pruned =
            dependencies_.prune(
                readyArtifact_->path);
        if (!pruned.hasValue()) {
            emit runtimeErrorOccurred(
                pruned.error());
        }
    }
    UpdateServiceSnapshot ready =
        snapshot_;
    ready.phase =
        UpdatePhase::ReadyToInstall;
    ready.title =
        QStringLiteral(
            "Update ready");
    ready.detail =
        QStringLiteral(
            "Version %1 is verified and ready to install.")
            .arg(
                availableManifest_
                        .has_value()
                    ? availableManifest_
                          ->version
                    : QString());
    ready.downloadProgress = 1;
    ready.errorCode.clear();
    publish(std::move(ready));
}

void UpdateService::publishDownloadProgress(
    quint64 generation,
    qint64 received,
    qint64 total)
{
    if (generation
            != operationGeneration_
        || snapshot_.phase
            != UpdatePhase::Downloading
        || total <= 0) {
        return;
    }
    UpdateServiceSnapshot progress =
        snapshot_;
    progress.downloadProgress =
        std::clamp(
            static_cast<double>(
                received)
                / static_cast<double>(
                    total),
            0.0,
            1.0);
    publish(std::move(progress));
}

void UpdateService::
cancelActiveOperation() noexcept
{
    ++operationGeneration_;
    if (!dependencies_.cancel) {
        return;
    }
    try {
        dependencies_.cancel();
    } catch (...) {
    }
}

std::unique_ptr<UpdateService>
createProductionUpdateService(
    UpdateInstallerLaunch
        installerLaunch,
    QObject* parent,
    std::optional<QUrl>
        manifestUrlOverride)
{
    UpdateServiceOptions options;
    options.installedVersion =
        QStringLiteral(
            COMPANION_WINDOWS_VERSION);
    options.installedBuild =
        COMPANION_WINDOWS_BUILD;
    options.artifactRoot =
        UpdateArtifactStore::
            defaultRootPath();
    options.userAgent =
        QStringLiteral(
            "CodexCompanionWindows/%1 (%2)")
            .arg(
                options.installedVersion)
            .arg(
                options.installedBuild)
            .toUtf8();

    const auto channel =
        UpdateChannelConfiguration::
            fromBundledResource();
    if (!channel.hasValue()) {
        options.unavailableDetail =
            channel.error().message;
        return std::make_unique<
            UpdateService>(
                std::move(options),
                UpdateServiceDependencies{},
                parent);
    }
    if (manifestUrlOverride
            .has_value()) {
        const auto overridden =
            channel.value()
                .withManifestUrlOverride(
                    std::move(
                        *manifestUrlOverride));
        if (!overridden.hasValue()) {
            options.unavailableDetail =
                overridden.error()
                    .message;
            return std::make_unique<
                UpdateService>(
                    std::move(options),
                    UpdateServiceDependencies{},
                    parent);
        }
        options.channel =
            overridden.value();
    } else {
        options.channel =
            channel.value();
    }

    UpdateServiceDependencies
        dependencies;
    if (options.channel.configured) {
        const auto backend =
            std::make_shared<
                ProductionUpdateBackend>(
                options,
                std::move(
                    installerLaunch));
        dependencies =
            backend->dependencies(
                options,
                backend);
    }
    return std::make_unique<
        UpdateService>(
            std::move(options),
            std::move(dependencies),
            parent);
}

} // namespace companion
