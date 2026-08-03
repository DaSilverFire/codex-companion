#pragma once

#include "core/Result.h"
#include "update/UpdateManifest.h"

#include <QByteArray>
#include <QString>
#include <QStringView>

namespace companion {

struct UpdateManifestSigningRequest final {
    QString version;
    qint64 build = 0;
    QString minimumSystemVersion;
    QString publishedAt;
    QString downloadUrl;
    QString sha256;
    qint64 size = 0;
    QString expectedPublicKeyBase64;
};

struct SignedUpdateManifest final {
    UpdateManifest manifest;
    QByteArray json;
    QString publicKeyBase64;
};

Result<SignedUpdateManifest> signUpdateManifest(
    UpdateManifestSigningRequest request,
    QByteArray privateSeedBase64);

Result<void> writeSignedUpdateManifest(
    QStringView outputPath,
    QByteArrayView manifestJson);

} // namespace companion
