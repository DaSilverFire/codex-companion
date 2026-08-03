#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString readUtf8(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void requireTokens(
    const QString& source,
    const QStringList& tokens)
{
    for (const QString& token : tokens) {
        QVERIFY2(
            source.contains(
                token,
                Qt::CaseInsensitive),
            qPrintable(token));
    }
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

void writeUtf8(
    const QString& path,
    QByteArrayView contents)
{
    QVERIFY2(
        QDir().mkpath(
            QFileInfo(path)
                .absolutePath()),
        qPrintable(path));
    QFile file(path);
    QVERIFY2(
        file.open(
            QIODevice::WriteOnly
            | QIODevice::Truncate),
        qPrintable(file.errorString()));
    QCOMPARE(
        file.write(
            contents.data(),
            contents.size()),
        contents.size());
}

struct ProcessResult final {
    bool finished = false;
    int exitCode = -1;
    QByteArray standardOutput;
    QByteArray standardError;
};

ProcessResult runPowerShell(
    const QString& scriptPath,
    const QStringList& arguments,
    QProcessEnvironment environment =
        QProcessEnvironment::
            systemEnvironment())
{
    const QString powershell =
        QStandardPaths::findExecutable(
            QStringLiteral(
                "powershell.exe"));
    if (powershell.isEmpty()) {
        return {};
    }

    QProcess process;
    process.setProcessEnvironment(
        std::move(environment));
    process.setProgram(powershell);
    QStringList processArguments{
        QStringLiteral("-NoProfile"),
        QStringLiteral("-NonInteractive"),
        QStringLiteral(
            "-ExecutionPolicy"),
        QStringLiteral("Bypass"),
        QStringLiteral("-File"),
        scriptPath,
    };
    processArguments.append(arguments);
    process.setArguments(
        processArguments);
    process.start();

    ProcessResult result;
    result.finished =
        process.waitForFinished(60'000);
    if (!result.finished) {
        process.kill();
        process.waitForFinished();
    }
    result.exitCode =
        process.exitCode();
    result.standardOutput =
        process.readAllStandardOutput();
    result.standardError =
        process.readAllStandardError();
    return result;
}

struct FallbackFixture final {
    QString sourceRoot;
    QString outputDir;
    QString publishScript;
    QString gitScript;
};

FallbackFixture createFallbackFixture(
    QTemporaryDir& directory)
{
    const QString sourceRoot =
        directory.filePath(
            QStringLiteral("source"));
    const QString outputDir =
        QDir(sourceRoot).filePath(
            QStringLiteral(
                "work/release/fallback"));
    const QString publishScript =
        QDir(sourceRoot).filePath(
            QStringLiteral(
                "scripts/fake-publish.ps1"));
    const QString gitScript =
        QDir(sourceRoot).filePath(
            QStringLiteral(
                "tools/fake-git.ps1"));

    writeUtf8(
        QDir(sourceRoot).filePath(
            QStringLiteral(
                "windows/src/"
                "CodexCompanion.App/"
                "CodexCompanion.App.csproj")),
        QByteArrayLiteral(
            "<Project>"
            "<PropertyGroup>"
            "<Version>0.3.3</Version>"
            "</PropertyGroup>"
            "</Project>\n"));
    writeUtf8(
        publishScript,
        QByteArrayLiteral(
            "param(\n"
            "    [string]$Version,\n"
            "    [string]$OutputRoot\n"
            ")\n"
            "$ErrorActionPreference = 'Stop'\n"
            "foreach ($runtime in @('win-x64', 'win-arm64')) {\n"
            "    $directory = Join-Path $OutputRoot $runtime\n"
            "    New-Item -ItemType Directory -Path $directory "
            "-Force | Out-Null\n"
            "    [IO.File]::WriteAllText(\n"
            "        (Join-Path $directory 'CodexCompanion.exe'),\n"
            "        \"app-$runtime-$Version\")\n"
            "    [IO.File]::WriteAllText(\n"
            "        (Join-Path $directory 'coreclr.dll'),\n"
            "        \"coreclr-$runtime\")\n"
            "    if ($env:FAKE_PUBLISH_OMIT_ARM64_HOSTFXR -ne '1' "
            "-or $runtime -ne 'win-arm64') {\n"
            "        [IO.File]::WriteAllText(\n"
            "            (Join-Path $directory 'hostfxr.dll'),\n"
            "            \"hostfxr-$runtime\")\n"
            "    }\n"
            "}\n"));
    writeUtf8(
        gitScript,
        QByteArrayLiteral(
            "param(\n"
            "    [string]$C,\n"
            "    [Parameter(ValueFromRemainingArguments = $true)]\n"
            "    [string[]]$GitArguments\n"
            ")\n"
            "if ($GitArguments[0] -eq 'status') {\n"
            "    if ($env:FAKE_GIT_DIRTY -eq '1') {\n"
            "        Write-Output ' M windows/dirty.txt'\n"
            "    }\n"
            "    exit 0\n"
            "}\n"
            "if ($GitArguments[0] -eq 'rev-parse' "
            "-and $GitArguments[1] -eq 'HEAD') {\n"
            "    Write-Output ('a' * 40)\n"
            "    exit 0\n"
            "}\n"
            "if ($GitArguments[0] -eq 'rev-parse' "
            "-and $GitArguments[1] -eq 'HEAD:windows') {\n"
            "    Write-Output ('b' * 40)\n"
            "    exit 0\n"
            "}\n"
            "exit 1\n"));
    writeUtf8(
        QDir(outputDir).filePath(
            QStringLiteral("verified-sentinel.txt")),
        QByteArrayLiteral(
            "previous verified fallback\n"));

    return {
        sourceRoot,
        outputDir,
        publishScript,
        gitScript,
    };
}

QStringList fallbackArguments(
    const FallbackFixture& fixture)
{
    return {
        QStringLiteral("-SourceRoot"),
        fixture.sourceRoot,
        QStringLiteral("-OutputDir"),
        fixture.outputDir,
        QStringLiteral(
            "-PublishScriptPath"),
        fixture.publishScript,
        QStringLiteral("-GitPath"),
        fixture.gitScript,
    };
}

class CutoverAutomationTests final
    : public QObject {
    Q_OBJECT

private slots:
    void cleanVmProvisioningIsBoundedAndFailClosed();
    void cleanVmRunnerCoversReleaseLifecycle();
    void guestVerifierChecksWindowsLifecycle();
    void cleanVmSettingsReuseAndIconIdentity();
    void cleanVmUpdateContractCarriesTheTlsFeedIntoCompanion();
    void cleanVmUpdateCompletionRequiresExactTransactionAndRelaunchIdentity();
    void cleanVmUpdatePreservesSeededStateAndRedactsLogs();
    void cleanVmPreviousInstallerMustBeExactSignedPredecessor();
    void cleanVmRollbackHarnessMustBeAuthenticatedAndResultValidated();
    void portableDpiMatrixCoversPackagedSurfacesAndRestoration();
    void petDragReversalVerifierCoversRenderedReversalsAndRestoration();
    void nativeQtRebuildPinsVerifiedNinja();
    void taskSnapshotLoaderDisablesIncrementalLinking();
    void nativeQtRebuildTreatsAlreadyStoppedCompanionAsSuccess();
    void dotnetFallbackUsesExistingPublisherAndRecordsEvidence();
    void dotnetFallbackRejectsDirtySourceBeforePublishing();
    void dotnetFallbackPreservesVerifiedOutputWhenReplacementFails();
    void dotnetFallbackPromotesValidatedOutputAndArchivesPrevious();
};

void CutoverAutomationTests::
    nativeQtRebuildPinsVerifiedNinja()
{
    const QString nativeCommand =
        readUtf8(
            QStringLiteral(
                COMPANION_NATIVE_COMMAND_SCRIPT_PATH));
    const QString rebuildCommand =
        readUtf8(
            QStringLiteral(
                COMPANION_CURRENT_CANDIDATE_REBUILD_SCRIPT_PATH));
    QVERIFY(!nativeCommand.isEmpty());
    QVERIFY(!rebuildCommand.isEmpty());

    requireTokens(
        nativeCommand,
        {
            QStringLiteral("COMPANION_NINJA_EXE"),
            QStringLiteral("%VSINSTALLDIR%"),
            QStringLiteral(
                "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\Ninja\\ninja.exe"),
            QStringLiteral(
                "\"%COMPANION_NINJA_EXE%\" --version"),
            QStringLiteral(
                "if /I \"%~2\"==\"--preset\" goto run_cmake_configure"),
            QStringLiteral(
                ":run_cmake_configure"),
            QStringLiteral(
                "%* \"-DCMAKE_MAKE_PROGRAM=%COMPANION_NINJA_EXE%\""),
        });
    requireTokens(
        rebuildCommand,
        {
            QStringLiteral("COMPANION_NINJA_EXE"),
            QStringLiteral("CMAKE_MAKE_PROGRAM"),
            QStringLiteral("windows-msvc-release"),
        });
    QVERIFY(
        !nativeCommand.contains(
            QStringLiteral(
                "WinGet\\Links\\ninja.exe"),
            Qt::CaseInsensitive));
    QVERIFY(
        !rebuildCommand.contains(
            QStringLiteral(
                "WinGet\\Links\\ninja.exe"),
            Qt::CaseInsensitive));
}

void CutoverAutomationTests::
    taskSnapshotLoaderDisablesIncrementalLinking()
{
    const QString source =
        readUtf8(
            QStringLiteral(
                COMPANION_CODEX_TEST_CMAKE_PATH));
    QVERIFY(!source.isEmpty());

    const QString target =
        QStringLiteral(
            "companion_codex_task_snapshot_loader");
    const qsizetype targetStart =
        source.indexOf(
            QStringLiteral(
                "qt_add_executable(")
            + target);
    QVERIFY(targetStart >= 0);

    const qsizetype nextTarget =
        source.indexOf(
            QStringLiteral(
                "qt_add_executable"),
            targetStart + target.size());
    const QString targetBlock =
        nextTarget < 0
        ? source.mid(targetStart)
        : source.mid(
              targetStart,
              nextTarget - targetStart);
    requireTokens(
        targetBlock,
        {
            QStringLiteral(
                "target_link_options("),
            target,
            QStringLiteral(
                "/INCREMENTAL:NO"),
        });
}

void CutoverAutomationTests::
    nativeQtRebuildTreatsAlreadyStoppedCompanionAsSuccess()
{
    const QString rebuildCommand =
        readUtf8(
            QStringLiteral(
                COMPANION_CURRENT_CANDIDATE_REBUILD_SCRIPT_PATH));
    QVERIFY(!rebuildCommand.isEmpty());

    requireTokens(
        rebuildCommand,
        {
            QStringLiteral(
                "call :stopCompanionForReplacement"),
            QStringLiteral(
                ":stopCompanionForReplacement"),
            QStringLiteral(
                "tasklist /FI \"IMAGENAME eq CodexCompanion.exe\""),
            QStringLiteral(
                "Codex Companion was not running; continuing replacement."),
            QStringLiteral(
                "exit /b 0"),
        });
}

void CutoverAutomationTests::
    cleanVmProvisioningIsBoundedAndFailClosed()
{
    const QString source =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_CREATE_SCRIPT_PATH));
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral("SupportsShouldProcess"),
            QStringLiteral("PSCredential"),
            QStringLiteral("New-VHD"),
            QStringLiteral("New-VM"),
            QStringLiteral("Set-VMFirmware"),
            QStringLiteral("Enable-VMIntegrationService"),
            QStringLiteral("Disconnect-VMNetworkAdapter"),
            QStringLiteral("Checkpoint-VM"),
            QStringLiteral("CompanionCleanBaseline"),
            QStringLiteral("GetFullPath"),
            QStringLiteral("StartsWith"),
            QStringLiteral("IsInRole"),
            QStringLiteral("standard user"),
            QStringLiteral("Windows 11"),
        });
    QVERIFY(
        !source.contains(
            QStringLiteral("ConvertTo-SecureString")
            + QStringLiteral(" -AsPlainText"),
            Qt::CaseInsensitive));
    QVERIFY(
        !source.contains(
            QStringLiteral("AutoAdminLogon")
            + QStringLiteral("=1"),
            Qt::CaseInsensitive));
}

void CutoverAutomationTests::
    cleanVmRunnerCoversReleaseLifecycle()
{
    const QString source =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH));
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral("Restore-VMSnapshot"),
            QStringLiteral("Disconnect-VMNetworkAdapter"),
            QStringLiteral("Connect-VMNetworkAdapter"),
            QStringLiteral("New-PSSession"),
            QStringLiteral("-VMName"),
            QStringLiteral("-ToSession"),
            QStringLiteral("verify-installed.ps1"),
            QStringLiteral("/VERYSILENT"),
            QStringLiteral("/SUPPRESSMSGBOXES"),
            QStringLiteral("/NORESTART"),
            QStringLiteral("RunOnce"),
            QStringLiteral("DpiPercent"),
            QStringLiteral("100"),
            QStringLiteral("125"),
            QStringLiteral("150"),
            QStringLiteral("200"),
            QStringLiteral("PreviousInstallerPath"),
            QStringLiteral("ManifestPath"),
            QStringLiteral("https"),
            QStringLiteral("uninstall"),
            QStringLiteral("rollback"),
            QStringLiteral("clean-vm-summary.json"),
        });
    QVERIFY(
        !source.contains(
            QStringLiteral("GuestCredential.Password"),
            Qt::CaseInsensitive));
    QVERIFY(
        !source.contains(
            QStringLiteral("ConvertFrom-SecureString"),
            Qt::CaseInsensitive));
}

void CutoverAutomationTests::
    guestVerifierChecksWindowsLifecycle()
{
    const QString source =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral("Shell_NotifyIconGetRect"),
            QStringLiteral("9B3C42CB"),
            QStringLiteral("DwmGetWindowAttribute"),
            QStringLiteral("DWMWA_CLOAKED"),
            QStringLiteral("GetAncestor"),
            QStringLiteral("GetLastActivePopup"),
            QStringLiteral("WS_EX_TOOLWINDOW"),
            QStringLiteral("WS_EX_NOACTIVATE"),
            QStringLiteral("GW_OWNER"),
            QStringLiteral("WM_CLOSE"),
            QStringLiteral("Codex Companion Settings"),
            QStringLiteral("Get-AuthenticodeSignature"),
            QStringLiteral("WScript.Shell"),
            QStringLiteral("Start Menu"),
            QStringLiteral("CopyFromScreen"),
            QStringLiteral("Installed"),
            QStringLiteral("Uninstalled"),
            QStringLiteral("sourceSha256"),
            QStringLiteral("taskbarCandidate"),
            QStringLiteral("altTabCandidate"),
            QStringLiteral("altTabRepresentative"),
        });
    QVERIFY(
        !source.contains(
            QStringLiteral(
                "$taskbarCandidate = $altTabCandidate")));
}

void CutoverAutomationTests::
    cleanVmSettingsReuseAndIconIdentity()
{
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    requireTokens(
        verifySource,
        {
            QStringLiteral(
                "Test-CompanionShortcutIdentity"),
            QStringLiteral(
                "runtime.second_launch_reuses_settings_hwnd"),
            QStringLiteral(
                "installed.start_menu_icon"),
            QStringLiteral(
                "installed.executable_icon"),
            QStringLiteral(
                "ExtractAssociatedIcon"),
            QStringLiteral(
                "executable-icon.png"),
        });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-shortcut-identity-contract.ps1"));
    writeUtf8(
        probe,
        QByteArray(
            R"ps1(param(
    [string]$ModulePath,
    [string]$FixtureRoot
)
$ErrorActionPreference = 'Stop'
Import-Module -Name $ModulePath -Force

$executable = Join-Path $FixtureRoot 'Codex Companion\bin\CodexCompanion.exe'
$workingDirectory = Split-Path -Parent $executable
$commaExecutable = Join-Path $FixtureRoot 'Codex, Companion\bin\CodexCompanion.exe'
$commaWorkingDirectory = Split-Path -Parent $commaExecutable

[ordered]@{
    exactAccepted = Test-CompanionShortcutIdentity `
        -TargetPath $executable `
        -IconLocation "$executable,0" `
        -WorkingDirectory $workingDirectory `
        -ExecutablePath $executable
    quotedAccepted = Test-CompanionShortcutIdentity `
        -TargetPath $executable `
        -IconLocation "`"$executable`",0" `
        -WorkingDirectory $workingDirectory `
        -ExecutablePath $executable
    commaPathAccepted = Test-CompanionShortcutIdentity `
        -TargetPath $commaExecutable `
        -IconLocation "`"$commaExecutable`",0" `
        -WorkingDirectory $commaWorkingDirectory `
        -ExecutablePath $commaExecutable
    wrongTargetRejected = -not (
        Test-CompanionShortcutIdentity `
            -TargetPath (Join-Path $FixtureRoot 'wrong.exe') `
            -IconLocation "$executable,0" `
            -WorkingDirectory $workingDirectory `
            -ExecutablePath $executable
    )
    wrongIconRejected = -not (
        Test-CompanionShortcutIdentity `
            -TargetPath $executable `
            -IconLocation (
                (Join-Path $FixtureRoot 'wrong.exe') + ',0'
            ) `
            -WorkingDirectory $workingDirectory `
            -ExecutablePath $executable
    )
    wrongIndexRejected = -not (
        Test-CompanionShortcutIdentity `
            -TargetPath $executable `
            -IconLocation "$executable,1" `
            -WorkingDirectory $workingDirectory `
            -ExecutablePath $executable
    )
    wrongWorkingDirectoryRejected = -not (
        Test-CompanionShortcutIdentity `
            -TargetPath $executable `
            -IconLocation "$executable,0" `
            -WorkingDirectory $FixtureRoot `
            -ExecutablePath $executable
    )
    emptyIconRejected = -not (
        Test-CompanionShortcutIdentity `
            -TargetPath $executable `
            -IconLocation '' `
            -WorkingDirectory $workingDirectory `
            -ExecutablePath $executable
    )
} | ConvertTo-Json -Compress
)ps1"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
                QStringLiteral("-FixtureRoot"),
                directory.path(),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    for (const QString& field : {
             QStringLiteral("exactAccepted"),
             QStringLiteral("quotedAccepted"),
             QStringLiteral(
                 "commaPathAccepted"),
             QStringLiteral(
                 "wrongTargetRejected"),
             QStringLiteral(
                 "wrongIconRejected"),
             QStringLiteral(
                 "wrongIndexRejected"),
             QStringLiteral(
                 "wrongWorkingDirectoryRejected"),
             QStringLiteral(
                 "emptyIconRejected"),
         }) {
        QCOMPARE(
            object.value(field).toBool(),
            true);
    }
}

void CutoverAutomationTests::
    cleanVmUpdateContractCarriesTheTlsFeedIntoCompanion()
{
    const QString invokeSource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH));
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    QVERIFY(
        invokeSource.contains(
            QStringLiteral(
                "Set-CompanionUpdateFeedConfiguration")));
    QVERIFY(
        verifySource.contains(
            QStringLiteral(
                "Get-CompanionLaunchArguments")));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-clean-vm-contract.ps1"));
    writeUtf8(
        probe,
        QByteArrayLiteral(
            "param([string]$ModulePath)\n"
            "Import-Module -Name $ModulePath -Force\n"
            "$configuration = [ordered]@{ mode = 'Update' }\n"
            "$configuration = "
            "Set-CompanionUpdateFeedConfiguration "
            "-Configuration $configuration "
            "-DriveUpdateUi $true "
            "-UpdateFeedUrl "
            "'https://fixture.example.test/update-windows-x64.json'\n"
            "$launchArguments = @(\n"
            "    Get-CompanionLaunchArguments "
            "-Mode 'Update' "
            "-UpdateFeedUrl $configuration.updateFeedUrl\n"
            ")\n"
            "$httpRejected = $false\n"
            "try {\n"
            "    [void](Set-CompanionUpdateFeedConfiguration "
            "-Configuration ([ordered]@{}) "
            "-DriveUpdateUi $true "
            "-UpdateFeedUrl 'http://fixture.example.test/update.json')\n"
            "} catch {\n"
            "    $httpRejected = $true\n"
            "}\n"
            "[ordered]@{\n"
            "    updateFeedUrl = $configuration.updateFeedUrl\n"
            "    launchArguments = $launchArguments\n"
            "    httpRejected = $httpRejected\n"
            "} | ConvertTo-Json -Compress\n"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    QCOMPARE(
        object.value(
            QStringLiteral(
                "updateFeedUrl"))
            .toString(),
        QStringLiteral(
            "https://fixture.example.test/"
            "update-windows-x64.json"));
    const QJsonArray expectedLaunchArguments{
        QStringLiteral(
            "--update-manifest-url"),
        QStringLiteral(
            "https://fixture.example.test/"
            "update-windows-x64.json"),
    };
    QCOMPARE(
        object.value(
            QStringLiteral(
                "launchArguments"))
            .toArray(),
        expectedLaunchArguments);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "httpRejected"))
            .toBool(),
        true);
}

void CutoverAutomationTests::
    cleanVmUpdateCompletionRequiresExactTransactionAndRelaunchIdentity()
{
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    requireTokens(
        verifySource,
        {
            QStringLiteral(
                "Find-CompanionUpdateTransaction"),
            QStringLiteral(
                "Test-CompanionUpdateResult"),
            QStringLiteral(
                "Test-CompanionVersionIdentity"),
            QStringLiteral(
                "update.helper_exited"),
            QStringLiteral(
                "update.rollback_removed"),
            QStringLiteral(
                "update.relaunched_process"),
            QStringLiteral(
                "update.tray_recovered"),
        });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-update-completion-contract.ps1"));
    writeUtf8(
        probe,
        QByteArray(
            R"ps1(param(
    [string]$ModulePath,
    [string]$FixtureRoot
)
$ErrorActionPreference = 'Stop'
Import-Module -Name $ModulePath -Force

$updaterRoot = Join-Path $FixtureRoot 'CodexCompanionUpdater'
$oldRoot = Join-Path $updaterRoot '00000000-0000-0000-0000-000000000000'
New-Item -ItemType Directory -Path $oldRoot -Force | Out-Null
$before = @(
    Get-CompanionUpdaterTransactionRoots -UpdaterRoot $updaterRoot
)

$requestId = '11111111-1111-1111-1111-111111111111'
$transactionRoot = Join-Path $updaterRoot $requestId
$installRoot = Join-Path $FixtureRoot 'Codex Companion'
New-Item -ItemType Directory -Path $transactionRoot -Force | Out-Null
$request = [ordered]@{
    requestId = $requestId
    installerPath = (Join-Path $FixtureRoot 'Codex-Companion.exe')
    expectedSha256 = ('a' * 64)
    expectedSize = 123
    expectedVersion = '0.3.4'
    expectedBuild = 7
    installRoot = $installRoot
    rollbackRoot = "$installRoot.rollback.$requestId"
    uninstallRegistryKey = 'HKCU\Software\Fixture'
    startMenuShortcut = (Join-Path $FixtureRoot 'Codex Companion.lnk')
    acknowledgementEvent = "Local\CodexCompanion.UpdateAck.$requestId"
    parentProcessId = 42
}
$request |
    ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $transactionRoot 'request.json')

$transaction = Find-CompanionUpdateTransaction `
    -UpdaterRoot $updaterRoot `
    -ExcludedRoots $before `
    -ExpectedVersion '0.3.4' `
    -ExpectedBuild 7 `
    -InstallRoot $installRoot
$wrongBuildRejected = $null -eq (
    Find-CompanionUpdateTransaction `
        -UpdaterRoot $updaterRoot `
        -ExcludedRoots $before `
        -ExpectedVersion '0.3.4' `
        -ExpectedBuild 8 `
        -InstallRoot $installRoot
)

$result = [ordered]@{
    schema = 1
    requestId = $requestId
    success = $true
    errorCode = ''
    message = 'The update installed successfully.'
    completedAtUtc = '2026-07-26T12:00:00.000Z'
    installerLogPath = (Join-Path $transactionRoot 'installer.log')
    context = [ordered]@{}
}
$validResult = Test-CompanionUpdateResult `
    -Result $result `
    -RequestId $requestId `
    -TransactionRoot $transactionRoot
$result.requestId = '22222222-2222-2222-2222-222222222222'
$wrongResultRejected = -not (
    Test-CompanionUpdateResult `
        -Result $result `
        -RequestId $requestId `
        -TransactionRoot $transactionRoot
)

[ordered]@{
    matchedRequestId = [string]$transaction.request.requestId
    matchedRoot = [string]$transaction.root
    wrongBuildRejected = $wrongBuildRejected
    validResult = $validResult
    wrongResultRejected = $wrongResultRejected
    exactVersion = Test-CompanionVersionIdentity `
        -ProductVersion '0.3.4.7' `
        -FileVersion '0.3.4.7' `
        -ExpectedVersion '0.3.4' `
        -ExpectedBuild 7
    staleBuildRejected = -not (
        Test-CompanionVersionIdentity `
            -ProductVersion '0.3.4.6' `
            -FileVersion '0.3.4.6' `
            -ExpectedVersion '0.3.4' `
            -ExpectedBuild 7
    )
} | ConvertTo-Json -Compress
)ps1"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
                QStringLiteral("-FixtureRoot"),
                directory.path(),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    QCOMPARE(
        object.value(
            QStringLiteral(
                "matchedRequestId"))
            .toString(),
        QStringLiteral(
            "11111111-1111-1111-1111-111111111111"));
    QCOMPARE(
        QDir::cleanPath(
            object.value(
                QStringLiteral(
                    "matchedRoot"))
                .toString()),
        QDir::cleanPath(
            QDir(directory.path())
                .filePath(
                    QStringLiteral(
                        "CodexCompanionUpdater/"
                        "11111111-1111-1111-1111-111111111111"))));
    for (const QString& field : {
             QStringLiteral(
                 "wrongBuildRejected"),
             QStringLiteral(
                 "validResult"),
             QStringLiteral(
                 "wrongResultRejected"),
             QStringLiteral(
                 "exactVersion"),
             QStringLiteral(
                 "staleBuildRejected"),
         }) {
        QCOMPARE(
            object.value(field).toBool(),
            true);
    }
}

void CutoverAutomationTests::
    cleanVmUpdatePreservesSeededStateAndRedactsLogs()
{
    const QString invokeSource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH));
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    requireTokens(
        invokeSource,
        {
            QStringLiteral(
                "SeedDurableState"),
            QStringLiteral(
                "BaselineUserDataPath"),
            QStringLiteral(
                "update-before"),
        });
    requireTokens(
        verifySource,
        {
            QStringLiteral(
                "Get-CompanionDurableStateSnapshot"),
            QStringLiteral(
                "Test-CompanionDurableStatePreserved"),
            QStringLiteral(
                "Get-CompanionSensitiveLogEvidence"),
            QStringLiteral(
                "update.sensitive_log_clean"),
            QStringLiteral(
                "PersistenceProbe"),
        });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-update-persistence-contract.ps1"));
    writeUtf8(
        probe,
        QByteArray(
            R"ps1(param(
    [string]$ModulePath,
    [string]$FixtureRoot
)
$ErrorActionPreference = 'Stop'
Import-Module -Name $ModulePath -Force

$settingsPath = Join-Path $FixtureRoot 'CodexCompanion.ini'
$credentialsRoot = Join-Path $FixtureRoot 'Credentials'
$pairingsPath = Join-Path $FixtureRoot 'Security\paired-devices.v1.json'
$goalsPath = Join-Path $FixtureRoot 'goals_1.sqlite'
$privacyRoot = Join-Path $FixtureRoot 'PrivacyCanaries'
New-Item -ItemType Directory -Path $credentialsRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path $pairingsPath) -Force |
    Out-Null
New-Item -ItemType Directory -Path $privacyRoot -Force | Out-Null
[IO.File]::WriteAllText($settingsPath, "[pet]`nselectedId=fixture-preserved-pet`n")
[IO.File]::WriteAllBytes(
    (Join-Path $credentialsRoot 'companion.openai-api-key.bin'),
    [byte[]](1, 2, 3, 4)
)
[IO.File]::WriteAllText($pairingsPath, '{"version":1,"records":[]}')
[IO.File]::WriteAllBytes($goalsPath, [byte[]](5, 6, 7, 8))
[IO.File]::WriteAllText(
    (Join-Path $privacyRoot 'prompt.txt'),
    'prompt-canary'
)

$roots = [ordered]@{
    settings = $settingsPath
    credentials = $credentialsRoot
    pairings = $pairingsPath
    goals = $goalsPath
    privacy = $privacyRoot
}
$baseline = @(
    Get-CompanionDurableStateSnapshot -Roots $roots
)
$current = @(
    Get-CompanionDurableStateSnapshot -Roots $roots
)
$preserved = Test-CompanionDurableStatePreserved `
    -Baseline $baseline `
    -Current $current `
    -RequiredRoots @('settings', 'credentials', 'pairings', 'goals')

[IO.File]::AppendAllText($settingsPath, "visible=false`n")
$mutated = @(
    Get-CompanionDurableStateSnapshot -Roots $roots
)
$mutationRejected = -not (
    Test-CompanionDurableStatePreserved `
        -Baseline $baseline `
        -Current $mutated `
        -RequiredRoots @('settings', 'credentials', 'pairings', 'goals')
)

$markers = [ordered]@{
    prompt = 'PROMPT_CANARY_2F481F9B'
    credential = 'CREDENTIAL_CANARY_4D721CAB'
    attachment = 'ATTACHMENT_CANARY_8B793E62'
    privateKey = 'PRIVATE_KEY_CANARY_C7A453D1'
}
$cleanLog = Join-Path $FixtureRoot 'installer-clean.log'
$taintedLog = Join-Path $FixtureRoot 'installer-tainted.log'
[IO.File]::WriteAllText(
    $cleanLog,
    ("installer completed successfully`n" * 131072)
)
$cleanEvidence = Get-CompanionSensitiveLogEvidence `
    -Paths @($cleanLog) `
    -Markers $markers
$taintedBytes = New-Object System.Collections.Generic.List[byte]
$taintedBytes.AddRange(
    [Text.Encoding]::UTF8.GetBytes(
        "prefix $($markers.credential) suffix`n"
    )
)
$taintedBytes.AddRange(
    [Text.Encoding]::Unicode.GetBytes(
        "prefix $($markers.privateKey) suffix"
    )
)
[IO.File]::WriteAllBytes($taintedLog, $taintedBytes.ToArray())
$taintedEvidence = Get-CompanionSensitiveLogEvidence `
    -Paths @($taintedLog) `
    -Markers $markers
$serializedEvidence = @(
    $cleanEvidence
    $taintedEvidence
) | ConvertTo-Json -Depth 6 -Compress
$evidenceRedacted = $true
foreach ($marker in $markers.Values) {
    if ($serializedEvidence.Contains([string]$marker)) {
        $evidenceRedacted = $false
    }
}

[ordered]@{
    roots = @($baseline.root | Sort-Object -Unique)
    preserved = $preserved
    mutationRejected = $mutationRejected
    cleanLog = [bool]$cleanEvidence.clean
    taintedLogRejected = -not [bool]$taintedEvidence.clean
    matchedCategories = @($taintedEvidence.matchedCategories)
    evidenceRedacted = $evidenceRedacted
} | ConvertTo-Json -Depth 6 -Compress
)ps1"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
                QStringLiteral("-FixtureRoot"),
                directory.path(),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    const QJsonArray expectedRoots{
        QStringLiteral("credentials"),
        QStringLiteral("goals"),
        QStringLiteral("pairings"),
        QStringLiteral("privacy"),
        QStringLiteral("settings"),
    };
    QCOMPARE(
        object.value(
            QStringLiteral("roots"))
            .toArray(),
        expectedRoots);
    for (const QString& field : {
             QStringLiteral("preserved"),
             QStringLiteral(
                 "mutationRejected"),
             QStringLiteral("cleanLog"),
             QStringLiteral(
                 "taintedLogRejected"),
             QStringLiteral(
                 "evidenceRedacted"),
         }) {
        QCOMPARE(
            object.value(field).toBool(),
            true);
    }
    const QJsonArray expectedCategories{
        QStringLiteral("credential"),
        QStringLiteral("privateKey"),
    };
    QCOMPARE(
        object.value(
            QStringLiteral(
                "matchedCategories"))
            .toArray(),
        expectedCategories);

    auto verifierEnvironment =
        QProcessEnvironment::
            systemEnvironment();
    const QString verifierRoot =
        directory.filePath(
            QStringLiteral(
                "verifier-user"));
    verifierEnvironment.insert(
        QStringLiteral("LOCALAPPDATA"),
        QDir(verifierRoot).filePath(
            QStringLiteral(
                "LocalAppData")));
    verifierEnvironment.insert(
        QStringLiteral("APPDATA"),
        QDir(verifierRoot).filePath(
            QStringLiteral("AppData")));
    verifierEnvironment.insert(
        QStringLiteral("USERPROFILE"),
        QDir(verifierRoot).filePath(
            QStringLiteral(
                "UserProfile")));
    const QString verifierEvidence =
        QDir(verifierRoot).filePath(
            QStringLiteral("Evidence"));
    const auto verifierResult =
        runPowerShell(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH),
            {
                QStringLiteral(
                    "-EvidenceRoot"),
                verifierEvidence,
                QStringLiteral(
                    "-PersistenceProbe"),
            },
            verifierEnvironment);
    QVERIFY(verifierResult.finished);
    QVERIFY2(
        verifierResult.exitCode == 0,
        verifierResult.standardError.constData());
    const QJsonDocument verifierDocument =
        QJsonDocument::fromJson(
            verifierResult.standardOutput.trimmed());
    QVERIFY(verifierDocument.isObject());
    const QJsonObject verifierObject =
        verifierDocument.object();
    QCOMPARE(
        verifierObject.value(
            QStringLiteral("passed"))
            .toBool(),
        true);
    QCOMPARE(
        verifierObject.value(
            QStringLiteral("roots"))
            .toArray(),
        expectedRoots);
    const QJsonObject semantic =
        verifierObject.value(
            QStringLiteral("semantic"))
            .toObject();
    for (const QString& field : {
             QStringLiteral("settings"),
             QStringLiteral(
                 "petSelection"),
             QStringLiteral("credential"),
             QStringLiteral("pairings"),
             QStringLiteral("goals"),
             QStringLiteral(
                 "privacyCanaries"),
         }) {
        QCOMPARE(
            semantic.value(field).toBool(),
            true);
    }
}

void CutoverAutomationTests::
    cleanVmPreviousInstallerMustBeExactSignedPredecessor()
{
    const QString invokeSource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH));
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    requireTokens(
        invokeSource,
        {
            QStringLiteral(
                "ExpectedPreviousVersion"),
            QStringLiteral(
                "ExpectedPreviousBuild"),
            QStringLiteral(
                "ExpectedPreviousSha256"),
            QStringLiteral(
                "Test-CompanionInstallerIdentity"),
            QStringLiteral(
                "Test-CompanionReleasePredecessor"),
            QStringLiteral(
                "Get-CompanionPeMachine"),
            QStringLiteral(
                "Get-AuthenticodeSignature"),
        });
    requireTokens(
        verifySource,
        {
            QStringLiteral(
                "update.initial_version"),
        });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-previous-installer-contract.ps1"));
    writeUtf8(
        probe,
        QByteArray(
            R"ps1(param(
    [string]$ModulePath,
    [string]$FixtureRoot
)
Import-Module -Name $ModulePath -Force

$pePath = Join-Path $FixtureRoot 'fixture-x64.exe'
$bytes = New-Object byte[] 512
$bytes[0] = 0x4d
$bytes[1] = 0x5a
[BitConverter]::GetBytes([int]128).CopyTo($bytes, 0x3c)
$bytes[128] = 0x50
$bytes[129] = 0x45
$bytes[130] = 0
$bytes[131] = 0
[BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 132)
[IO.File]::WriteAllBytes($pePath, $bytes)

[ordered]@{
    exactIdentity = Test-CompanionInstallerIdentity `
        -ProductVersion 'cc-update/1|0.3.3|9|w|x64|10.0.22000' `
        -FileVersion '0.3.3.9' `
        -OriginalFilename 'Codex-Companion-0.3.3-9-windows-x64.exe' `
        -ProductName 'Codex Companion' `
        -ExpectedVersion '0.3.3' `
        -ExpectedBuild 9
    wrongMarkerRejected = -not (
        Test-CompanionInstallerIdentity `
            -ProductVersion '0.3.4.9' `
            -FileVersion '0.3.3.9' `
            -OriginalFilename 'Codex-Companion-0.3.3-9-windows-x64.exe' `
            -ProductName 'Codex Companion' `
            -ExpectedVersion '0.3.3' `
            -ExpectedBuild 9
    )
    predecessorAccepted = Test-CompanionReleasePredecessor `
        -PreviousVersion '0.3.3' `
        -PreviousBuild 9 `
        -CurrentVersion '0.3.4' `
        -CurrentBuild 1
    sameReleaseRejected = -not (
        Test-CompanionReleasePredecessor `
            -PreviousVersion '0.3.4' `
            -PreviousBuild 1 `
            -CurrentVersion '0.3.4' `
            -CurrentBuild 1
    )
    peMachine = Get-CompanionPeMachine -Path $pePath
} | ConvertTo-Json -Compress
)ps1"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
                QStringLiteral("-FixtureRoot"),
                directory.path(),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    for (const QString& field : {
             QStringLiteral(
                 "exactIdentity"),
             QStringLiteral(
                 "wrongMarkerRejected"),
             QStringLiteral(
                 "predecessorAccepted"),
             QStringLiteral(
                 "sameReleaseRejected"),
         }) {
        QCOMPARE(
            object.value(field).toBool(),
            true);
    }
    QCOMPARE(
        object.value(
            QStringLiteral(
                "peMachine"))
            .toInt(),
        0x8664);
}

void CutoverAutomationTests::
    cleanVmRollbackHarnessMustBeAuthenticatedAndResultValidated()
{
    const QString invokeSource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH));
    const QString verifySource =
        readUtf8(
            QStringLiteral(
                COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH));
    requireTokens(
        invokeSource,
        {
            QStringLiteral(
                "ExpectedRollbackHarnessSha256"),
            QStringLiteral(
                "Test-CompanionRollbackResult"),
            QStringLiteral(
                "rollback.installer-failure"),
            QStringLiteral(
                "rollback.acknowledgement-timeout"),
            QStringLiteral(
                "Wait-Job"),
            QStringLiteral(
                "Get-AuthenticodeSignature"),
        });
    requireTokens(
        verifySource,
        {
            QStringLiteral(
                "installed.no_rollback_directories"),
            QStringLiteral(
                "runtime.no_updater_helpers"),
        });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString probe =
        directory.filePath(
            QStringLiteral(
                "probe-rollback-result-contract.ps1"));
    writeUtf8(
        probe,
        QByteArray(
            R"ps1(param([string]$ModulePath)
Import-Module -Name $ModulePath -Force

$result = [ordered]@{
    schemaVersion = 1
    scenario = 'installer-failure'
    passed = $true
    requestId = '11111111-1111-1111-1111-111111111111'
    injectedFailureCode = 'update.installer_exit_failed'
    rollbackSucceeded = $true
    completedAtUtc = '2026-07-26T12:00:00.000Z'
    evidence = [ordered]@{
        restoredVersion = '0.3.3'
        restoredBuild = 9
        relaunchProcessId = 1234
        helperExited = $true
        rollbackRemoved = $true
        userDataPreserved = $true
    }
}
$valid = Test-CompanionRollbackResult `
    -Result $result `
    -Scenario 'installer-failure' `
    -ExpectedVersion '0.3.3' `
    -ExpectedBuild 9
$result.injectedFailureCode = 'update.acknowledgement_timeout'
$wrongFailureRejected = -not (
    Test-CompanionRollbackResult `
        -Result $result `
        -Scenario 'installer-failure' `
        -ExpectedVersion '0.3.3' `
        -ExpectedBuild 9
)

[ordered]@{
    valid = $valid
    wrongFailureRejected = $wrongFailureRejected
} | ConvertTo-Json -Compress
)ps1"));

    const auto result =
        runPowerShell(
            probe,
            {
                QStringLiteral("-ModulePath"),
                QStringLiteral(
                    COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH),
            });

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    QCOMPARE(
        document.object()
            .value(
                QStringLiteral(
                    "valid"))
            .toBool(),
        true);
    QCOMPARE(
        document.object()
            .value(
                QStringLiteral(
                    "wrongFailureRejected"))
            .toBool(),
        true);
}

void CutoverAutomationTests::
    portableDpiMatrixCoversPackagedSurfacesAndRestoration()
{
    const QString scriptPath =
        QStringLiteral(
            COMPANION_PORTABLE_DPI_MATRIX_SCRIPT_PATH);
    const QString source =
        readUtf8(scriptPath);
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral(
                "ValidateSet(100, 125, 150, 200)"),
            QStringLiteral(
                "QT_SCALE_FACTOR"),
            QStringLiteral(
                "QT_SCALE_FACTOR_ROUNDING_POLICY"),
            QStringLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"),
            QStringLiteral(
                "CODEX_COMPANION_TEST_STARTUP_ROUTE"),
            QStringLiteral(
                "LOCALAPPDATA"),
            QStringLiteral(
                "APPDATA"),
            QStringLiteral(
                "UIAutomationClient"),
            QStringLiteral(
                "PrintWindow"),
            QStringLiteral(
                "SetCursorPos"),
            QStringLiteral(
                "GetCursorPos"),
            QStringLiteral(
                "startupRoutes"),
            QStringLiteral(
                "GetWindowRect"),
            QStringLiteral(
                "processes-hover.png"),
            QStringLiteral(
                "taskbarCandidate"),
            QStringLiteral(
                "altTabCandidate"),
            QStringLiteral(
                "TemporarilyStopRunningInstance"),
            QStringLiteral(
                "Close Settings"),
            QStringLiteral(
                "productionSettingsPreserved"),
            QStringLiteral(
                "cursorRestored"),
            QStringLiteral(
                "GetFullPath"),
            QStringLiteral(
                "StartsWith"),
            QStringLiteral(
                "dpi-matrix-summary.json"),
        });

    const auto result =
        runPowerShell(
            scriptPath,
            {
                QStringLiteral(
                    "-ContractProbe"),
            });
    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    QCOMPARE(
        object.value(
            QStringLiteral(
                "scalePercent"))
            .toArray(),
        QJsonArray({
            100,
            125,
            150,
            200,
        }));
    QCOMPARE(
        object.value(
            QStringLiteral(
                "surfaces"))
            .toArray(),
        QJsonArray({
            QStringLiteral("settings"),
            QStringLiteral("pet"),
            QStringLiteral("processes"),
            QStringLiteral("chat"),
        }));
    QCOMPARE(
        object.value(
            QStringLiteral(
                "startupRoutes"))
            .toArray(),
        QJsonArray({
            QStringLiteral("none"),
            QStringLiteral("processes"),
            QStringLiteral("local-chat"),
        }));
    QCOMPARE(
        object.value(
            QStringLiteral(
                "isolatedProfile"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "restoresPreviousInstance"))
            .toBool(),
        true);
}

void CutoverAutomationTests::
    petDragReversalVerifierCoversRenderedReversalsAndRestoration()
{
    const QString scriptPath =
        QStringLiteral(
            COMPANION_PET_DRAG_REVERSAL_SCRIPT_PATH);
    const QString source =
        readUtf8(scriptPath);
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral(
                "CODEX_COMPANION_TEST_STANDARD_PATHS"),
            QStringLiteral(
                "LOCALAPPDATA"),
            QStringLiteral(
                "qttest\\DaSilverFire\\Codex Companion\\CodexCompanion.ini"),
            QStringLiteral(
                "PrintWindow"),
            QStringLiteral(
                "SetCursorPos"),
            QStringLiteral(
                "SendInput"),
            QStringLiteral(
                "GetWindowRect"),
            QStringLiteral(
                "PostMessage"),
            QStringLiteral(
                "WM_CLOSE"),
            QStringLiteral(
                "Close Settings"),
            QStringLiteral(
                "TemporarilyStopRunningInstance"),
            QStringLiteral(
                "stationary-after-right-reversal"),
            QStringLiteral(
                "stationary-after-left-reversal"),
            QStringLiteral(
                "productionSettingsPreserved"),
            QStringLiteral(
                "qtTestSettingsPreserved"),
            QStringLiteral(
                "measuredCaptureRate"),
            QStringLiteral(
                "NextCaptureDueMilliseconds"),
            QStringLiteral(
                "pet-drag-reversal-summary.json"),
        });

    const auto result =
        runPowerShell(
            scriptPath,
            {
                QStringLiteral(
                    "-ContractProbe"),
            });
    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    const QJsonDocument document =
        QJsonDocument::fromJson(
            result.standardOutput.trimmed());
    QVERIFY(document.isObject());
    const QJsonObject object =
        document.object();
    QCOMPARE(
        object.value(
            QStringLiteral(
                "schemaVersion"))
            .toInt(),
        1);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "scenario"))
            .toString(),
        QStringLiteral(
            "pet-drag-reversal"));
    QCOMPARE(
        object.value(
            QStringLiteral(
                "directions"))
            .toArray(),
        QJsonArray({
            QStringLiteral("left"),
            QStringLiteral("right"),
            QStringLiteral("left"),
        }));
    QCOMPARE(
        object.value(
            QStringLiteral(
                "stationaryAfterReversal"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "isolatedProfile"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "exactWindowCapture"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "productionSettingsPreserved"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "qtTestSettingsPreserved"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "recordsMeasuredCadence"))
            .toBool(),
        true);
    QCOMPARE(
        object.value(
            QStringLiteral(
                "restoresPreviousInstance"))
            .toBool(),
        true);
}

void CutoverAutomationTests::
    dotnetFallbackUsesExistingPublisherAndRecordsEvidence()
{
    const QString source =
        readUtf8(
            QStringLiteral(
                COMPANION_DOTNET_FALLBACK_SCRIPT_PATH));
    QVERIFY(!source.isEmpty());
    requireTokens(
        source,
        {
            QStringLiteral("scripts\\publish-windows.ps1"),
            QStringLiteral("[xml]"),
            QStringLiteral("CodexCompanion.App.csproj"),
            QStringLiteral("-Version"),
            QStringLiteral("-OutputRoot"),
            QStringLiteral("Get-FileHash"),
            QStringLiteral("SHA256SUMS"),
            QStringLiteral("fallback-metadata.json"),
            QStringLiteral("fallback-verification.json"),
            QStringLiteral("sourceStatusBefore"),
            QStringLiteral("sourceStatusAfter"),
            QStringLiteral("source scope changed"),
            QStringLiteral("Codex-Companion-"),
            QStringLiteral("coreclr.dll"),
            QStringLiteral("hostfxr.dll"),
            QStringLiteral("Compress-Archive"),
            QStringLiteral("sanitized"),
            QStringLiteral("'.pdb'"),
            QStringLiteral("'.dbg'"),
        });
    QVERIFY(
        !source.contains(
            QStringLiteral("dotnet publish"),
            Qt::CaseInsensitive));
}

void CutoverAutomationTests::
    dotnetFallbackRejectsDirtySourceBeforePublishing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fixture =
        createFallbackFixture(directory);
    auto environment =
        QProcessEnvironment::
            systemEnvironment();
    environment.insert(
        QStringLiteral("FAKE_GIT_DIRTY"),
        QStringLiteral("1"));

    const auto result =
        runPowerShell(
            QStringLiteral(
                COMPANION_DOTNET_FALLBACK_SCRIPT_PATH),
            fallbackArguments(fixture),
            environment);

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode != 0,
        result.standardOutput.constData());
    QVERIFY(
        result.standardError.contains(
            "must be clean")
        || result.standardOutput.contains(
            "must be clean"));
    QCOMPARE(
        readBytes(
            QDir(fixture.outputDir)
                .filePath(
                    QStringLiteral(
                        "verified-sentinel.txt"))),
        QByteArrayLiteral(
            "previous verified fallback\n"));
}

void CutoverAutomationTests::
    dotnetFallbackPreservesVerifiedOutputWhenReplacementFails()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fixture =
        createFallbackFixture(directory);
    auto environment =
        QProcessEnvironment::
            systemEnvironment();
    environment.insert(
        QStringLiteral(
            "FAKE_PUBLISH_OMIT_ARM64_HOSTFXR"),
        QStringLiteral("1"));

    const auto result =
        runPowerShell(
            QStringLiteral(
                COMPANION_DOTNET_FALLBACK_SCRIPT_PATH),
            fallbackArguments(fixture),
            environment);

    QVERIFY(result.finished);
    QVERIFY(result.exitCode != 0);
    QCOMPARE(
        readBytes(
            QDir(fixture.outputDir)
                .filePath(
                    QStringLiteral(
                        "verified-sentinel.txt"))),
        QByteArrayLiteral(
            "previous verified fallback\n"));
}

void CutoverAutomationTests::
    dotnetFallbackPromotesValidatedOutputAndArchivesPrevious()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fixture =
        createFallbackFixture(directory);

    const auto result =
        runPowerShell(
            QStringLiteral(
                COMPANION_DOTNET_FALLBACK_SCRIPT_PATH),
            fallbackArguments(fixture));

    QVERIFY(result.finished);
    QVERIFY2(
        result.exitCode == 0,
        result.standardError.constData());
    QVERIFY(
        !QFileInfo::exists(
            QDir(fixture.outputDir)
                .filePath(
                    QStringLiteral(
                        "verified-sentinel.txt"))));

    const QJsonDocument metadata =
        QJsonDocument::fromJson(
            readBytes(
                QDir(fixture.outputDir)
                    .filePath(
                        QStringLiteral(
                            "fallback-metadata.json"))));
    QVERIFY(metadata.isObject());
    QCOMPARE(
        metadata.object()
            .value(
                QStringLiteral(
                    "sourceScopeClean"))
            .toBool(),
        true);
    QCOMPARE(
        metadata.object()
            .value(
                QStringLiteral(
                    "sourceCommit"))
            .toString(),
        QString(40, QLatin1Char('a')));
    QCOMPARE(
        metadata.object()
            .value(
                QStringLiteral(
                    "windowsTree"))
            .toString(),
        QString(40, QLatin1Char('b')));

    const QString releaseRoot =
        QFileInfo(fixture.outputDir)
            .absolutePath();
    const QString outputName =
        QFileInfo(fixture.outputDir)
            .fileName();
    const QStringList previous =
        QDir(releaseRoot).entryList(
            {
                outputName
                    + QStringLiteral(
                        ".previous-*"),
            },
            QDir::Dirs
                | QDir::NoDotAndDotDot);
    QCOMPARE(previous.size(), 1);
    QCOMPARE(
        readBytes(
            QDir(
                QDir(releaseRoot)
                    .filePath(
                        previous.first()))
                .filePath(
                    QStringLiteral(
                        "verified-sentinel.txt"))),
        QByteArrayLiteral(
            "previous verified fallback\n"));
}

} // namespace

QTEST_GUILESS_MAIN(CutoverAutomationTests)

#include "CutoverAutomationTests.moc"
