#pragma once

#include "codex/discovery/CodexDiscoverySource.h"
#include "core/Result.h"

#include <QString>

namespace companion {

struct CodexEnvironment final {
    QString homeDirectory;
    QString localAppData;
    QString codexHome;
    QString stateDatabase;
    QString goalDatabase;
    QString sessionIndex;
    QString rolloutRoot;
    QString configToml;
    QString petRoot;
    QString codexBinRoot;
    QString configuredExecutable;

    static Result<CodexEnvironment> discover(
        const QString& homeDirectory = {},
        const QString& localAppData = {},
        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource());
};

} // namespace companion
