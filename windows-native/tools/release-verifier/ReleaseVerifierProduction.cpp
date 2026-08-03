#include "ReleaseVerifierProduction.h"

#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/MonocypherEd25519Verifier.h"
#include "update/UpdateManifestValidator.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QVariantMap>

#define NOMINMAX
#include <windows.h>

#ifndef COMPANION_RELEASE_SOURCE_COMMIT
#define COMPANION_RELEASE_SOURCE_COMMIT ""
#endif
#ifndef COMPANION_RELEASE_SOURCE_TREE
#define COMPANION_RELEASE_SOURCE_TREE ""
#endif
#ifndef COMPANION_RELEASE_COMPILER_VERSION
#define COMPANION_RELEASE_COMPILER_VERSION ""
#endif
#ifndef COMPANION_RELEASE_QT_VERSION
#define COMPANION_RELEASE_QT_VERSION ""
#endif
#ifndef COMPANION_RELEASE_INNO_VERSION
#define COMPANION_RELEASE_INNO_VERSION ""
#endif
#ifndef COMPANION_RELEASE_WINDOWS_SDK_VERSION
#define COMPANION_RELEASE_WINDOWS_SDK_VERSION ""
#endif
#ifndef COMPANION_RELEASE_WEBSOCKETS_COMMIT
#define COMPANION_RELEASE_WEBSOCKETS_COMMIT ""
#endif
#ifndef COMPANION_RELEASE_MONOCYPHER_COMMIT
#define COMPANION_RELEASE_MONOCYPHER_COMMIT ""
#endif

namespace companion {
namespace {

CompanionError productionError(
    QString code,
    QString message,
    QStringView path)
{
    return {
        std::move(code),
        std::move(message),
        false,
        {
            {
                QStringLiteral("path"),
                path.toString(),
            },
        },
    };
}

BOOL CALLBACK countIconGroups(
    HMODULE,
    LPCWSTR,
    LPWSTR,
    LONG_PTR parameter)
{
    auto* count =
        reinterpret_cast<int*>(
            parameter);
    ++(*count);
    return TRUE;
}

Result<bool> inspectExecutableIcon(
    QStringView path,
    ExecutableIconExpectation expectation)
{
    const std::wstring native =
        QDir::toNativeSeparators(
            QFileInfo(path.toString())
                .absoluteFilePath())
            .toStdWString();
    const HMODULE module =
        LoadLibraryExW(
            native.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr) {
        return Result<bool>::failure(
            productionError(
                QStringLiteral(
                    "release.icon_resource_unavailable"),
                QStringLiteral(
                    "An executable icon resource could not be inspected."),
                path));
    }

    bool present = false;
    if (expectation
        == ExecutableIconExpectation::
            CompanionApplication) {
        present =
            FindResourceW(
                module,
                MAKEINTRESOURCEW(101),
                RT_GROUP_ICON)
            != nullptr;
    } else {
        int count = 0;
        SetLastError(ERROR_SUCCESS);
        const BOOL enumerated =
            EnumResourceNamesW(
                module,
                RT_GROUP_ICON,
                countIconGroups,
                reinterpret_cast<LONG_PTR>(
                    &count));
        const DWORD error =
            GetLastError();
        present =
            enumerated != FALSE
            && count > 0;
        if (!present
            && error
                != ERROR_SUCCESS
            && error
                != ERROR_RESOURCE_TYPE_NOT_FOUND) {
            FreeLibrary(module);
            return Result<bool>::failure(
                productionError(
                    QStringLiteral(
                        "release.icon_resource_unavailable"),
                    QStringLiteral(
                        "An executable icon resource could not be enumerated."),
                    path));
        }
    }

    FreeLibrary(module);
    return Result<bool>::success(
        present);
}

} // namespace

ReleaseVerifierDependencies
makeProductionReleaseVerifierDependencies()
{
    ReleaseVerifierDependencies
        dependencies;
    dependencies.manifestValidator =
        [](
            QByteArrayView manifest,
            QStringView publicKey) {
            const MonocypherEd25519Verifier
                signatureVerifier;
            const UpdateManifestValidator
                validator(
                    signatureVerifier);
            return validator.validate(
                manifest,
                publicKey);
        };
    dependencies.windowsVersionProvider =
        [] {
            return WindowsVersionProvider()
                .current();
        };
    dependencies.peInspector =
        [](QStringView path) {
            return PeImageInspector()
                .machine(path);
        };
    dependencies.metadataInspector =
        [](QStringView path) {
            return InstallerMetadataReader()
                .read(path);
        };

    const AuthenticodePolicy policy =
        AuthenticodePolicy::
            fromBuildConfiguration();
    dependencies.allowedSignerSha256 =
        policy.allowedSignerSha256;
    dependencies.signerInspector =
        [policy](QStringView path) {
            return AuthenticodeVerifier()
                .verify(path, policy);
        };
    dependencies.iconInspector =
        inspectExecutableIcon;
    return dependencies;
}

ReleaseEvidenceMetadata
productionReleaseEvidenceMetadata()
{
    ReleaseEvidenceMetadata metadata;
    metadata.sourceCommit =
        QString::fromLatin1(
            COMPANION_RELEASE_SOURCE_COMMIT);
    metadata.sourceTree =
        QString::fromLatin1(
            COMPANION_RELEASE_SOURCE_TREE);
    metadata.compilerVersion =
        QString::fromLatin1(
            COMPANION_RELEASE_COMPILER_VERSION);
    metadata.qtVersion =
        QString::fromLatin1(
            COMPANION_RELEASE_QT_VERSION);
    metadata.innoVersion =
        QString::fromLatin1(
            COMPANION_RELEASE_INNO_VERSION);
    metadata.windowsSdkVersion =
        QString::fromLatin1(
            COMPANION_RELEASE_WINDOWS_SDK_VERSION);
    metadata.webSocketsSourceCommit =
        QString::fromLatin1(
            COMPANION_RELEASE_WEBSOCKETS_COMMIT);
    metadata.monocypherCommit =
        QString::fromLatin1(
            COMPANION_RELEASE_MONOCYPHER_COMMIT);
    return metadata;
}

QString productionDefaultReleaseStagePath(
    QStringView installerPath)
{
    return QDir(
               QFileInfo(
                   installerPath.toString())
                   .absolutePath())
        .filePath(
            QStringLiteral(
                "portable/Codex Companion"));
}

QString productionDefaultReleaseMetadataPath(
    QStringView installerPath)
{
    return QDir(
               QFileInfo(
                   installerPath.toString())
                   .absolutePath())
        .filePath(
            QStringLiteral(
                "release-metadata.json"));
}

} // namespace companion
