#pragma once

#include "core/Result.h"
#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/UpdateManifest.h"

#include <functional>

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

namespace companion {

enum class ReleaseCheckStatus {
    Passed,
    Failed,
    Skipped,
};

struct ReleaseVerificationCheck final {
    QString id;
    ReleaseCheckStatus status =
        ReleaseCheckStatus::Skipped;
    QString code;

    friend bool operator==(
        const ReleaseVerificationCheck&,
        const ReleaseVerificationCheck&) =
        default;
};

struct ReleaseEvidenceMetadata final {
    QString sourceCommit;
    QString sourceTree;
    QString cmakeCacheSha256;
    QString compilerVersion;
    QString qtVersion;
    QString innoVersion;
    QString windowsSdkVersion;
    QString webSocketsSourceCommit;
    QString monocypherCommit;
    QString recordingSha256;
};

struct ReleaseVerifierOptions final {
    QString installerPath;
    QString manifestPath;
    QString publicKeyBase64;
    QString stagePath;
    QStringList approvedSupportScripts;
    ReleaseEvidenceMetadata metadata;
    QString metadataErrorCode;
};

enum class ExecutableIconExpectation {
    CompanionApplication,
    Installer,
};

using ReleaseManifestValidator =
    std::function<Result<UpdateManifest>(
        QByteArrayView,
        QStringView)>;
using ReleaseWindowsVersionProvider =
    std::function<Result<WindowsVersion>()>;
using ReleasePeInspector =
    std::function<Result<PeMachine>(
        QStringView)>;
using ReleaseMetadataInspector =
    std::function<Result<InstallerMetadata>(
        QStringView)>;
using ReleaseSignerInspector =
    std::function<Result<AuthenticodeIdentity>(
        QStringView)>;
using ReleaseIconInspector =
    std::function<Result<bool>(
        QStringView,
        ExecutableIconExpectation)>;

struct ReleaseVerifierDependencies final {
    ReleaseManifestValidator
        manifestValidator;
    ReleaseWindowsVersionProvider
        windowsVersionProvider;
    ReleasePeInspector peInspector;
    ReleaseMetadataInspector
        metadataInspector;
    ReleaseSignerInspector signerInspector;
    ReleaseIconInspector iconInspector;
    QStringList allowedSignerSha256;
};

struct ReleaseVerificationEvidence final {
    bool passed = false;
    QString sourceCommit;
    QString sourceTree;
    QString version;
    qint64 build = 0;
    QString minimumSystemVersion;
    QString installerSha256;
    qint64 installerSize = 0;
    QString manifestSha256;
    QString signerSha256;
    QString qtVersion;
    QString innoVersion;
    QString windowsSdkVersion;
    QString cmakeCacheSha256;
    QString compilerVersion;
    QString webSocketsSourceCommit;
    QString monocypherCommit;
    QString recordingSha256;
    QVector<ReleaseVerificationCheck>
        checks;
};

class ReleaseVerifier final {
public:
    explicit ReleaseVerifier(
        ReleaseVerifierDependencies
            dependencies);

    ReleaseVerificationEvidence verify(
        const ReleaseVerifierOptions&
            options) const;

    static QStringList
    requiredStageFiles();
    static QStringList
    requiredStageDirectories();

private:
    ReleaseVerifierDependencies
        dependencies_;
};

QJsonObject releaseVerificationEvidenceJson(
    const ReleaseVerificationEvidence&
        evidence);

Result<void> writeReleaseVerificationEvidence(
    QStringView path,
    const ReleaseVerificationEvidence&
        evidence);

Result<ReleaseEvidenceMetadata>
loadReleaseEvidenceMetadata(
    QStringView path);

ReleaseEvidenceMetadata
mergeReleaseEvidenceMetadata(
    ReleaseEvidenceMetadata defaults,
    const ReleaseEvidenceMetadata&
        overrides);

int releaseVerifierExitCode(
    const ReleaseVerificationEvidence&
        evidence) noexcept;

} // namespace companion
