#pragma once

#include "core/Result.h"
#include "update/UpdateArtifactStore.h"
#include "update/UpdateManifest.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

namespace companion {

enum class UpdatePhase {
    Idle,
    Checking,
    Unavailable,
    UpToDate,
    Available,
    Downloading,
    ReadyToInstall,
    Installing,
    Failed,
};

QString updatePhaseName(UpdatePhase phase);

struct UpdateChannelConfiguration final {
    bool configured = false;
    QUrl manifestUrl;
    QString publicKeyBase64;
    QStringList allowedSignerSha256;
    QString platform;
    QString architecture;
    QString minimumSystemVersion;

    static Result<UpdateChannelConfiguration>
    decode(QByteArrayView bytes);
    static Result<UpdateChannelConfiguration>
    fromBundledResource();
    Result<UpdateChannelConfiguration>
    withManifestUrlOverride(
        QUrl manifestUrlOverride) const;
};

Result<std::optional<QUrl>>
updateManifestUrlOverrideFromArguments(
    const QStringList& arguments);

struct UpdateServiceOptions final {
    QString installedVersion;
    qint64 installedBuild = 0;
    UpdateChannelConfiguration channel;
    QString artifactRoot;
    QByteArray userAgent;
    QString unavailableDetail =
        QStringLiteral(
            "Release feed and signing key are not configured.");
};

using UpdateDownloadProgress =
    std::function<void(qint64 received, qint64 total)>;
using UpdateManifestRequest =
    std::function<
        QFuture<Result<UpdateManifest>>()>;
using UpdateArtifactRequest =
    std::function<
        QFuture<Result<VerifiedArtifact>>(
            const UpdateManifest&,
            UpdateDownloadProgress)>;
using UpdateInstallerLaunch =
    std::function<
        Result<void>(
            const UpdateManifest&,
            const VerifiedArtifact&)>;
using UpdatePruneCommand =
    std::function<Result<void>(QStringView)>;
using UpdateCancelCommand =
    std::function<void()>;

struct UpdateServiceDependencies final {
    UpdateManifestRequest requestManifest;
    UpdateArtifactRequest requestArtifact;
    UpdateInstallerLaunch launchInstaller;
    UpdatePruneCommand prune;
    UpdateCancelCommand cancel;
    std::shared_ptr<void> lifetime;
};

struct UpdateServiceSnapshot final {
    UpdatePhase phase = UpdatePhase::Idle;
    QString title;
    QString detail;
    QString installedVersion;
    qint64 installedBuild = 0;
    QString availableVersion;
    qint64 availableBuild = 0;
    double downloadProgress = 0;
    QString errorCode;

    friend bool operator==(
        const UpdateServiceSnapshot&,
        const UpdateServiceSnapshot&) = default;
};

class UpdateService final : public QObject {
    Q_OBJECT

public:
    UpdateService(
        UpdateServiceOptions options,
        UpdateServiceDependencies dependencies,
        QObject* parent = nullptr);
    ~UpdateService() override;

    UpdateService(
        const UpdateService&) = delete;
    UpdateService& operator=(
        const UpdateService&) = delete;

    const UpdateServiceSnapshot&
    snapshot() const noexcept;

    Result<void> checkForUpdates();
    Result<void> downloadAvailableUpdate();
    Result<void> installReadyUpdate();

signals:
    void stateChanged();
    void runtimeErrorOccurred(
        companion::CompanionError error);
    void installLaunched(QString path);

private:
    Result<void> invalidState(
        QStringView operation) const;
    void publish(
        UpdateServiceSnapshot snapshot);
    void publishFailure(
        const CompanionError& error);
    void finishManifestRequest(
        quint64 generation,
        Result<UpdateManifest> result);
    void finishArtifactRequest(
        quint64 generation,
        Result<VerifiedArtifact> result);
    void publishDownloadProgress(
        quint64 generation,
        qint64 received,
        qint64 total);
    void cancelActiveOperation() noexcept;

    UpdateServiceOptions options_;
    UpdateServiceDependencies dependencies_;
    UpdateServiceSnapshot snapshot_;
    std::optional<UpdateManifest>
        availableManifest_;
    std::optional<VerifiedArtifact>
        readyArtifact_;
    quint64 operationGeneration_ = 0;
};

std::unique_ptr<UpdateService>
createProductionUpdateService(
    UpdateInstallerLaunch
        installerLaunch,
    QObject* parent = nullptr,
    std::optional<QUrl>
        manifestUrlOverride =
            std::nullopt);

} // namespace companion

Q_DECLARE_METATYPE(companion::UpdatePhase)
