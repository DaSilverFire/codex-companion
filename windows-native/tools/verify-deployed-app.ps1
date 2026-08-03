[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ExecutablePath,

    [ValidateRange(1000, 30000)]
    [int]$StartupTimeoutMilliseconds = 6000
)

$ErrorActionPreference = "Stop"

$resolvedExecutable = (Resolve-Path -LiteralPath $ExecutablePath).Path
$workingDirectory = Split-Path -Parent $resolvedExecutable
$expectedProcessName = [System.IO.Path]::GetFileNameWithoutExtension(
    $resolvedExecutable)
$packageRoot = if (
    (Split-Path -Leaf $workingDirectory) -ieq "bin"
) {
    Split-Path -Parent $workingDirectory
}
else {
    $workingDirectory
}

$requiredUpdaterFiles = @(
    "CodexCompanionUpdater.exe",
    "Qt6Core.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
$missingUpdaterFiles = $requiredUpdaterFiles |
    Where-Object {
        -not (Test-Path -LiteralPath (
            Join-Path $workingDirectory $_) -PathType Leaf)
    }
if ($missingUpdaterFiles) {
    throw (
        "The deployed Companion package is missing updater files: " +
        ($missingUpdaterFiles -join ", "))
}

$webpPluginCandidates = @(
    "plugins\imageformats\qwebp.dll",
    "plugins\imageformats\qwebpd.dll"
)
$deployedWebpPlugins = $webpPluginCandidates |
    Where-Object {
        Test-Path -LiteralPath (
            Join-Path $packageRoot $_) -PathType Leaf
    }
if (-not $deployedWebpPlugins) {
    throw (
        "The deployed Companion package is missing its Qt WebP image-format " +
        "plugin. Expected one of: " +
        ($webpPluginCandidates -join ", "))
}

$requiredCompilerRuntimeFiles = @(
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_atomic_wait.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll"
)
$missingCompilerRuntimeFiles = $requiredCompilerRuntimeFiles |
    Where-Object {
        -not (Test-Path -LiteralPath (
            Join-Path $workingDirectory $_))
    }
if ($missingCompilerRuntimeFiles) {
    throw (
        "The deployed Companion package is missing app-local MSVC runtime " +
        "files: " +
        ($missingCompilerRuntimeFiles -join ", "))
}

$compilerRuntimeInstaller = Join-Path (
    $workingDirectory) "vc_redist.x64.exe"
if (Test-Path -LiteralPath $compilerRuntimeInstaller) {
    throw (
        "The deployed Companion package must use only app-local MSVC " +
        "runtime DLLs; remove vc_redist.x64.exe.")
}

$existing = Get-Process -Name $expectedProcessName -ErrorAction SilentlyContinue |
    Where-Object {
        try {
            $_.Path -eq $resolvedExecutable
        }
        catch {
            $false
        }
    }
if ($existing) {
    throw "A deployed-app smoke-test process is already running: $resolvedExecutable"
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CompanionDeploymentProbe
{
    private static readonly IntPtr HwndMessage = new IntPtr(-3);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(
        IntPtr parent,
        IntPtr childAfter,
        string className,
        string windowName);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    public static bool HasTrayHost(uint expectedProcessId)
    {
        IntPtr current = IntPtr.Zero;
        while (true)
        {
            current = FindWindowEx(
                HwndMessage,
                current,
                "CodexCompanion.NotificationAreaHost",
                null);
            if (current == IntPtr.Zero)
            {
                return false;
            }

            uint processId;
            GetWindowThreadProcessId(current, out processId);
            if (processId == expectedProcessId)
            {
                return true;
            }
        }
    }
}
'@

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $resolvedExecutable
$startInfo.WorkingDirectory = $workingDirectory
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$startInfo.EnvironmentVariables["PATH"] = (
    "$env:SystemRoot\System32;" +
    "$env:SystemRoot;" +
    "$env:SystemRoot\System32\Wbem")
foreach ($name in @(
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
    "QML2_IMPORT_PATH",
    "QML_IMPORT_PATH"
)) {
    $startInfo.EnvironmentVariables.Remove($name)
}

$updaterSmokeParent = Join-Path (
    [System.IO.Path]::GetTempPath()) "CodexCompanionUpdaterSmoke"
$updaterSmokeRoot = Join-Path $updaterSmokeParent (
    [Guid]::NewGuid().ToString("D"))
$resolvedTemp = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath())
$resolvedUpdaterSmokeRoot = [System.IO.Path]::GetFullPath(
    $updaterSmokeRoot)
if (-not $resolvedUpdaterSmokeRoot.StartsWith(
        $resolvedTemp,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "The updater smoke-test path escaped the system temporary directory."
}

$updaterBootstrapReady = $false
$updaterResultReady = $false
try {
    New-Item -ItemType Directory -Path $updaterSmokeRoot |
        Out-Null
    Copy-Item -LiteralPath (
        Join-Path $workingDirectory "CodexCompanionUpdater.exe") -Destination (
        Join-Path $updaterSmokeRoot "CodexCompanionUpdater.exe")
    Get-ChildItem -LiteralPath $workingDirectory -Filter "*.dll" -File |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (
                Join-Path $updaterSmokeRoot $_.Name)
        }

    $updaterStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $updaterStartInfo.FileName = Join-Path (
        $updaterSmokeRoot) "CodexCompanionUpdater.exe"
    $updaterStartInfo.WorkingDirectory = $updaterSmokeRoot
    $updaterStartInfo.UseShellExecute = $false
    $updaterStartInfo.CreateNoWindow = $true
    $updaterStartInfo.EnvironmentVariables["PATH"] = (
        "$env:SystemRoot\System32;" +
        "$env:SystemRoot;" +
        "$env:SystemRoot\System32\Wbem")

    $updaterProcess = [System.Diagnostics.Process]::new()
    $updaterProcess.StartInfo = $updaterStartInfo
    try {
        if (-not $updaterProcess.Start()) {
            throw "Windows did not start the detached updater smoke test."
        }
        if (-not $updaterProcess.WaitForExit(10000)) {
            $updaterProcess.Kill()
            $updaterProcess.WaitForExit()
            throw "The detached updater smoke test did not exit."
        }
        if ($updaterProcess.ExitCode -ne 2) {
            throw (
                "The detached updater did not reach argument validation. " +
                "Exit code: $($updaterProcess.ExitCode).")
        }
        $updaterBootstrapReady = $true
    }
    finally {
        $updaterProcess.Dispose()
    }

    $invalidRequestPath = Join-Path (
        $updaterSmokeRoot) "request.json"
    [System.IO.File]::WriteAllText(
        $invalidRequestPath,
        "{}",
        [System.Text.UTF8Encoding]::new($false))
    $updaterStartInfo.Arguments = (
        "--request `"$invalidRequestPath`"")
    $updaterProcess = [System.Diagnostics.Process]::new()
    $updaterProcess.StartInfo = $updaterStartInfo
    try {
        if (-not $updaterProcess.Start()) {
            throw "Windows did not start the updater result smoke test."
        }
        if (-not $updaterProcess.WaitForExit(10000)) {
            $updaterProcess.Kill()
            $updaterProcess.WaitForExit()
            throw "The updater result smoke test did not exit."
        }
        if ($updaterProcess.ExitCode -ne 3) {
            throw (
                "The updater did not reject an invalid request. " +
                "Exit code: $($updaterProcess.ExitCode).")
        }
    }
    finally {
        $updaterProcess.Dispose()
    }

    $updaterResultPath = Join-Path (
        $updaterSmokeRoot) "result.json"
    if (-not (Test-Path -LiteralPath $updaterResultPath -PathType Leaf)) {
        throw "The updater did not retain result.json."
    }
    $updaterResult = Get-Content -LiteralPath (
        $updaterResultPath) -Raw | ConvertFrom-Json
    if (
        $updaterResult.schema -ne 1 -or
        [bool]$updaterResult.success -or
        $updaterResult.errorCode -ne "update.install_request_invalid" -or
        [string]::IsNullOrWhiteSpace(
            [string]$updaterResult.completedAtUtc) -or
        [System.IO.Path]::GetFullPath(
            [string]$updaterResult.installerLogPath) -ne
            [System.IO.Path]::GetFullPath(
                (Join-Path $updaterSmokeRoot "installer.log"))
    ) {
        throw "The updater retained an invalid result.json contract."
    }
    $updaterResultReady = $true
}
finally {
    if (Test-Path -LiteralPath $updaterSmokeRoot) {
        Remove-Item -LiteralPath $updaterSmokeRoot -Recurse -Force
    }
}

$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $startInfo
$started = $false
$completedSuccessfully = $false

try {
    try {
        $started = $process.Start()
    }
    catch {
        throw (
            "Failed to start deployed Companion executable " +
            "'$resolvedExecutable': $($_.Exception.Message)")
    }
    if (-not $started) {
        throw "Windows did not start the deployed Companion executable."
    }

    $deadline = [DateTime]::UtcNow.AddMilliseconds(
        $StartupTimeoutMilliseconds)
    $trayHostReady = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) {
            $stdout = $process.StandardOutput.ReadToEnd()
            $stderr = $process.StandardError.ReadToEnd()
            throw (
                "Companion exited before its tray host was ready. " +
                "Exit code: $($process.ExitCode). " +
                "stdout: $stdout stderr: $stderr")
        }

        if ([CompanionDeploymentProbe]::HasTrayHost(
                [uint32]$process.Id)) {
            $trayHostReady = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }

    if (-not $trayHostReady) {
        throw (
            "Companion stayed alive but did not create its tray host within " +
            "$StartupTimeoutMilliseconds ms.")
    }

    $resultJson = [pscustomobject]@{
        executable = $resolvedExecutable
        processId = $process.Id
        trayHostReady = $trayHostReady
        sanitizedPath = $startInfo.EnvironmentVariables["PATH"]
        updaterBootstrapReady = $updaterBootstrapReady
        updaterResultReady = $updaterResultReady
        updaterFiles = $requiredUpdaterFiles
        imageFormatFiles = $deployedWebpPlugins
        compilerRuntimeFiles = $requiredCompilerRuntimeFiles
    } | ConvertTo-Json -Compress
    $completedSuccessfully = $true
    $resultJson
}
finally {
    $cleanupError = $null
    if ($started) {
        try {
            if (-not $process.HasExited) {
                $process.Kill()
                $process.WaitForExit()
            }
        }
        catch {
            $cleanupError = $_
        }
    }
    try {
        $process.Dispose()
    }
    catch {
        if ($null -eq $cleanupError) {
            $cleanupError = $_
        }
    }
    if ($completedSuccessfully -and $null -ne $cleanupError) {
        throw $cleanupError
    }
}
