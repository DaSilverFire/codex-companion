#pragma once

#include "codex/discovery/CodexEnvironment.h"

#include <QHash>
#include <QString>
#include <QStringView>
#include <QVector>

namespace companion {

struct FollowerProcessEvidence final {
    QString imagePath;
    QString commandLine;
};

enum class FollowerExecutableTrust {
    Untrusted,
    ConfiguredOverride,
    LocalRuntime,
    WindowsPackage,
};

class IFollowerEndpointEvidenceSource {
public:
    virtual ~IFollowerEndpointEvidenceSource() = default;

    virtual QString environmentValue(QStringView name) const = 0;
    virtual QVector<QString>
    protectedProgramFilesDirectories() const = 0;
    virtual QVector<QString>
    installedCodexPackageExecutables() const = 0;
    virtual QVector<FollowerProcessEvidence>
    runningCodexProcesses() const = 0;
    virtual QVector<QString> installedRuntimeMetadata(
        const CodexEnvironment& environment) const = 0;
};

const IFollowerEndpointEvidenceSource&
systemFollowerEndpointEvidenceSource();

class FollowerEndpointDiscovery final {
public:
    explicit FollowerEndpointDiscovery(
        const IFollowerEndpointEvidenceSource& source =
            systemFollowerEndpointEvidenceSource());

    QVector<QString> candidates(
        const CodexEnvironment& environment) const;

    static QString verifiedWindowsEndpoint();
    static bool isOfficialWindowsPackageProcess(
        const QString& imagePath,
        const QString& packageFamilyName);
    static FollowerExecutableTrust executableTrust(
        const QString& imagePath,
        const CodexEnvironment& environment,
        const QVector<QString>&
            protectedProgramFilesDirectories,
        const QVector<QString>&
            installedCodexPackageExecutables);

private:
    const IFollowerEndpointEvidenceSource& source_;
};

} // namespace companion
