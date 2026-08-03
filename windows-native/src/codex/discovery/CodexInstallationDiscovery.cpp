#include "codex/discovery/CodexInstallationDiscovery.h"

#include "codex/ipc/FollowerEndpointDiscovery.h"

#include <QDir>
#include <QSet>

#include <algorithm>

namespace companion {

namespace {

struct HashCandidate final {
    QString path;
    quint64 lastWriteTimeTicks = 0;
};

bool pathLessThan(const QString& left, const QString& right)
{
    const int insensitive =
        QString::compare(left, right, Qt::CaseInsensitive);
    if (insensitive != 0) {
        return insensitive < 0;
    }
    return left < right;
}

QString comparisonKey(
    const QString& path,
    const ICodexDiscoverySource& source)
{
    QString key =
        source.canonicalComparisonKey(path);
    if (key.isEmpty()) {
        key = path.toCaseFolded();
    }
    return key;
}

void appendUnique(
    QVector<QString>& candidates,
    QSet<QString>& comparisonKeys,
    const QString& candidate,
    const ICodexDiscoverySource& source)
{
    if (candidate.isEmpty()) {
        return;
    }

    const QString display = source.absoluteDisplayPath(candidate);
    if (display.isEmpty()) {
        return;
    }

    const QString key =
        comparisonKey(display, source);
    if (comparisonKeys.contains(key)) {
        return;
    }

    comparisonKeys.insert(key);
    candidates.append(display);
}

} // namespace

QVector<QString> CodexInstallationDiscovery::candidates(
    const CodexEnvironment& environment,
    const ICodexDiscoverySource& source)
{
    QVector<QString> result;
    QSet<QString> comparisonKeys;

    appendUnique(
        result,
        comparisonKeys,
        environment.configuredExecutable,
        source);

    QVector<QString> processImages =
        source.runningProcessImages(u"codex.exe");
    for (QString& image : processImages) {
        image = source.absoluteDisplayPath(image);
    }
    std::sort(processImages.begin(), processImages.end(), pathLessThan);
    for (const QString& image : processImages) {
        appendUnique(result, comparisonKeys, image, source);
    }

    if (!environment.codexBinRoot.isEmpty()) {
        appendUnique(
            result,
            comparisonKeys,
            QDir(environment.codexBinRoot).filePath(
                QStringLiteral("codex.exe")),
            source);

        QVector<HashCandidate> hashCandidates;
        const QVector<QString> childDirectories =
            source.childDirectories(environment.codexBinRoot);
        hashCandidates.reserve(childDirectories.size());
        for (const QString& directory : childDirectories) {
            const QString executable = source.absoluteDisplayPath(
                QDir(directory).filePath(QStringLiteral("codex.exe")));
            if (executable.isEmpty()) {
                continue;
            }
            hashCandidates.append({
                executable,
                source.fileAttributes(executable).lastWriteTimeTicks,
            });
        }

        std::sort(
            hashCandidates.begin(),
            hashCandidates.end(),
            [](const HashCandidate& left, const HashCandidate& right) {
                if (left.lastWriteTimeTicks != right.lastWriteTimeTicks) {
                    return left.lastWriteTimeTicks >
                        right.lastWriteTimeTicks;
                }
                return pathLessThan(left.path, right.path);
            });
        for (const HashCandidate& candidate : hashCandidates) {
            appendUnique(
                result, comparisonKeys, candidate.path, source);
        }
    }

    appendUnique(
        result,
        comparisonKeys,
        source.searchPath(u"codex.exe"),
        source);
    return result;
}

QVector<QString>
CodexInstallationDiscovery::trustedAppServerCandidates(
    const CodexEnvironment& environment,
    const ICodexDiscoverySource& source)
{
    QVector<QString> discovered =
        candidates(environment, source);
    QSet<QString> comparisonKeys;
    for (const QString& candidate : discovered) {
        comparisonKeys.insert(
            comparisonKey(candidate, source));
    }
    const QVector<QString> packageExecutables =
        source.installedCodexPackageExecutables();
    for (const QString& package :
         packageExecutables) {
        appendUnique(
            discovered,
            comparisonKeys,
            package,
            source);
    }

    const QString directRuntimeKey =
        comparisonKey(
            source.absoluteDisplayPath(
                QDir(environment.codexBinRoot)
                    .filePath(
                        QStringLiteral("codex.exe"))),
            source);
    QVector<QString> configured;
    QVector<HashCandidate> hashedRuntime;
    QVector<QString> directRuntime;
    QVector<HashCandidate> windowsPackage;
    const QVector<QString> protectedProgramFiles =
        source.protectedProgramFilesDirectories();
    for (const QString& candidate : discovered) {
        const auto trust =
            FollowerEndpointDiscovery::executableTrust(
                candidate,
                environment,
                protectedProgramFiles,
                packageExecutables);
        switch (trust) {
        case FollowerExecutableTrust::ConfiguredOverride:
            configured.append(candidate);
            break;
        case FollowerExecutableTrust::LocalRuntime:
            if (comparisonKey(candidate, source)
                == directRuntimeKey) {
                directRuntime.append(candidate);
            } else {
                hashedRuntime.append({
                    candidate,
                    source
                        .fileAttributes(candidate)
                        .lastWriteTimeTicks,
                });
            }
            break;
        case FollowerExecutableTrust::WindowsPackage:
            windowsPackage.append({
                candidate,
                source
                    .fileAttributes(candidate)
                    .lastWriteTimeTicks,
            });
            break;
        case FollowerExecutableTrust::Untrusted:
            break;
        }
    }

    const auto byFreshness =
        [](const HashCandidate& left,
           const HashCandidate& right) {
            if (left.lastWriteTimeTicks
                != right.lastWriteTimeTicks) {
                return left.lastWriteTimeTicks
                    > right.lastWriteTimeTicks;
            }
            return pathLessThan(
                left.path, right.path);
        };
    std::sort(
        hashedRuntime.begin(),
        hashedRuntime.end(),
        byFreshness);
    std::sort(
        windowsPackage.begin(),
        windowsPackage.end(),
        byFreshness);

    QVector<QString> trusted;
    trusted.reserve(
        configured.size()
        + hashedRuntime.size()
        + directRuntime.size()
        + windowsPackage.size());
    trusted += configured;
    for (const HashCandidate& candidate :
         hashedRuntime) {
        trusted.append(candidate.path);
    }
    trusted += directRuntime;
    for (const HashCandidate& candidate :
         windowsPackage) {
        trusted.append(candidate.path);
    }
    return trusted;
}

Result<QString> CodexInstallationDiscovery::firstRunnable(
    const CodexEnvironment& environment,
    const ICodexDiscoverySource& source)
{
    QVector<QString> discovered =
        trustedAppServerCandidates(environment, source);
    QSet<QString> comparisonKeys;
    for (const QString& candidate : discovered) {
        comparisonKeys.insert(
            comparisonKey(candidate, source));
    }
    for (const QString& candidate : candidates(environment, source)) {
        appendUnique(
            discovered,
            comparisonKeys,
            candidate,
            source);
    }
    for (const QString& candidate : discovered) {
        if (!candidate.endsWith(
                QStringLiteral(".exe"), Qt::CaseInsensitive)) {
            continue;
        }

        const CodexFileAttributes attributes =
            source.fileAttributes(candidate);
        if (attributes.exists && attributes.regularFile) {
            return Result<QString>::success(candidate);
        }
    }

    return Result<QString>::failure({
        QStringLiteral("codex.executable_not_found"),
        QStringLiteral("Could not find an installed Codex executable."),
        false,
        {{QStringLiteral("candidateCount"), discovered.size()}},
    });
}

} // namespace companion
