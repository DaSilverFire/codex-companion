#include "codex/discovery/CodexDiscoverySource.h"
#include "codex/discovery/CodexEnvironment.h"
#include "codex/discovery/CodexInstallationDiscovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>
#include <QtTest>

#include <utility>

using namespace companion;

namespace {

QString displayPath(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::toNativeSeparators(
        QDir::cleanPath(QDir::fromNativeSeparators(path)));
}

QString lexicalKey(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

QString childPath(const QString& parent, const QString& child)
{
    return displayPath(QDir(QDir::fromNativeSeparators(parent)).filePath(child));
}

class FakeCodexDiscoverySource final : public ICodexDiscoverySource {
public:
    QString profile = displayPath(QStringLiteral("C:/Users/Test"));
    QString localAppData =
        displayPath(QStringLiteral("C:/Users/Test/AppData/Local"));
    QHash<QString, QString> environment;
    QVector<QString> processImages;
    QHash<QString, QVector<QString>> children;
    QString pathExecutable;
    QHash<QString, CodexFileAttributes> attributes;
    QHash<QString, QString> canonicalKeys;
    QVector<QString> protectedProgramFiles{
        displayPath(
            QStringLiteral("C:/Program Files")),
    };
    QVector<QString> packageExecutables;

    QString environmentValue(QStringView name) const override
    {
        return environment.value(name.toString());
    }

    QString profileDirectory() const override
    {
        return profile;
    }

    QString localAppDataDirectory() const override
    {
        return localAppData;
    }

    QVector<QString>
    protectedProgramFilesDirectories() const override
    {
        return protectedProgramFiles;
    }

    QVector<QString>
    installedCodexPackageExecutables() const override
    {
        return packageExecutables;
    }

    QVector<QString> runningProcessImages(
        QStringView executableName) const override
    {
        if (executableName != u"codex.exe") {
            qFatal("unexpected process executable name");
        }
        return processImages;
    }

    QVector<QString> childDirectories(const QString& directory) const override
    {
        return children.value(lexicalKey(directory));
    }

    QString searchPath(QStringView executableName) const override
    {
        if (executableName != u"codex.exe") {
            qFatal("unexpected PATH executable name");
        }
        return pathExecutable;
    }

    CodexFileAttributes fileAttributes(const QString& path) const override
    {
        return attributes.value(lexicalKey(path));
    }

    QString absoluteDisplayPath(const QString& path) const override
    {
        return displayPath(path);
    }

    QString canonicalComparisonKey(const QString& path) const override
    {
        const QString key = lexicalKey(path);
        return canonicalKeys.value(key, key);
    }

    void setChildren(
        const QString& directory,
        QVector<QString> childDirectories)
    {
        children.insert(
            lexicalKey(directory), std::move(childDirectories));
    }

    void setAttributes(
        const QString& path,
        bool exists,
        bool regularFile,
        quint64 lastWriteTimeTicks = 0)
    {
        attributes.insert(
            lexicalKey(path),
            {exists, regularFile, lastWriteTimeTicks});
    }

    void setCanonicalKey(const QString& path, const QString& key)
    {
        canonicalKeys.insert(lexicalKey(path), key.toCaseFolded());
    }
};

CodexEnvironment discoveredEnvironment(FakeCodexDiscoverySource& source)
{
    const auto result = CodexEnvironment::discover({}, {}, source);
    if (!result.hasValue()) {
        qFatal("%s", qPrintable(result.error().message));
    }
    return result.value();
}

} // namespace

class CodexDiscoveryTests final : public QObject {
    Q_OBJECT

private slots:
    void resolvesDefaultEnvironmentWithoutDataFiles()
    {
        FakeCodexDiscoverySource source;
        source.profile = displayPath(QStringLiteral("C:/Users/Ada"));
        source.localAppData =
            displayPath(QStringLiteral("D:/Profiles/Ada/Local"));
        source.environment.insert(
            QStringLiteral("CODEX_COMPANION_CODEX_EXE"),
            QStringLiteral("E:/Configured/../Configured/codex.exe"));

        const auto result = CodexEnvironment::discover({}, {}, source);

        QVERIFY(result.hasValue());
        const CodexEnvironment& environment = result.value();
        QCOMPARE(environment.homeDirectory, source.profile);
        QCOMPARE(environment.localAppData, source.localAppData);
        QCOMPARE(
            environment.codexHome,
            childPath(source.profile, QStringLiteral(".codex")));
        QCOMPARE(
            environment.stateDatabase,
            childPath(environment.codexHome, QStringLiteral("state_5.sqlite")));
        QCOMPARE(
            environment.goalDatabase,
            childPath(environment.codexHome, QStringLiteral("goals_1.sqlite")));
        QCOMPARE(
            environment.sessionIndex,
            childPath(
                environment.codexHome,
                QStringLiteral("session_index.jsonl")));
        QCOMPARE(
            environment.rolloutRoot,
            childPath(environment.codexHome, QStringLiteral("sessions")));
        QCOMPARE(
            environment.configToml,
            childPath(environment.codexHome, QStringLiteral("config.toml")));
        QCOMPARE(
            environment.petRoot,
            childPath(
                source.localAppData,
                QStringLiteral("Codex Companion/Pets")));
        QCOMPARE(
            environment.codexBinRoot,
            childPath(
                source.localAppData,
                QStringLiteral("OpenAI/Codex/bin")));
        QCOMPARE(
            environment.configuredExecutable,
            displayPath(QStringLiteral("E:/Configured/codex.exe")));
    }

    void explicitDirectoriesOverrideKnownFolders()
    {
        FakeCodexDiscoverySource source;
        source.profile = {};
        source.localAppData = {};

        const QString home = displayPath(QStringLiteral("F:/Portable/Home"));
        const QString local =
            displayPath(QStringLiteral("G:/Portable/Local"));
        const auto result = CodexEnvironment::discover(home, local, source);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value().homeDirectory, home);
        QCOMPARE(result.value().localAppData, local);
        QCOMPARE(
            result.value().configToml,
            childPath(home, QStringLiteral(".codex/config.toml")));
        QCOMPARE(
            result.value().codexBinRoot,
            childPath(local, QStringLiteral("OpenAI/Codex/bin")));
    }

    void ordersCandidatesByPriorityAndHashFreshness()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment = discoveredEnvironment(source);
        const QString configured =
            displayPath(QStringLiteral("D:/Configured/codex.exe"));
        const QString runningA =
            displayPath(QStringLiteral("C:/Apps/A/codex.exe"));
        const QString runningZ =
            displayPath(QStringLiteral("C:/Apps/Z/codex.exe"));
        const QString direct =
            childPath(environment.codexBinRoot, QStringLiteral("codex.exe"));
        const QString hashOldDirectory =
            childPath(environment.codexBinRoot, QStringLiteral("hash-old"));
        const QString hashADirectory =
            childPath(environment.codexBinRoot, QStringLiteral("AAA"));
        const QString hashZDirectory =
            childPath(environment.codexBinRoot, QStringLiteral("bbb"));
        const QString hashOld =
            childPath(hashOldDirectory, QStringLiteral("codex.exe"));
        const QString hashA =
            childPath(hashADirectory, QStringLiteral("codex.exe"));
        const QString hashZ =
            childPath(hashZDirectory, QStringLiteral("codex.exe"));
        const QString fromPath =
            displayPath(QStringLiteral("C:/Path/codex.exe"));

        CodexEnvironment configuredEnvironment = environment;
        configuredEnvironment.configuredExecutable = configured;
        source.processImages = {runningZ, runningA};
        source.setChildren(
            environment.codexBinRoot,
            {hashOldDirectory, hashZDirectory, hashADirectory});
        source.setAttributes(hashOld, true, true, 10);
        source.setAttributes(hashA, true, true, 20);
        source.setAttributes(hashZ, true, true, 20);
        source.pathExecutable = fromPath;

        QCOMPARE(
            CodexInstallationDiscovery::candidates(
                configuredEnvironment, source),
            QVector<QString>({
                configured,
                runningA,
                runningZ,
                direct,
                hashA,
                hashZ,
                hashOld,
                fromPath,
            }));
    }

    void deduplicatesCanonicalAliasesPreservingFirst()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment = discoveredEnvironment(source);
        const QString processLink =
            displayPath(QStringLiteral("C:/Links/codex.exe"));
        const QString direct =
            childPath(environment.codexBinRoot, QStringLiteral("codex.exe"));
        const QString hashDirectory =
            childPath(environment.codexBinRoot, QStringLiteral("unique"));
        const QString hashExecutable =
            childPath(hashDirectory, QStringLiteral("codex.exe"));

        source.processImages = {processLink};
        source.setChildren(environment.codexBinRoot, {hashDirectory});
        source.setAttributes(hashExecutable, true, true, 1);
        source.pathExecutable = QStringLiteral("c:/links/CODEX.exe");
        source.setCanonicalKey(processLink, QStringLiteral("c:/real/codex.exe"));
        source.setCanonicalKey(direct, QStringLiteral("c:/real/codex.exe"));

        QCOMPARE(
            CodexInstallationDiscovery::candidates(environment, source),
            QVector<QString>({processLink, hashExecutable}));
    }

    void filtersAppServerLaunchCandidatesByExecutableTrust()
    {
        FakeCodexDiscoverySource source;
        CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString configured = displayPath(
            QStringLiteral("D:/Explicit/custom-codex.exe"));
        const QString rogueRunning = displayPath(
            QStringLiteral(
                "C:/Users/Test/Downloads/codex.exe"));
        const QString package = displayPath(
            QStringLiteral(
                "C:/Program Files/WindowsApps/"
                "OpenAI.Codex_26.715.7063.0_x64__"
                "2p2nqsd0c76g0/app/resources/codex.exe"));
        const QString lookalikePackage = displayPath(
            QStringLiteral(
                "C:/Program Files/WindowsApps/"
                "OpenAI.Codex_26.715.7063.0_x64__"
                "lookalike/app/resources/codex.exe"));
        const QString direct = childPath(
            environment.codexBinRoot,
            QStringLiteral("codex.exe"));
        const QString hashDirectory = childPath(
            environment.codexBinRoot,
            QStringLiteral("trusted-hash"));
        const QString hashExecutable = childPath(
            hashDirectory,
            QStringLiteral("codex.exe"));
        const QString olderHashDirectory = childPath(
            environment.codexBinRoot,
            QStringLiteral("older-hash"));
        const QString olderHashExecutable = childPath(
            olderHashDirectory,
            QStringLiteral("codex.exe"));
        const QString roguePath = displayPath(
            QStringLiteral("C:/Tools/codex.exe"));

        environment.configuredExecutable = configured;
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            displayPath(QStringLiteral("C:/Program Files")));
        source.environment.insert(
            QStringLiteral("ProgramW6432"),
            displayPath(QStringLiteral("C:/Program Files")));
        source.processImages = {
            rogueRunning,
            lookalikePackage,
            package,
        };
        source.setChildren(
            environment.codexBinRoot,
            {
                olderHashDirectory,
                hashDirectory,
            });
        source.setAttributes(
            olderHashExecutable,
            true,
            true,
            10);
        source.setAttributes(
            hashExecutable,
            true,
            true,
            20);
        source.pathExecutable = roguePath;

        QCOMPARE(
            CodexInstallationDiscovery::
                trustedAppServerCandidates(
                    environment, source),
            QVector<QString>({
                configured,
                hashExecutable,
                olderHashExecutable,
                direct,
                package,
            }));
    }

    void newerHashedRuntimeOutranksOlderRunningRuntime()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString direct = childPath(
            environment.codexBinRoot,
            QStringLiteral("codex.exe"));
        const QString olderDirectory = childPath(
            environment.codexBinRoot,
            QStringLiteral("older-running"));
        const QString newerDirectory = childPath(
            environment.codexBinRoot,
            QStringLiteral("newer-installed"));
        const QString older = childPath(
            olderDirectory,
            QStringLiteral("codex.exe"));
        const QString newer = childPath(
            newerDirectory,
            QStringLiteral("codex.exe"));

        source.processImages = {older};
        source.setChildren(
            environment.codexBinRoot,
            {olderDirectory, newerDirectory});
        source.setAttributes(older, true, true, 10);
        source.setAttributes(newer, true, true, 20);

        QCOMPARE(
            CodexInstallationDiscovery::
                trustedAppServerCandidates(
                    environment, source),
            QVector<QString>({
                newer,
                older,
                direct,
            }));
    }

    void firstRunnablePrefersNewestHashedRuntimeOverStaleDirectRuntime()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString direct = childPath(
            environment.codexBinRoot,
            QStringLiteral("codex.exe"));
        const QString newerDirectory = childPath(
            environment.codexBinRoot,
            QStringLiteral("current-hash"));
        const QString newer = childPath(
            newerDirectory,
            QStringLiteral("codex.exe"));

        source.setChildren(
            environment.codexBinRoot,
            {newerDirectory});
        source.setAttributes(direct, true, true, 10);
        source.setAttributes(newer, true, true, 20);

        const auto result =
            CodexInstallationDiscovery::firstRunnable(
                environment, source);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value(), newer);
    }

    void spoofedProgramFilesEnvironmentDoesNotGrantPackageTrust()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString spoofedPackage = displayPath(
            QStringLiteral(
                "D:/Attacker/WindowsApps/"
                "OpenAI.Codex_26.715.7063.0_x64__"
                "2p2nqsd0c76g0/app/resources/codex.exe"));
        source.environment.insert(
            QStringLiteral("ProgramFiles"),
            displayPath(QStringLiteral("D:/Attacker")));
        source.environment.insert(
            QStringLiteral("ProgramW6432"),
            displayPath(QStringLiteral("D:/Attacker")));
        source.processImages = {spoofedPackage};

        const QVector<QString> trusted =
            CodexInstallationDiscovery::
                trustedAppServerCandidates(
                    environment, source);

        QVERIFY(!trusted.contains(spoofedPackage));
    }

    void discoversOfficialPackageFallbackWithoutRunningCodex()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString package = displayPath(
            QStringLiteral(
                "C:/Program Files/WindowsApps/"
                "OpenAI.Codex_26.715.7063.0_x64__"
                "2p2nqsd0c76g0/app/resources/codex.exe"));
        source.packageExecutables = {package};
        source.setAttributes(package, true, true, 30);

        const QVector<QString> trusted =
            CodexInstallationDiscovery::
                trustedAppServerCandidates(
                    environment, source);

        QVERIFY(trusted.contains(package));
        QCOMPARE(trusted.last(), package);
    }

    void trustsOfficialPackageFallbackOnSecondaryAppxVolume()
    {
        FakeCodexDiscoverySource source;
        const CodexEnvironment environment =
            discoveredEnvironment(source);
        const QString package = displayPath(
            QStringLiteral(
                "E:/WindowsApps/"
                "OpenAI.Codex_26.715.7063.0_x64__"
                "2p2nqsd0c76g0/app/resources/codex.exe"));
        source.packageExecutables = {package};
        source.setAttributes(package, true, true, 40);

        const QVector<QString> trusted =
            CodexInstallationDiscovery::
                trustedAppServerCandidates(
                    environment, source);

        QVERIFY(trusted.contains(package));
        QCOMPARE(trusted.last(), package);
    }

    void fallsThroughInvalidHighPriorityCandidates()
    {
        FakeCodexDiscoverySource source;
        CodexEnvironment environment = discoveredEnvironment(source);
        const QString configured =
            displayPath(QStringLiteral("D:/Missing/codex.exe"));
        const QString processDirectory =
            displayPath(QStringLiteral("C:/Directory/codex.exe"));
        const QString direct =
            childPath(environment.codexBinRoot, QStringLiteral("codex.exe"));

        environment.configuredExecutable = configured;
        source.processImages = {processDirectory};
        source.setAttributes(configured, false, false);
        source.setAttributes(processDirectory, true, false);
        source.setAttributes(direct, true, true);

        const auto result =
            CodexInstallationDiscovery::firstRunnable(environment, source);

        QVERIFY(result.hasValue());
        QCOMPARE(result.value(), direct);
    }

    void rejectsDirectoriesAndNonExecutablesWithTypedError()
    {
        FakeCodexDiscoverySource source;
        CodexEnvironment environment = discoveredEnvironment(source);
        const QString configuredText =
            displayPath(QStringLiteral("D:/Configured/codex.txt"));
        const QString processDirectory =
            displayPath(QStringLiteral("C:/Directory/codex.exe"));
        const QString direct =
            childPath(environment.codexBinRoot, QStringLiteral("codex.exe"));

        environment.configuredExecutable = configuredText;
        source.processImages = {processDirectory};
        source.setAttributes(configuredText, true, true);
        source.setAttributes(processDirectory, true, false);
        source.setAttributes(direct, false, false);

        const auto candidates =
            CodexInstallationDiscovery::candidates(environment, source);
        const auto result =
            CodexInstallationDiscovery::firstRunnable(environment, source);

        QVERIFY(!result.hasValue());
        QCOMPARE(
            result.error().code,
            QStringLiteral("codex.executable_not_found"));
        QCOMPARE(
            result.error().context.value(
                QStringLiteral("candidateCount")).toInt(),
            candidates.size());
    }

    void acceptsTemporaryExecutableWithoutLaunching()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable =
            directory.filePath(QStringLiteral("codex.exe"));
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource();
        CodexEnvironment environment;
        environment.codexBinRoot =
            source.absoluteDisplayPath(directory.path());
        environment.configuredExecutable =
            source.absoluteDisplayPath(executable);

        const auto result =
            CodexInstallationDiscovery::firstRunnable(environment, source);

        QVERIFY(result.hasValue());
        QCOMPARE(
            result.value(),
            source.absoluteDisplayPath(executable));
        QCOMPARE(QFileInfo(executable).size(), 0);
    }

    void canonicalizesExtendedExistingPath()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable =
            directory.filePath(QStringLiteral("codex.exe"));
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();

        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource();
        const QString normal = QDir::toNativeSeparators(
            QFileInfo(executable).absoluteFilePath());
        const QString extended = QStringLiteral("\\\\?\\") + normal;

        QCOMPARE(
            source.absoluteDisplayPath(extended),
            source.absoluteDisplayPath(normal));
        QCOMPARE(
            source.canonicalComparisonKey(extended),
            source.canonicalComparisonKey(normal));
    }

    void discoversLiveRuntimeWhenRequested()
    {
        if (!qEnvironmentVariableIsSet(
                "CODEX_COMPANION_VERIFY_LIVE_CODEX")) {
            QSKIP("live Codex discovery was not requested");
        }

        const auto environment = CodexEnvironment::discover();
        QVERIFY(environment.hasValue());
        const auto executable =
            CodexInstallationDiscovery::firstRunnable(environment.value());
        const QString errorMessage = executable.hasValue()
            ? QString()
            : executable.error().message;
        QVERIFY2(
            executable.hasValue(),
            qPrintable(errorMessage));
        QVERIFY(
            QFileInfo(executable.value()).isFile());
        QVERIFY(
            executable.value().endsWith(
                QStringLiteral(".exe"), Qt::CaseInsensitive));

        const ICodexDiscoverySource& source =
            systemCodexDiscoverySource();
        const QVector<QString> programFiles =
            source.protectedProgramFilesDirectories();
        QVERIFY(!programFiles.isEmpty());
        const QVector<QString> packages =
            source.installedCodexPackageExecutables();
        QVERIFY(!packages.isEmpty());
        QVERIFY(std::all_of(
            packages.cbegin(),
            packages.cend(),
            [](const QString& package) {
                return package.endsWith(
                    QStringLiteral(
                        "app\\resources\\codex.exe"),
                    Qt::CaseInsensitive);
            }));
    }
};

QTEST_GUILESS_MAIN(CodexDiscoveryTests)
#include "CodexDiscoveryTests.moc"
