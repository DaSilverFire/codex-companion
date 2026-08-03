#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

namespace companion {

struct UpdateManifest final {
    static constexpr qint64 maximumSignedSize =
        512LL * 1024LL * 1024LL;

    int schemaVersion = 0;
    QString version;
    qint64 build = 0;
    QString minimumSystemVersion;
    QString publishedAt;
    QString downloadUrl;
    QString sha256;
    qint64 size = 0;
    QString signature;

    static Result<UpdateManifest> decode(QByteArrayView bytes);

    QByteArray canonicalPayload() const;
    bool isNewerThan(
        QStringView currentVersion,
        qint64 currentBuild) const;

    friend bool operator==(
        const UpdateManifest&,
        const UpdateManifest&) = default;
};

} // namespace companion
