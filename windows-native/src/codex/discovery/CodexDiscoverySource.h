#pragma once

#include <QString>
#include <QStringView>
#include <QVector>
#include <QtGlobal>

namespace companion {

struct CodexFileAttributes final {
    bool exists = false;
    bool regularFile = false;
    quint64 lastWriteTimeTicks = 0;
};

class ICodexDiscoverySource {
public:
    virtual ~ICodexDiscoverySource() = default;

    virtual QString environmentValue(QStringView name) const = 0;
    virtual QString profileDirectory() const = 0;
    virtual QString localAppDataDirectory() const = 0;
    virtual QVector<QString>
    protectedProgramFilesDirectories() const = 0;
    virtual QVector<QString>
    installedCodexPackageExecutables() const = 0;
    virtual QVector<QString> runningProcessImages(
        QStringView executableName) const = 0;
    virtual QVector<QString> childDirectories(
        const QString& directory) const = 0;
    virtual QString searchPath(QStringView executableName) const = 0;
    virtual CodexFileAttributes fileAttributes(const QString& path) const = 0;
    virtual QString absoluteDisplayPath(const QString& path) const = 0;
    virtual QString canonicalComparisonKey(const QString& path) const = 0;
};

QVector<QString>
systemProtectedProgramFilesDirectories();

const ICodexDiscoverySource& systemCodexDiscoverySource();

} // namespace companion
