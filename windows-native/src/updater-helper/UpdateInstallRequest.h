#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>

namespace companion {

struct UpdateInstallRequest final {
    QString requestId;
    QString installerPath;
    QString expectedSha256;
    qint64 expectedSize = 0;
    QString expectedVersion;
    qint64 expectedBuild = 0;
    QString installRoot;
    QString rollbackRoot;
    QString uninstallRegistryKey;
    QString startMenuShortcut;
    QString acknowledgementEvent;
    quint32 parentProcessId = 0;

    static Result<UpdateInstallRequest>
    decode(QByteArrayView bytes);
    static Result<UpdateInstallRequest>
    load(QStringView path);

    Result<void> validate() const;
    Result<void> writeAtomically(
        QStringView path) const;
    QByteArray encode() const;

    static QString
    expectedInstallRoot();
    static QString
    expectedUninstallRegistryKey();
    static QString
    expectedStartMenuShortcut();
    static QString acknowledgementEventFor(
        QStringView requestId);
    static QString helperReadyEventFor(
        QStringView requestId);

    friend bool operator==(
        const UpdateInstallRequest&,
        const UpdateInstallRequest&) = default;
};

} // namespace companion
