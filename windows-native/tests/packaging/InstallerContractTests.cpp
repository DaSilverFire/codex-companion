#include <vector>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <windows.h>

namespace {

struct ProcessResult {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QByteArray standardOutput;
    QByteArray standardError;
    QString processError;
};

struct VersionTranslation {
    WORD language;
    WORD codePage;
};

using InnoSections = QHash<QString, QStringList>;
using SetupValues = QHash<QString, QString>;

QString installerScriptPath()
{
    return QStringLiteral(COMPANION_INSTALLER_SCRIPT_PATH);
}

QString releaseBuildScriptPath()
{
    return QStringLiteral(COMPANION_RELEASE_BUILD_SCRIPT_PATH);
}

QString powershellPath()
{
    return QDir(qEnvironmentVariable("SystemRoot"))
        .filePath(QStringLiteral(
            "System32/WindowsPowerShell/v1.0/powershell.exe"));
}

bool writeFile(
    const QString& path,
    const QByteArray& contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(contents) == qint64(contents.size());
}

bool writeStageContractFiles(
    const QString& stageDirectory)
{
    const QDir stage(stageDirectory);
    const QStringList requiredFiles = {
        QStringLiteral(
            "bin/CodexCompanion.exe"),
        QStringLiteral(
            "bin/CodexCompanionUpdater.exe"),
        QStringLiteral("bin/Qt6Core.dll"),
        QStringLiteral("bin/Qt6Gui.dll"),
        QStringLiteral("bin/Qt6Network.dll"),
        QStringLiteral("bin/Qt6Qml.dll"),
        QStringLiteral("bin/Qt6Quick.dll"),
        QStringLiteral(
            "bin/Qt6QuickControls2.dll"),
        QStringLiteral("bin/Qt6Sql.dll"),
        QStringLiteral(
            "bin/Qt6WebSockets.dll"),
        QStringLiteral("bin/msvcp140.dll"),
        QStringLiteral(
            "bin/msvcp140_1.dll"),
        QStringLiteral(
            "bin/vcruntime140.dll"),
        QStringLiteral(
            "bin/vcruntime140_1.dll"),
        QStringLiteral(
            "plugins/platforms/qwindows.dll"),
        QStringLiteral(
            "plugins/tls/qschannelbackend.dll"),
        QStringLiteral(
            "plugins/sqldrivers/qsqlite.dll"),
        QStringLiteral(
            "plugins/imageformats/qwebp.dll"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "SKILL.md"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "agents/openai.yaml"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "references/"
            "codex-pet-schema-2026-07-13.json"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "references/companion-contract.md"),
        QStringLiteral(
            "resources/skills/companion-pet/"
            "scripts/companion_pet_assets.py"),
        QStringLiteral(
            "qml/QtQuick/qmldir"),
    };
    for (const QString& relativePath :
         requiredFiles) {
        const QString path =
            stage.filePath(relativePath);
        if (!QDir().mkpath(
                QFileInfo(path)
                    .absolutePath())) {
            return false;
        }
        if (!writeFile(
                path,
                QByteArrayLiteral(
                    "stage-file"))) {
            return false;
        }
    }
    return true;
}

InnoSections parseInnoSections(
    const QString& source)
{
    InnoSections sections;
    QString currentSection;
    const QStringList lines =
        source.split(QRegularExpression(QStringLiteral("[\r\n]+")));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty()
            || line.startsWith(QLatin1Char(';'))) {
            continue;
        }
        if (line.startsWith(QLatin1Char('['))
            && line.endsWith(QLatin1Char(']'))) {
            currentSection =
                line.sliced(1, line.size() - 2).toLower();
            continue;
        }
        if (!currentSection.isEmpty()) {
            sections[currentSection].append(line);
        }
    }
    return sections;
}

SetupValues parseSetupValues(
    const QStringList& lines)
{
    SetupValues values;
    for (const QString& line : lines) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            continue;
        }
        values.insert(
            line.first(equals).trimmed().toLower(),
            line.sliced(equals + 1).trimmed());
    }
    return values;
}

QStringList requiredBuildArguments()
{
    return {
        QStringLiteral("-Version"),
        QStringLiteral("0.3.4"),
        QStringLiteral("-Build"),
        QStringLiteral("1"),
        QStringLiteral("-StageDir"),
        QStringLiteral("C:\\missing-stage"),
        QStringLiteral("-OutputDir"),
        QStringLiteral("C:\\missing-output"),
        QStringLiteral("-IconPath"),
        QStringLiteral("C:\\missing-icon.ico"),
        QStringLiteral("-IsccPath"),
        QStringLiteral("C:\\missing-iscc.exe"),
    };
}

ProcessResult runReleaseBuild(
    const QStringList& arguments,
    const QProcessEnvironment& environment =
        QProcessEnvironment::systemEnvironment())
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setProgram(powershellPath());
    process.setArguments(
        QStringList{
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-File"),
            releaseBuildScriptPath(),
        }
        + arguments);
    process.start();

    ProcessResult result;
    result.started = process.waitForStarted(10'000);
    if (!result.started) {
        result.processError = process.errorString();
        return result;
    }
    result.finished = process.waitForFinished(30'000);
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    result.processError = process.errorString();
    return result;
}

QByteArray processDiagnostics(
    const ProcessResult& result)
{
    return result.standardOutput
        + result.standardError
        + result.processError.toLocal8Bit();
}

QString versionStringValue(
    const std::vector<BYTE>& versionInfo,
    const QString& name)
{
    void* rawTranslations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(
            versionInfo.data(),
            L"\\VarFileInfo\\Translation",
            &rawTranslations,
            &translationBytes)
        || rawTranslations == nullptr) {
        return {};
    }

    const auto* translations =
        static_cast<const VersionTranslation*>(rawTranslations);
    const UINT translationCount =
        translationBytes / sizeof(VersionTranslation);
    for (UINT index = 0;
         index < translationCount;
         ++index) {
        const QString query =
            QStringLiteral("\\StringFileInfo\\%1%2\\%3")
                .arg(
                    qulonglong(translations[index].language),
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(
                    qulonglong(translations[index].codePage),
                    4,
                    16,
                    QLatin1Char('0'))
                .arg(name);
        void* rawValue = nullptr;
        UINT valueCharacters = 0;
        if (VerQueryValueW(
                versionInfo.data(),
                reinterpret_cast<const wchar_t*>(query.utf16()),
                &rawValue,
                &valueCharacters)
            && rawValue != nullptr
            && valueCharacters > 0) {
            return QString::fromWCharArray(
                       static_cast<const wchar_t*>(rawValue),
                       int(valueCharacters - 1))
                .trimmed();
        }
    }
    return {};
}

BOOL CALLBACK countIconGroups(
    HMODULE,
    LPCWSTR,
    LPWSTR,
    LONG_PTR parameter)
{
    auto* count =
        reinterpret_cast<int*>(parameter);
    ++(*count);
    return TRUE;
}

class InstallerContractTests final
    : public QObject {
    Q_OBJECT

private slots:
    void innoSourceDefinesInstallerIdentityAndArchitecture();
    void innoSourceRestrictsPayloadAndShellIntegration();
    void buildScriptRejectsMissingRequiredInputs();
    void buildScriptRejectsMissingRequiredPaths();
    void buildScriptRejectsBundledPetArtwork();
    void buildScriptValidatesSemVerAndNumericBuild();
    void buildScriptInvokesIsccWithValidatedDefinitions();
    void buildScriptGeneratesManifestWithoutPrivateKeyArguments();
    void buildScriptConfiguresAuthenticodeSigning();
    void compiledInstallerResourcesMatchContract();
};

void InstallerContractTests::
    innoSourceDefinesInstallerIdentityAndArchitecture()
{
    QFile file(installerScriptPath());
    QVERIFY2(
        file.open(QIODevice::ReadOnly),
        qPrintable(file.errorString()));
    const QString source =
        QString::fromUtf8(file.readAll());
    const InnoSections sections =
        parseInnoSections(source);
    const SetupValues setup =
        parseSetupValues(
            sections.value(QStringLiteral("setup")));

    QCOMPARE(
        setup.value(QStringLiteral("appid")),
        QStringLiteral(
            "{{9B3C42CB-4B7F-4A08-B675-071708948C88}"));
    QCOMPARE(
        setup.value(QStringLiteral("appname")),
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        setup.value(QStringLiteral("apppublisher")),
        QStringLiteral("DaSilverFire"));
    QCOMPARE(
        setup.value(QStringLiteral("defaultdirname")),
        QStringLiteral(
            "{localappdata}\\Programs\\Codex Companion"));
    QCOMPARE(
        setup.value(QStringLiteral("defaultgroupname")),
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        setup.value(QStringLiteral("privilegesrequired")),
        QStringLiteral("lowest"));
    QCOMPARE(
        setup.value(
            QStringLiteral(
                "privilegesrequiredoverridesallowed")),
        QString());
    QVERIFY(!setup.contains(
        QStringLiteral("setuparchitecture")));
    QCOMPARE(
        setup.value(QStringLiteral("architecturesallowed")),
        QStringLiteral("x64compatible"));
    QCOMPARE(
        setup.value(
            QStringLiteral(
                "architecturesinstallin64bitmode")),
        QStringLiteral("x64compatible"));
    QCOMPARE(
        setup.value(QStringLiteral("minversion")),
        QStringLiteral("10.0.22000"));
    QCOMPARE(
        setup.value(QStringLiteral("appversion")),
        QStringLiteral("{#Version}"));
    QCOMPARE(
        setup.value(QStringLiteral("outputdir")),
        QStringLiteral("{#OutputDir}"));
    QCOMPARE(
        setup.value(QStringLiteral("outputbasefilename")),
        QStringLiteral(
            "Codex-Companion-{#Version}-{#Build}-windows-x64"));
    QCOMPARE(
        setup.value(QStringLiteral("versioninfoversion")),
        QStringLiteral(
            "{#VersionMajor}.{#VersionMinor}."
            "{#VersionPatch}.{#Build}"));
    QCOMPARE(
        setup.value(
            QStringLiteral("versioninfoproductname")),
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        setup.value(
            QStringLiteral(
                "versioninfooriginalfilename")),
        QStringLiteral(
            "Codex-Companion-{#Version}-{#Build}-windows-x64.exe"));
    QCOMPARE(
        setup.value(
            QStringLiteral(
                "versioninfoproducttextversion")),
        QStringLiteral(
            "cc-update/1|{#Version}|{#Build}|"
            "w|x64|10.0.22000"));
    QCOMPARE(
        setup.value(QStringLiteral("setupiconfile")),
        QStringLiteral("{#IconPath}"));
    QCOMPARE(
        setup.value(
            QStringLiteral("uninstalldisplayicon")),
        QStringLiteral(
            "{app}\\bin\\CodexCompanion.exe"));
    QCOMPARE(
        setup.value(
            QStringLiteral("uninstalldisplayname")),
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        setup.value(
            QStringLiteral("createuninstallregkey")),
        QStringLiteral("yes"));
    QCOMPARE(
        setup.value(QStringLiteral("uninstallable")),
        QStringLiteral("yes"));
}

void InstallerContractTests::
    innoSourceRestrictsPayloadAndShellIntegration()
{
    QFile installerFile(installerScriptPath());
    QVERIFY2(
        installerFile.open(QIODevice::ReadOnly),
        qPrintable(installerFile.errorString()));
    const QString installerSource =
        QString::fromUtf8(installerFile.readAll());
    const InnoSections sections =
        parseInnoSections(installerSource);

    const QStringList expectedFiles = {
        QStringLiteral(
            "Source: \"{#SourceDir}\\*\"; DestDir: \"{app}\"; "
            "Flags: ignoreversion recursesubdirs createallsubdirs"),
        QStringLiteral(
            "Source: \"{#SourceDir}\\resources\\skills\\companion-pet\\*\"; "
            "DestDir: \"{%USERPROFILE}\\.codex\\skills\\companion-pet\"; "
            "Flags: ignoreversion recursesubdirs createallsubdirs "
            "uninsneveruninstall"),
    };
    QCOMPARE(
        sections.value(QStringLiteral("files")),
        expectedFiles);

    const QStringList expectedIcons = {
        QStringLiteral(
            "Name: \"{group}\\Codex Companion\"; "
            "Filename: \"{app}\\bin\\CodexCompanion.exe\"; "
            "WorkingDir: \"{app}\\bin\"; "
            "IconFilename: \"{app}\\bin\\CodexCompanion.exe\""),
    };
    QCOMPARE(
        sections.value(QStringLiteral("icons")),
        expectedIcons);

    const QStringList expectedRun = {
        QStringLiteral(
            "Filename: \"{app}\\bin\\CodexCompanion.exe\"; "
            "WorkingDir: \"{app}\\bin\"; "
            "Description: \"Launch Codex Companion\"; "
            "Flags: postinstall nowait skipifsilent"),
    };
    QCOMPARE(
        sections.value(QStringLiteral("run")),
        expectedRun);

    const QStringList expectedUninstallDelete = {
        QStringLiteral(
            "Type: filesandordirs; Name: \"{app}\""),
    };
    QCOMPARE(
        sections.value(
            QStringLiteral("uninstalldelete")),
        expectedUninstallDelete);

    QVERIFY(
        !installerSource.contains(
            QStringLiteral("desktop"),
            Qt::CaseInsensitive));
    QVERIFY(
        !installerSource.contains(
            QStringLiteral("dotnet"),
            Qt::CaseInsensitive));
    QVERIFY(
        !installerSource.contains(
            QStringLiteral("download"),
            Qt::CaseInsensitive));
    QVERIFY(
        !installerSource.contains(
            QStringLiteral("\\resources\\pets\\"),
            Qt::CaseInsensitive));

    QFile releaseFile(releaseBuildScriptPath());
    QVERIFY2(
        releaseFile.open(QIODevice::ReadOnly),
        qPrintable(releaseFile.errorString()));
    const QString releaseSource =
        QString::fromUtf8(releaseFile.readAll());
    const QStringList forbiddenAcquisitionTokens = {
        QStringLiteral("Invoke-WebRequest"),
        QStringLiteral("Start-BitsTransfer"),
        QStringLiteral("winget"),
        QStringLiteral("dotnet-install"),
    };
    for (const QString& token :
         forbiddenAcquisitionTokens) {
        QVERIFY2(
            !releaseSource.contains(
                token,
                Qt::CaseInsensitive),
            qPrintable(token));
    }
    QVERIFY(
        releaseSource.contains(
            QStringLiteral(
                "CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64")));
    QVERIFY(
        releaseSource.contains(
            QStringLiteral(
                "resources\\pets"),
            Qt::CaseInsensitive));
    QVERIFY(
        !releaseSource.contains(
            QStringLiteral("--private-key")));
    QVERIFY(
        installerSource.contains(
            QStringLiteral(
                "#ifdef EnableSigning")));
    QVERIFY(
        installerSource.contains(
            QStringLiteral(
                "SignedUninstaller=yes")));
    QVERIFY(
        installerSource.contains(
            QStringLiteral(
                "SignTool=companion")));
}

void InstallerContractTests::
    buildScriptRejectsBundledPetArtwork()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString stagePath =
        root.filePath(QStringLiteral("stage"));
    const QString outputPath =
        root.filePath(QStringLiteral("output"));
    QVERIFY(writeStageContractFiles(stagePath));
    QVERIFY(QDir().mkpath(outputPath));

    const QString bundledPetPath =
        QDir(stagePath)
            .filePath(
                QStringLiteral(
                    "resources/pets/private-pet/"
                    "pet.json"));
    QVERIFY(
        QDir().mkpath(
            QFileInfo(bundledPetPath)
                .absolutePath()));
    QVERIFY(
        writeFile(
            bundledPetPath,
            QByteArrayLiteral("{}")));

    const ProcessResult result =
        runReleaseBuild({
            QStringLiteral("-Version"),
            QStringLiteral("0.3.5"),
            QStringLiteral("-Build"),
            QStringLiteral("1"),
            QStringLiteral("-StageDir"),
            stagePath,
            QStringLiteral("-OutputDir"),
            outputPath,
            QStringLiteral("-IconPath"),
            root.filePath(
                QStringLiteral("missing.ico")),
            QStringLiteral("-IsccPath"),
            root.filePath(
                QStringLiteral("missing-iscc.exe")),
        });
    QVERIFY2(
        result.started && result.finished,
        processDiagnostics(result).constData());
    QVERIFY(result.exitCode != 0);
    const QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        diagnostics.contains(
            "must not contain bundled pet artwork"),
        diagnostics.constData());
}

void InstallerContractTests::
    buildScriptRejectsMissingRequiredInputs()
{
    const QStringList requiredInputs = {
        QStringLiteral("Version"),
        QStringLiteral("Build"),
        QStringLiteral("StageDir"),
        QStringLiteral("OutputDir"),
        QStringLiteral("IconPath"),
        QStringLiteral("IsccPath"),
    };
    for (const QString& input : requiredInputs) {
        QStringList arguments =
            requiredBuildArguments();
        const qsizetype index =
            arguments.indexOf(
                QStringLiteral("-") + input);
        QVERIFY(index >= 0);
        arguments.removeAt(index);
        arguments.removeAt(index);

        const ProcessResult result =
            runReleaseBuild(arguments);
        const QByteArray diagnostics =
            processDiagnostics(result);
        QVERIFY2(
            result.started,
            diagnostics.constData());
        QVERIFY2(
            result.finished,
            diagnostics.constData());
        QCOMPARE(
            result.exitStatus,
            QProcess::NormalExit);
        QVERIFY2(
            result.exitCode != 0,
            diagnostics.constData());
        QVERIFY2(
            diagnostics.contains(
                (QStringLiteral("-")
                 + input
                 + QStringLiteral(" is required."))
                    .toLocal8Bit()),
            diagnostics.constData());
    }
}

void InstallerContractTests::
    buildScriptRejectsMissingRequiredPaths()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString stageDirectory =
        root.filePath(QStringLiteral("stage"));
    const QString outputDirectory =
        root.filePath(QStringLiteral("output"));
    const QString iconPath =
        root.filePath(QStringLiteral("companion.ico"));
    const QString isccPath =
        root.filePath(QStringLiteral("iscc.cmd"));

    const auto runWithPaths =
        [&](
            const QString& stage,
            const QString& output,
            const QString& icon,
            const QString& iscc) {
            return runReleaseBuild(
                {
                    QStringLiteral("-Version"),
                    QStringLiteral("0.3.4"),
                    QStringLiteral("-Build"),
                    QStringLiteral("1"),
                    QStringLiteral("-StageDir"),
                    stage,
                    QStringLiteral("-OutputDir"),
                    output,
                    QStringLiteral("-IconPath"),
                    icon,
                    QStringLiteral("-IsccPath"),
                    iscc,
                });
        };

    QVERIFY(
        writeFile(
            iconPath,
            QByteArrayLiteral("icon")));
    QVERIFY(
        writeFile(
            isccPath,
            QByteArrayLiteral("@exit /b 0\r\n")));

    ProcessResult result =
        runWithPaths(
            stageDirectory,
            outputDirectory,
            iconPath,
            isccPath);
    QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.started && result.finished,
        diagnostics.constData());
    QVERIFY2(
        result.exitCode != 0,
        diagnostics.constData());
    QVERIFY2(
        diagnostics.contains(
            "-StageDir directory was not found:"),
        diagnostics.constData());

    QVERIFY(
        writeStageContractFiles(
            stageDirectory));

    const QString missingIcon =
        root.filePath(
            QStringLiteral("missing.ico"));
    result =
        runWithPaths(
            stageDirectory,
            outputDirectory,
            missingIcon,
            isccPath);
    diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.exitCode != 0,
        diagnostics.constData());
    QVERIFY2(
        diagnostics.contains(
            "-IconPath file was not found:"),
        diagnostics.constData());

    const QString missingIscc =
        root.filePath(
            QStringLiteral("missing-iscc.exe"));
    result =
        runWithPaths(
            stageDirectory,
            outputDirectory,
            iconPath,
            missingIscc);
    diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.exitCode != 0,
        diagnostics.constData());
    QVERIFY2(
        diagnostics.contains(
            "-IsccPath file was not found:"),
        diagnostics.constData());

    const QString outputFile =
        root.filePath(
            QStringLiteral("not-a-directory"));
    QVERIFY(
        writeFile(
            outputFile,
            QByteArrayLiteral("file")));
    result =
        runWithPaths(
            stageDirectory,
            outputFile,
            iconPath,
            isccPath);
    diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.exitCode != 0,
        diagnostics.constData());
    QVERIFY2(
        diagnostics.contains(
            "-OutputDir must identify a directory:"),
        diagnostics.constData());
}

void InstallerContractTests::
    buildScriptValidatesSemVerAndNumericBuild()
{
    const QStringList invalidSemVer = {
        QStringLiteral("1.2"),
        QStringLiteral("01.2.3"),
        QStringLiteral("1.2.3-01"),
    };
    for (const QString& version : invalidSemVer) {
        QStringList arguments =
            requiredBuildArguments();
        arguments[1] = version;
        const ProcessResult result =
            runReleaseBuild(arguments);
        const QByteArray diagnostics =
            processDiagnostics(result);
        QVERIFY2(
            result.started && result.finished,
            diagnostics.constData());
        QVERIFY2(
            result.exitCode != 0,
            diagnostics.constData());
        QVERIFY2(
            diagnostics.contains(
                "-Version must be a valid SemVer 2.0 value."),
            diagnostics.constData());
    }

    const QStringList outOfRangeVersions = {
        QStringLiteral("65536.2.3"),
        QStringLiteral("1.65536.3"),
        QStringLiteral("1.2.65536"),
    };
    for (const QString& version :
         outOfRangeVersions) {
        QStringList arguments =
            requiredBuildArguments();
        arguments[1] = version;
        const ProcessResult result =
            runReleaseBuild(arguments);
        const QByteArray diagnostics =
            processDiagnostics(result);
        QVERIFY2(
            result.started && result.finished,
            diagnostics.constData());
        QVERIFY2(
            result.exitCode != 0,
            diagnostics.constData());
        QVERIFY2(
            diagnostics.contains(
                "must be between 0 and 65535."),
            diagnostics.constData());
    }

    const QStringList invalidBuilds = {
        QStringLiteral("0"),
        QStringLiteral("65536"),
        QStringLiteral("1.5"),
        QStringLiteral("release"),
    };
    for (const QString& build : invalidBuilds) {
        QStringList arguments =
            requiredBuildArguments();
        arguments[3] = build;
        const ProcessResult result =
            runReleaseBuild(arguments);
        const QByteArray diagnostics =
            processDiagnostics(result);
        QVERIFY2(
            result.started && result.finished,
            diagnostics.constData());
        QVERIFY2(
            result.exitCode != 0,
            diagnostics.constData());
        QVERIFY2(
            diagnostics.contains(
                "-Build must be a positive integer "
                "between 1 and 65535."),
            diagnostics.constData());
    }

    QStringList arguments =
        requiredBuildArguments();
    arguments[1] =
        QStringLiteral(
            "1.2.3-abcdefghijklmnopqrstuvwxyz");
    const ProcessResult result =
        runReleaseBuild(arguments);
    const QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.started && result.finished,
        diagnostics.constData());
    QVERIFY2(
        result.exitCode != 0,
        diagnostics.constData());
    QVERIFY2(
        diagnostics.contains(
            "Installer ProductVersion metadata must not "
            "exceed 50 characters."),
        diagnostics.constData());
}

void InstallerContractTests::
    buildScriptInvokesIsccWithValidatedDefinitions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString stageDirectory =
        root.filePath(QStringLiteral("stage"));
    const QString outputDirectory =
        root.filePath(QStringLiteral("output"));
    const QString iconPath =
        root.filePath(QStringLiteral("companion.ico"));
    const QString fakeIsccPath =
        root.filePath(QStringLiteral("fake-iscc.cmd"));
    const QString argumentLogPath =
        root.filePath(QStringLiteral("iscc-arguments.txt"));
    const QString version =
        QStringLiteral("0.3.4-rc.1+build.5");
    const QString build = QStringLiteral("42");
    const QString installerPath =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "Codex-Companion-%1-%2-windows-x64.exe")
                .arg(version, build));
    const QString portableDirectory =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "portable/Codex Companion"));

    QVERIFY(
        writeStageContractFiles(
            stageDirectory));
    QVERIFY(
        writeFile(
            iconPath,
            QByteArrayLiteral("icon")));
    QVERIFY(
        writeFile(
            fakeIsccPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                "setlocal\r\n"
                "> \"%COMPANION_FAKE_ISCC_LOG%\" echo %*\r\n"
                "copy /y \"%ComSpec%\" "
                "\"%COMPANION_FAKE_INSTALLER%\" >nul\r\n"
                "exit /b 0\r\n")));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral("COMPANION_FAKE_ISCC_LOG"),
        argumentLogPath);
    environment.insert(
        QStringLiteral("COMPANION_FAKE_INSTALLER"),
        installerPath);

    const ProcessResult result =
        runReleaseBuild(
            {
                QStringLiteral("-Version"),
                version,
                QStringLiteral("-Build"),
                build,
                QStringLiteral("-StageDir"),
                stageDirectory,
                QStringLiteral("-OutputDir"),
                outputDirectory,
                QStringLiteral("-IconPath"),
                iconPath,
                QStringLiteral("-IsccPath"),
                fakeIsccPath,
            },
            environment);
    const QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.started && result.finished,
        diagnostics.constData());
    QCOMPARE(
        result.exitStatus,
        QProcess::NormalExit);
    QVERIFY2(
        result.exitCode == 0,
        diagnostics.constData());
    QVERIFY2(
        QFileInfo(installerPath).isFile(),
        qPrintable(installerPath));
    QVERIFY(
        QString::fromLocal8Bit(
            result.standardOutput)
            .contains(
                QDir::toNativeSeparators(
                    installerPath)));

    QFile argumentLog(argumentLogPath);
    QVERIFY2(
        argumentLog.open(QIODevice::ReadOnly),
        qPrintable(argumentLog.errorString()));
    const QString arguments =
        QString::fromLocal8Bit(
            argumentLog.readAll());
    const QStringList expectedDefinitions = {
        QStringLiteral("/DVersion=%1")
            .arg(version),
        QStringLiteral("/DVersionMajor=0"),
        QStringLiteral("/DVersionMinor=3"),
        QStringLiteral("/DVersionPatch=4"),
        QStringLiteral("/DBuild=%1")
            .arg(build),
        QStringLiteral("/DSourceDir=%1")
            .arg(
                QDir::toNativeSeparators(
                    QFileInfo(portableDirectory)
                        .absoluteFilePath())),
        QStringLiteral("/DOutputDir=%1")
            .arg(
                QDir::toNativeSeparators(
                    QFileInfo(outputDirectory)
                        .absoluteFilePath())),
        QStringLiteral("/DIconPath=%1")
            .arg(
                QDir::toNativeSeparators(
                    QFileInfo(iconPath)
                        .absoluteFilePath())),
        QDir::toNativeSeparators(
            QFileInfo(installerScriptPath())
                .absoluteFilePath()),
    };
    for (const QString& definition :
         expectedDefinitions) {
        QVERIFY2(
            arguments.contains(
                definition,
                Qt::CaseInsensitive),
            qPrintable(
                definition
                 + QStringLiteral("\n")
                 + arguments));
    }

    QVERIFY(
        QFileInfo(
            QDir(portableDirectory).filePath(
                QStringLiteral(
                    "bin/CodexCompanion.exe")))
            .isFile());
    QFile metadata(
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "release-metadata.json")));
    QVERIFY2(
        metadata.open(QIODevice::ReadOnly),
        qPrintable(metadata.errorString()));
    const QJsonDocument metadataDocument =
        QJsonDocument::fromJson(
            metadata.readAll());
    QVERIFY(metadataDocument.isObject());
    QCOMPARE(
        metadataDocument.object()
            .value(
                QStringLiteral(
                    "schemaVersion"))
            .toInt(),
        1);
}

void InstallerContractTests::
    buildScriptGeneratesManifestWithoutPrivateKeyArguments()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString stageDirectory =
        root.filePath(QStringLiteral("stage"));
    const QString outputDirectory =
        root.filePath(QStringLiteral("output"));
    const QString iconPath =
        root.filePath(QStringLiteral("companion.ico"));
    const QString fakeIsccPath =
        root.filePath(QStringLiteral("fake-iscc.cmd"));
    const QString fakeSignerPath =
        root.filePath(QStringLiteral("fake-signer.cmd"));
    const QString signerLogPath =
        root.filePath(QStringLiteral("signer-arguments.txt"));
    const QString installerPath =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "Codex-Companion-0.3.4-42-windows-x64.exe"));
    const QString versionedManifestPath =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "update-windows-x64-0.3.4-42.json"));
    const QString stableManifestPath =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "update-windows-x64.json"));
    constexpr auto privateSentinel =
        "private-seed-must-not-leak";
    constexpr auto publicSentinel =
        "public-key-is-not-secret";

    QVERIFY(
        writeStageContractFiles(
            stageDirectory));
    QVERIFY(
        writeFile(
            iconPath,
            QByteArrayLiteral("icon")));
    QVERIFY(
        writeFile(
            fakeIsccPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                "copy /y \"%ComSpec%\" "
                "\"%COMPANION_FAKE_INSTALLER%\" >nul\r\n"
                "exit /b 0\r\n")));
    QVERIFY(
        writeFile(
            fakeSignerPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                "setlocal\r\n"
                "> \"%COMPANION_FAKE_SIGNER_LOG%\" echo %*\r\n"
                "set \"output=\"\r\n"
                ":parse\r\n"
                "if \"%~1\"==\"\" goto write\r\n"
                "if /i \"%~1\"==\"--output\" set \"output=%~2\"\r\n"
                "shift\r\n"
                "goto parse\r\n"
                ":write\r\n"
                "if \"%output%\"==\"\" exit /b 3\r\n"
                "> \"%output%\" echo {\"fake\":true}\r\n"
                "exit /b 0\r\n")));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "COMPANION_FAKE_INSTALLER"),
        installerPath);
    environment.insert(
        QStringLiteral(
            "COMPANION_FAKE_SIGNER_LOG"),
        signerLogPath);
    environment.insert(
        QStringLiteral(
            "CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64"),
        QString::fromLatin1(
            privateSentinel));
    environment.insert(
        QStringLiteral(
            "CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY"),
        QString::fromLatin1(
            publicSentinel));

    const ProcessResult result =
        runReleaseBuild(
            {
                QStringLiteral("-Version"),
                QStringLiteral("0.3.4"),
                QStringLiteral("-Build"),
                QStringLiteral("42"),
                QStringLiteral("-StageDir"),
                stageDirectory,
                QStringLiteral("-OutputDir"),
                outputDirectory,
                QStringLiteral("-IconPath"),
                iconPath,
                QStringLiteral("-IsccPath"),
                fakeIsccPath,
                QStringLiteral(
                    "-ManifestSignerPath"),
                fakeSignerPath,
                QStringLiteral(
                    "-ArtifactBaseUrl"),
                QStringLiteral(
                    "https://updates.example.test/windows"),
                QStringLiteral("-PublishedAt"),
                QStringLiteral(
                    "2026-07-26T04:00:00Z"),
            },
            environment);
    const QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.started && result.finished,
        diagnostics.constData());
    QVERIFY2(
        result.exitCode == 0,
        diagnostics.constData());
    QVERIFY(
        QFileInfo(versionedManifestPath)
            .isFile());
    QVERIFY(
        QFileInfo(stableManifestPath)
            .isFile());

    QFile versioned(versionedManifestPath);
    QFile stable(stableManifestPath);
    QVERIFY(versioned.open(QIODevice::ReadOnly));
    QVERIFY(stable.open(QIODevice::ReadOnly));
    QCOMPARE(
        versioned.readAll(),
        stable.readAll());

    QFile signerLog(signerLogPath);
    QVERIFY(signerLog.open(QIODevice::ReadOnly));
    const QByteArray signerArguments =
        signerLog.readAll();
    QVERIFY(
        signerArguments.contains(
            "--version 0.3.4"));
    QVERIFY(
        signerArguments.contains(
            "--build 42"));
    QVERIFY(
        signerArguments.contains(
            "https://updates.example.test/windows/"
            "Codex-Companion-0.3.4-42-windows-x64.exe"));
    QVERIFY(
        signerArguments.contains(
            publicSentinel));
    QVERIFY(
        !signerArguments.contains(
            privateSentinel));
    QVERIFY(
        !result.standardOutput.contains(
            privateSentinel));
    QVERIFY(
        !result.standardError.contains(
            privateSentinel));
}

void InstallerContractTests::
    buildScriptConfiguresAuthenticodeSigning()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString stageDirectory =
        root.filePath(QStringLiteral("stage"));
    const QString outputDirectory =
        root.filePath(QStringLiteral("output"));
    const QString iconPath =
        root.filePath(QStringLiteral("companion.ico"));
    const QString fakeIsccPath =
        root.filePath(QStringLiteral("fake-iscc.cmd"));
    const QString fakeSignToolPath =
        root.filePath(QStringLiteral("fake-signtool.cmd"));
    const QString fakeGitPath =
        root.filePath(QStringLiteral("fake-git.cmd"));
    const QString isccLogPath =
        root.filePath(QStringLiteral("iscc-arguments.txt"));
    const QString signToolLogPath =
        root.filePath(QStringLiteral("signtool-arguments.txt"));
    const QString installerPath =
        QDir(outputDirectory).filePath(
            QStringLiteral(
                "Codex-Companion-0.3.4-42-windows-x64.exe"));
    const QString certificate(
        40,
        QLatin1Char('A'));

    QVERIFY(
        writeStageContractFiles(
            stageDirectory));
    QVERIFY(
        writeFile(
            iconPath,
            QByteArrayLiteral("icon")));
    QVERIFY(
        writeFile(
            fakeIsccPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                "> \"%COMPANION_FAKE_ISCC_LOG%\" echo %*\r\n"
                "copy /y \"%ComSpec%\" "
                "\"%COMPANION_FAKE_INSTALLER%\" >nul\r\n"
                "exit /b 0\r\n")));
    QVERIFY(
        writeFile(
            fakeSignToolPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                ">> \"%COMPANION_FAKE_SIGNTOOL_LOG%\" echo %*\r\n"
                "exit /b 0\r\n")));
    QVERIFY(
        writeFile(
            fakeGitPath,
            QByteArrayLiteral(
                "@echo off\r\n"
                "if /i \"%~1\"==\"-C\" goto skiproot\r\n"
                "goto command\r\n"
                ":skiproot\r\n"
                "shift\r\n"
                "shift\r\n"
                ":command\r\n"
                "if /i \"%~1\"==\"status\" goto clean\r\n"
                "if /i \"%~1\"==\"rev-parse\" goto hash\r\n"
                "exit /b 1\r\n"
                ":clean\r\n"
                "exit /b 0\r\n"
                ":hash\r\n"
                "echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n"
                "exit /b 0\r\n")));

    QProcessEnvironment environment =
        QProcessEnvironment::systemEnvironment();
    environment.insert(
        QStringLiteral(
            "COMPANION_FAKE_INSTALLER"),
        installerPath);
    environment.insert(
        QStringLiteral(
            "COMPANION_FAKE_ISCC_LOG"),
        isccLogPath);
    environment.insert(
        QStringLiteral(
            "COMPANION_FAKE_SIGNTOOL_LOG"),
        signToolLogPath);
    environment.insert(
        QStringLiteral(
            "CODEX_COMPANION_SIGN_CERT_SHA1"),
        certificate);
    environment.insert(
        QStringLiteral(
            "CODEX_COMPANION_TIMESTAMP_URL"),
        QStringLiteral(
            "http://timestamp.digicert.com"));

    const ProcessResult result =
        runReleaseBuild(
            {
                QStringLiteral("-Version"),
                QStringLiteral("0.3.4"),
                QStringLiteral("-Build"),
                QStringLiteral("42"),
                QStringLiteral("-StageDir"),
                stageDirectory,
                QStringLiteral("-OutputDir"),
                outputDirectory,
                QStringLiteral("-IconPath"),
                iconPath,
                QStringLiteral("-IsccPath"),
                fakeIsccPath,
                QStringLiteral("-SignToolPath"),
                fakeSignToolPath,
                QStringLiteral("-SourceRoot"),
                root.absolutePath(),
                QStringLiteral("-GitPath"),
                fakeGitPath,
            },
            environment);
    const QByteArray diagnostics =
        processDiagnostics(result);
    QVERIFY2(
        result.started && result.finished,
        diagnostics.constData());
    QVERIFY2(
        result.exitCode == 0,
        diagnostics.constData());

    QFile isccLog(isccLogPath);
    QVERIFY(isccLog.open(QIODevice::ReadOnly));
    const QByteArray isccArguments =
        isccLog.readAll();
    QVERIFY(
        isccArguments.contains(
            "/DEnableSigning=1"));
    QVERIFY(
        isccArguments.contains(
            "/Scompanion="));
    QVERIFY(
        isccArguments.contains(
            certificate.toLatin1()));
    QVERIFY(
        isccArguments.contains(
            "http://timestamp.digicert.com"));

    QFile signToolLog(signToolLogPath);
    QVERIFY(
        signToolLog.open(QIODevice::ReadOnly));
    const QByteArray signToolArguments =
        signToolLog.readAll();
    QCOMPARE(
        signToolArguments.count(
            QByteArrayLiteral("\n")),
        5);
    QVERIFY(
        signToolArguments.contains(
            "sign /sha1"));
    QVERIFY(
        signToolArguments.contains(
            "verify /pa /all /v"));
    QVERIFY(
        signToolArguments.contains(
            "CodexCompanion.exe"));
    QVERIFY(
        signToolArguments.contains(
            "CodexCompanionUpdater.exe"));
    QVERIFY(
        signToolArguments.contains(
            "Codex-Companion-0.3.4-42-windows-x64.exe"));
}

void InstallerContractTests::
    compiledInstallerResourcesMatchContract()
{
    const QString installerPath =
        qEnvironmentVariable(
            "COMPANION_TEST_INSTALLER");
    if (installerPath.isEmpty()) {
        QSKIP(
            "COMPANION_TEST_INSTALLER is not set; "
            "source-contract coverage remains active.");
    }

    const QString version =
        qEnvironmentVariable(
            "COMPANION_TEST_VERSION");
    const QString buildText =
        qEnvironmentVariable(
            "COMPANION_TEST_BUILD");
    QVERIFY(!version.isEmpty());
    QVERIFY(!buildText.isEmpty());

    const QRegularExpression coreVersion(
        QStringLiteral(
            "^(\\d+)\\.(\\d+)\\.(\\d+)"));
    const QRegularExpressionMatch match =
        coreVersion.match(version);
    QVERIFY(match.hasMatch());
    bool converted = false;
    const quint16 major =
        match.captured(1).toUShort(&converted);
    QVERIFY(converted);
    const quint16 minor =
        match.captured(2).toUShort(&converted);
    QVERIFY(converted);
    const quint16 patch =
        match.captured(3).toUShort(&converted);
    QVERIFY(converted);
    const quint16 build =
        buildText.toUShort(&converted);
    QVERIFY(converted);

    QFile installer(installerPath);
    QVERIFY2(
        installer.open(QIODevice::ReadOnly),
        qPrintable(installer.errorString()));
    const QByteArray dosHeader =
        installer.read(64);
    QVERIFY(dosHeader.size() >= 64);
    QCOMPARE(
        dosHeader.first(2),
        QByteArray("MZ", 2));
    const quint32 peOffset =
        qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(
                dosHeader.constData() + 0x3c));
    QVERIFY(installer.seek(qint64(peOffset)));
    const QByteArray peHeader =
        installer.read(6);
    QCOMPARE(
        peHeader.first(4),
        QByteArray("PE\0\0", 4));
    QCOMPARE(
        qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar*>(
                peHeader.constData() + 4)),
        quint16(IMAGE_FILE_MACHINE_I386));
    installer.close();

    const std::wstring nativePath =
        QDir::toNativeSeparators(
            QFileInfo(installerPath)
                .absoluteFilePath())
            .toStdWString();
    DWORD ignoredHandle = 0;
    const DWORD versionBytes =
        GetFileVersionInfoSizeW(
            nativePath.c_str(),
            &ignoredHandle);
    QVERIFY(versionBytes > 0);
    std::vector<BYTE> versionInfo(
        versionBytes);
    QVERIFY(
        GetFileVersionInfoW(
            nativePath.c_str(),
            0,
            versionBytes,
            versionInfo.data()));

    void* rawFixedInfo = nullptr;
    UINT fixedInfoBytes = 0;
    QVERIFY(
        VerQueryValueW(
            versionInfo.data(),
            L"\\",
            &rawFixedInfo,
            &fixedInfoBytes));
    QVERIFY(
        fixedInfoBytes
        >= sizeof(VS_FIXEDFILEINFO));
    const auto* fixedInfo =
        static_cast<const VS_FIXEDFILEINFO*>(
            rawFixedInfo);
    QCOMPARE(
        quint16(HIWORD(
            fixedInfo->dwFileVersionMS)),
        major);
    QCOMPARE(
        quint16(LOWORD(
            fixedInfo->dwFileVersionMS)),
        minor);
    QCOMPARE(
        quint16(HIWORD(
            fixedInfo->dwFileVersionLS)),
        patch);
    QCOMPARE(
        quint16(LOWORD(
            fixedInfo->dwFileVersionLS)),
        build);

    QCOMPARE(
        versionStringValue(
            versionInfo,
            QStringLiteral("ProductName")),
        QStringLiteral("Codex Companion"));
    QCOMPARE(
        versionStringValue(
            versionInfo,
            QStringLiteral("OriginalFilename")),
        QStringLiteral(
            "Codex-Companion-%1-%2-windows-x64.exe")
            .arg(version, buildText));
    QCOMPARE(
        versionStringValue(
            versionInfo,
            QStringLiteral("ProductVersion")),
        QStringLiteral(
            "cc-update/1|%1|%2|w|x64|10.0.22000")
            .arg(version, buildText));

    const HMODULE module =
        LoadLibraryExW(
            nativePath.c_str(),
            nullptr,
            LOAD_LIBRARY_AS_DATAFILE
                | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    QVERIFY(module != nullptr);
    int iconGroupCount = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated =
        EnumResourceNamesW(
            module,
            RT_GROUP_ICON,
            countIconGroups,
            reinterpret_cast<LONG_PTR>(
                &iconGroupCount));
    const DWORD resourceError =
        GetLastError();
    const BOOL freed =
        FreeLibrary(module);
    QVERIFY2(
        enumerated,
        qPrintable(
            QStringLiteral(
                "EnumResourceNamesW failed: %1")
                .arg(resourceError)));
    QVERIFY(iconGroupCount > 0);
    QVERIFY(freed);
}

} // namespace

QTEST_GUILESS_MAIN(InstallerContractTests)

#include "InstallerContractTests.moc"
