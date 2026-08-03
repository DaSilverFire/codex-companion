#include "update/UpdateCompatibility.h"

#include <utility>

#include <QSet>
#include <QVariantMap>

namespace companion {
namespace {

CompanionError compatibilityError(
    QString code,
    QString message,
    const ArtifactFacts& artifact,
    QVariantMap context = {})
{
    context.insert(
        QStringLiteral("path"),
        artifact.path);
    return {
        std::move(code),
        std::move(message),
        false,
        std::move(context),
    };
}

QString expectedInstallerMarker(
    const UpdateManifest& manifest)
{
    return QStringLiteral(
        "cc-update/1|%1|%2|w|x64|%3")
        .arg(manifest.version)
        .arg(manifest.build)
        .arg(manifest.minimumSystemVersion);
}

QString expectedInstallerFilename(
    const UpdateManifest& manifest)
{
    return QStringLiteral(
        "Codex-Companion-%1-%2-windows-x64.exe")
        .arg(manifest.version)
        .arg(manifest.build);
}

} // namespace

UpdateCompatibility::UpdateCompatibility(
    WindowsVersion currentWindowsVersion,
    QStringList allowedSignerSha256)
    : currentWindowsVersion_(
          currentWindowsVersion),
      allowedSignerSha256_(
          std::move(allowedSignerSha256))
{
}

Result<void>
UpdateCompatibility::validate(
    const UpdateManifest& manifest,
    const ArtifactFacts& artifact) const
{
    const auto minimumWindowsVersion =
        WindowsVersion::parse(
            manifest.minimumSystemVersion);
    if (!minimumWindowsVersion) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.invalid_minimum_windows_version"),
                QStringLiteral(
                    "The update minimum Windows version is invalid."),
                artifact));
    }
    if (currentWindowsVersion_
        < *minimumWindowsVersion) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.unsupported_windows_version"),
                QStringLiteral(
                    "This update requires a newer version of Windows."),
                artifact,
                {
                    {
                        QStringLiteral(
                            "currentWindowsVersion"),
                        currentWindowsVersion_
                            .toString(),
                    },
                    {
                        QStringLiteral(
                            "minimumWindowsVersion"),
                        minimumWindowsVersion
                            ->toString(),
                    },
                }));
    }

    if (!artifact.exists) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_missing"),
                QStringLiteral(
                    "The update installer does not exist."),
                artifact));
    }
    if (!artifact.regularFile) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_not_regular_file"),
                QStringLiteral(
                    "The update installer is not a regular file."),
                artifact));
    }
    if (artifact.reparsePoint) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_reparse_point"),
                QStringLiteral(
                    "The update installer cannot be a filesystem link."),
                artifact));
    }

    if (artifact.size != manifest.size) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_size_mismatch"),
                QStringLiteral(
                    "The update installer size does not match the signed manifest."),
                artifact,
                {
                    {
                        QStringLiteral(
                            "expectedSize"),
                        manifest.size,
                    },
                    {
                        QStringLiteral(
                            "actualSize"),
                        artifact.size,
                    },
                }));
    }

    const QByteArray expectedDigest =
        QByteArray::fromHex(
            manifest.sha256.toLatin1());
    if (expectedDigest.size() != 32
        || artifact.sha256.size() != 32
        || artifact.sha256
            != expectedDigest) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_digest_mismatch"),
                QStringLiteral(
                    "The update installer digest does not match the signed manifest."),
                artifact));
    }

    if (artifact.machine != PeMachine::X86
        && artifact.machine
            != PeMachine::X64) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.artifact_architecture_mismatch"),
                QStringLiteral(
                    "The update installer is not a supported Windows setup executable."),
                artifact));
    }

    if (artifact.metadata.productName
            != QStringLiteral(
                "Codex Companion")
        || artifact.metadata
               .productVersionMarker
            != expectedInstallerMarker(
                manifest)
        || artifact.metadata
               .originalFilename
            != expectedInstallerFilename(
                manifest)) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.installer_identity_mismatch"),
                QStringLiteral(
                    "The update installer identity does not match the signed manifest."),
                artifact));
    }

    if (allowedSignerSha256_.isEmpty()) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.signer_policy_unconfigured"),
                QStringLiteral(
                    "No trusted Windows update signer is configured."),
                artifact));
    }

    QSet<QString> allowed;
    for (const QString& thumbprint :
         allowedSignerSha256_) {
        const QString normalized =
            AuthenticodeVerifier::
                normalizeThumbprint(
                    thumbprint);
        if (normalized.isEmpty()) {
            return Result<void>::failure(
                compatibilityError(
                    QStringLiteral(
                        "update.invalid_signer_policy"),
                    QStringLiteral(
                        "The Windows update signer policy is invalid."),
                    artifact));
        }
        allowed.insert(normalized);
    }

    const QString signer =
        AuthenticodeVerifier::
            normalizeThumbprint(
                artifact.signer
                    .sha256Thumbprint);
    if (signer.isEmpty()
        || !allowed.contains(signer)) {
        return Result<void>::failure(
            compatibilityError(
                QStringLiteral(
                    "update.untrusted_signer"),
                QStringLiteral(
                    "The update installer was signed by an untrusted publisher."),
                artifact));
    }

    return Result<void>::success();
}

} // namespace companion
