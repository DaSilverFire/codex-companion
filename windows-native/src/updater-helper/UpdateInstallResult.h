#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <QVariantMap>

namespace companion {

struct UpdateInstallResultRecord final {
    QString requestId;
    bool success = false;
    QString errorCode;
    QString message;
    QString completedAtUtc;
    QString installerLogPath;
    QVariantMap context;

    QByteArray toJson() const;
    Result<void> write(
        QStringView filePath) const;

    friend bool operator==(
        const UpdateInstallResultRecord&,
        const UpdateInstallResultRecord&) =
        default;
};

} // namespace companion
