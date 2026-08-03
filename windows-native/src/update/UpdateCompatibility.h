#pragma once

#include "core/Result.h"
#include "platform/windows/AuthenticodeVerifier.h"
#include "platform/windows/InstallerMetadataReader.h"
#include "platform/windows/PeImageInspector.h"
#include "platform/windows/WindowsVersionProvider.h"
#include "update/UpdateManifest.h"

#include <QString>
#include <QStringList>

namespace companion {

struct ArtifactFacts final {
    QString path;
    bool exists = false;
    bool regularFile = false;
    bool reparsePoint = false;
    qint64 size = 0;
    QByteArray sha256;
    PeMachine machine = PeMachine::Unknown;
    InstallerMetadata metadata;
    AuthenticodeIdentity signer;
};

class UpdateCompatibility final {
public:
    UpdateCompatibility(
        WindowsVersion currentWindowsVersion,
        QStringList allowedSignerSha256);

    Result<void> validate(
        const UpdateManifest& manifest,
        const ArtifactFacts& artifact) const;

private:
    WindowsVersion currentWindowsVersion_;
    QStringList allowedSignerSha256_;
};

} // namespace companion
