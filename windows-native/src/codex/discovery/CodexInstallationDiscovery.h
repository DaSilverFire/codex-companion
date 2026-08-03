#pragma once

#include "codex/discovery/CodexDiscoverySource.h"
#include "codex/discovery/CodexEnvironment.h"
#include "core/Result.h"

#include <QString>
#include <QVector>

namespace companion {

class CodexInstallationDiscovery final {
public:
    static QVector<QString> candidates(
        const CodexEnvironment& environment,
        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource());

    static QVector<QString> trustedAppServerCandidates(
        const CodexEnvironment& environment,
        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource());

    static Result<QString> firstRunnable(
        const CodexEnvironment& environment,
        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource());
};

} // namespace companion
