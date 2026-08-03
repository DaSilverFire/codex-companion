#pragma once

#include "core/Result.h"
#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/UpdateManifest.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QUuid>

#include <functional>
#include <memory>

namespace companion {

struct VerifiedArtifact final {
    QString path;
    qint64 size = 0;
    QByteArray sha256;
    InstallerMetadata metadata;
    AuthenticodeIdentity signer;

    friend bool operator==(
        const VerifiedArtifact&,
        const VerifiedArtifact&) = default;
};

using UpdateArtifactClock =
    std::function<QDateTime()>;
using UpdateArtifactIdFactory =
    std::function<QUuid()>;
using UpdatePeInspector =
    std::function<Result<PeMachine>(
        QStringView)>;
using UpdateMetadataInspector =
    std::function<Result<InstallerMetadata>(
        QStringView)>;
using UpdateSignerInspector =
    std::function<Result<AuthenticodeIdentity>(
        QStringView)>;
using UpdateArtifactPathHook =
    std::function<void(QStringView)>;

struct UpdateArtifactStoreOptions final {
    QString rootPath;
    WindowsVersion currentWindowsVersion;
    QStringList allowedSignerSha256;
    UpdateArtifactClock clock;
    UpdateArtifactIdFactory idFactory;
    UpdatePeInspector peInspector;
    UpdateMetadataInspector
        metadataInspector;
    UpdateSignerInspector signerInspector;
    UpdateArtifactPathHook
        afterWriterClosed;
    UpdateArtifactPathHook
        beforePublishReopen;
};

struct UpdateArtifactStoreState;

class UpdateArtifactStagingSession final {
public:
    ~UpdateArtifactStagingSession();

    UpdateArtifactStagingSession(
        const UpdateArtifactStagingSession&) =
        delete;
    UpdateArtifactStagingSession& operator=(
        const UpdateArtifactStagingSession&) =
        delete;
    UpdateArtifactStagingSession(
        UpdateArtifactStagingSession&&)
        noexcept;
    UpdateArtifactStagingSession& operator=(
        UpdateArtifactStagingSession&&)
        noexcept;

    Result<void> append(
        QByteArrayView bytes);
    Result<VerifiedArtifact> finish();
    void cancel() noexcept;

    QString partialPath() const;
    qint64 receivedBytes() const noexcept;

private:
    struct Implementation;

    explicit UpdateArtifactStagingSession(
        std::unique_ptr<Implementation>
            implementation);

    friend class UpdateArtifactStore;

    std::unique_ptr<Implementation>
        implementation_;
};

class UpdateArtifactStore final {
public:
    static QString defaultRootPath();

    explicit UpdateArtifactStore(
        UpdateArtifactStoreOptions options);
    ~UpdateArtifactStore();

    UpdateArtifactStore(
        const UpdateArtifactStore&) = delete;
    UpdateArtifactStore& operator=(
        const UpdateArtifactStore&) = delete;
    UpdateArtifactStore(
        UpdateArtifactStore&&) noexcept =
        default;
    UpdateArtifactStore& operator=(
        UpdateArtifactStore&&) noexcept =
        default;

    Result<std::unique_ptr<
        UpdateArtifactStagingSession>>
    begin(const UpdateManifest& manifest);

    Result<void> prune(
        QStringView activeArtifactPath = {})
        const;

private:
    std::shared_ptr<
        UpdateArtifactStoreState>
        state_;
};

} // namespace companion
