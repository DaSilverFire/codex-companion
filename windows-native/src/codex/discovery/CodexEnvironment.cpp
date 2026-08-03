#include "codex/discovery/CodexEnvironment.h"

#include <QDir>

#include <utility>

namespace companion {

namespace {

QString derivedPath(
    const ICodexDiscoverySource& source,
    const QString& parent,
    const QString& child)
{
    return source.absoluteDisplayPath(QDir(parent).filePath(child));
}

CompanionError missingDirectoryError(const QString& name)
{
    return {
        QStringLiteral("codex.environment_unavailable"),
        QStringLiteral("Could not resolve the Windows %1 directory.").arg(name),
        false,
        {{QStringLiteral("directory"), name}},
    };
}

} // namespace

Result<CodexEnvironment> CodexEnvironment::discover(
    const QString& homeDirectory,
    const QString& localAppData,
    const ICodexDiscoverySource& source)
{
    const QString resolvedHome = source.absoluteDisplayPath(
        homeDirectory.isEmpty()
            ? source.profileDirectory()
            : homeDirectory);
    if (resolvedHome.isEmpty()) {
        return Result<CodexEnvironment>::failure(
            missingDirectoryError(QStringLiteral("profile")));
    }

    const QString resolvedLocalAppData = source.absoluteDisplayPath(
        localAppData.isEmpty()
            ? source.localAppDataDirectory()
            : localAppData);
    if (resolvedLocalAppData.isEmpty()) {
        return Result<CodexEnvironment>::failure(
            missingDirectoryError(QStringLiteral("Local AppData")));
    }

    CodexEnvironment environment;
    environment.homeDirectory = resolvedHome;
    environment.localAppData = resolvedLocalAppData;
    environment.codexHome =
        derivedPath(source, resolvedHome, QStringLiteral(".codex"));
    environment.stateDatabase = derivedPath(
        source, environment.codexHome, QStringLiteral("state_5.sqlite"));
    environment.goalDatabase = derivedPath(
        source, environment.codexHome, QStringLiteral("goals_1.sqlite"));
    environment.sessionIndex = derivedPath(
        source, environment.codexHome, QStringLiteral("session_index.jsonl"));
    environment.rolloutRoot = derivedPath(
        source, environment.codexHome, QStringLiteral("sessions"));
    environment.configToml = derivedPath(
        source, environment.codexHome, QStringLiteral("config.toml"));
    environment.petRoot = derivedPath(
        source,
        resolvedLocalAppData,
        QStringLiteral("Codex Companion/Pets"));
    environment.codexBinRoot = derivedPath(
        source,
        resolvedLocalAppData,
        QStringLiteral("OpenAI/Codex/bin"));

    const QString configured = source.environmentValue(
        u"CODEX_COMPANION_CODEX_EXE");
    if (!configured.isEmpty()) {
        environment.configuredExecutable =
            source.absoluteDisplayPath(configured);
    }

    return Result<CodexEnvironment>::success(std::move(environment));
}

} // namespace companion
