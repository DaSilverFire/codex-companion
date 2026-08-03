[CmdletBinding()]
param(
    [ValidateSet('Installed', 'Update', 'Uninstalled')]
    [string]$Mode = 'Installed',

    [string]$ConfigurationPath,

    [string]$EvidenceRoot,

    [string]$ExpectedVersion,

    [int]$ExpectedBuild = 0,

    [string]$InitialExpectedVersion,

    [int]$InitialExpectedBuild = 0,

    [string]$ExpectedSignerSha256,

    [string]$SourceInstallerPath,

    [string]$ExpectedSourceSha256,

    [string]$BaselineUserDataPath,

    [ValidateSet(100, 125, 150, 200)]
    [int]$DpiPercent = 100,

    [int]$TimeoutSeconds = 120,

    [bool]$RequireSettingsVisible = $true,

    [bool]$DriveUpdateUi = $false,

    [string]$UpdateFeedUrl,

    [bool]$RequireUserData = $false,

    [bool]$SeedDurableState = $false,

    [bool]$RequireSeededDurableState = $false,

    [bool]$RequireSensitiveLogClean = $false,

    [switch]$PersistenceProbe,

    [bool]$LeaveRunning = $false,

    [string]$InstallRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-Configuration {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "The clean-VM verification configuration was not found: $Path"
    }
    $configuration =
        Get-Content -LiteralPath $Path -Raw |
        ConvertFrom-Json
    foreach ($property in $configuration.PSObject.Properties) {
        switch ($property.Name) {
        'mode' {
            $script:Mode = [string]$property.Value
        }
        'evidenceRoot' {
            $script:EvidenceRoot = [string]$property.Value
        }
        'expectedVersion' {
            $script:ExpectedVersion = [string]$property.Value
        }
        'expectedBuild' {
            $script:ExpectedBuild = [int]$property.Value
        }
        'initialExpectedVersion' {
            $script:InitialExpectedVersion = [string]$property.Value
        }
        'initialExpectedBuild' {
            $script:InitialExpectedBuild = [int]$property.Value
        }
        'expectedSignerSha256' {
            $script:ExpectedSignerSha256 = [string]$property.Value
        }
        'sourceInstallerPath' {
            $script:SourceInstallerPath = [string]$property.Value
        }
        'expectedSourceSha256' {
            $script:ExpectedSourceSha256 = [string]$property.Value
        }
        'baselineUserDataPath' {
            $script:BaselineUserDataPath = [string]$property.Value
        }
        'dpiPercent' {
            $script:DpiPercent = [int]$property.Value
        }
        'timeoutSeconds' {
            $script:TimeoutSeconds = [int]$property.Value
        }
        'requireSettingsVisible' {
            $script:RequireSettingsVisible = [bool]$property.Value
        }
        'driveUpdateUi' {
            $script:DriveUpdateUi = [bool]$property.Value
        }
        'updateFeedUrl' {
            $script:UpdateFeedUrl = [string]$property.Value
        }
        'requireUserData' {
            $script:RequireUserData = [bool]$property.Value
        }
        'seedDurableState' {
            $script:SeedDurableState = [bool]$property.Value
        }
        'requireSeededDurableState' {
            $script:RequireSeededDurableState =
                [bool]$property.Value
        }
        'requireSensitiveLogClean' {
            $script:RequireSensitiveLogClean =
                [bool]$property.Value
        }
        'leaveRunning' {
            $script:LeaveRunning = [bool]$property.Value
        }
        'installRoot' {
            $script:InstallRoot = [string]$property.Value
        }
        }
    }
}

Import-Configuration -Path $ConfigurationPath
$contractModule =
    Join-Path $PSScriptRoot 'CleanVmContract.psm1'
if (-not (Test-Path -LiteralPath $contractModule -PathType Leaf)) {
    throw "The clean-VM contract module was not found: $contractModule"
}
Import-Module `
    -Name $contractModule `
    -Force

if ($Mode -notin @('Installed', 'Update', 'Uninstalled')) {
    throw "Unsupported clean-VM verification mode: $Mode"
}
if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    throw '-EvidenceRoot is required.'
}
if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path `
        $env:LOCALAPPDATA `
        'Programs\Codex Companion'
}
$EvidenceRoot = [System.IO.Path]::GetFullPath($EvidenceRoot)
New-Item `
    -ItemType Directory `
    -Path $EvidenceRoot `
    -Force | Out-Null

$utf8WithoutBom =
    New-Object System.Text.UTF8Encoding($false)
function Write-JsonAtomic {
    param(
        [string]$Path,
        [object]$Value
    )

    $temporaryPath = "$Path.partial"
    [System.IO.File]::WriteAllText(
        $temporaryPath,
        (($Value | ConvertTo-Json -Depth 12) + "`n"),
        $utf8WithoutBom
    )
    Move-Item `
        -LiteralPath $temporaryPath `
        -Destination $Path `
        -Force
}

$checks =
    New-Object System.Collections.ArrayList
function Add-Check {
    param(
        [string]$Id,
        [bool]$Passed,
        [string]$Detail
    )

    [void]$checks.Add(
        [ordered]@{
            id = $Id
            passed = $Passed
            detail = $Detail
        }
    )
}

function Get-CertificateSha256 {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]
        $Certificate
    )

    if ($null -eq $Certificate) {
        return ''
    }
    $algorithm =
        [System.Security.Cryptography.SHA256]::Create()
    try {
        return (
            $algorithm.ComputeHash($Certificate.RawData) |
                ForEach-Object { $_.ToString('x2') }
        ) -join ''
    } finally {
        $algorithm.Dispose()
    }
}

function Get-SignatureEvidence {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [ordered]@{
            path = [System.IO.Path]::GetFileName($Path)
            status = 'Missing'
            signerSha256 = ''
        }
    }
    $signature =
        Get-AuthenticodeSignature -LiteralPath $Path
    return [ordered]@{
        path = [System.IO.Path]::GetFileName($Path)
        status = [string]$signature.Status
        signerSha256 =
            Get-CertificateSha256 `
                -Certificate $signature.SignerCertificate
    }
}

function Get-ExactCompanionProcesses {
    param([string]$ExecutablePath)

    $expected =
        [System.IO.Path]::GetFullPath($ExecutablePath)
    return @(
        Get-CimInstance Win32_Process `
            -Filter "Name='CodexCompanion.exe'" |
            Where-Object {
                -not [string]::IsNullOrWhiteSpace(
                    [string]$_.ExecutablePath
                ) -and
                [System.IO.Path]::GetFullPath(
                    [string]$_.ExecutablePath
                ) -ieq $expected
            }
    )
}

function Get-ExactUpdaterProcesses {
    param([string]$ExecutablePath)

    $expected =
        [System.IO.Path]::GetFullPath($ExecutablePath)
    return @(
        Get-CimInstance `
            Win32_Process `
            -Filter "Name='CodexCompanionUpdater.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                -not [string]::IsNullOrWhiteSpace(
                    [string]$_.ExecutablePath
                ) -and
                [System.IO.Path]::GetFullPath(
                    [string]$_.ExecutablePath
                ) -ieq $expected
            }
    )
}

function Wait-Until {
    param(
        [scriptblock]$Condition,
        [int]$Seconds,
        [int]$PollMilliseconds = 250
    )

    $deadline =
        [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $value = & $Condition
        if ($value) {
            return $value
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Start-CompanionProcess {
    param(
        [string]$ExecutablePath,
        [string]$LaunchMode,
        [string]$FeedUrl
    )

    $launchArguments = @(
        Get-CompanionLaunchArguments `
            -Mode $LaunchMode `
            -UpdateFeedUrl $FeedUrl
    )
    $startParameters = @{
        FilePath = $ExecutablePath
        WorkingDirectory =
            Split-Path -Parent $ExecutablePath
    }
    if ($launchArguments.Count -gt 0) {
        $startParameters.ArgumentList =
            $launchArguments
    }
    Start-Process @startParameters | Out-Null
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class CompanionCleanVmNative
{
    public const int GWL_EXSTYLE = -20;
    public const long WS_EX_TOOLWINDOW = 0x00000080L;
    public const long WS_EX_APPWINDOW = 0x00040000L;
    public const long WS_EX_NOACTIVATE = 0x08000000L;
    public const uint GW_OWNER = 4;
    public const uint GA_ROOTOWNER = 3;
    public const uint DWMWA_CLOAKED = 14;
    public const uint WM_CLOSE = 0x0010;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct NOTIFYICONIDENTIFIER
    {
        public uint cbSize;
        public IntPtr hWnd;
        public uint uID;
        public Guid guidItem;
    }

    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool EnumWindows(
        EnumWindowsProc callback,
        IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(
        IntPtr hWnd,
        out uint processId);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr64(
        IntPtr hWnd,
        int index);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong32(
        IntPtr hWnd,
        int index);

    public static long GetWindowLongPtrSafe(IntPtr hWnd, int index)
    {
        return IntPtr.Size == 8
            ? GetWindowLongPtr64(hWnd, index).ToInt64()
            : GetWindowLong32(hWnd, index);
    }

    [DllImport("user32.dll")]
    public static extern IntPtr GetWindow(IntPtr hWnd, uint command);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(
        IntPtr hWnd,
        uint flags);

    [DllImport("user32.dll")]
    public static extern IntPtr GetLastActivePopup(IntPtr hWnd);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(
        IntPtr hWnd,
        uint attribute,
        out int value,
        int valueSize);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextLengthW(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(
        IntPtr hWnd,
        StringBuilder value,
        int length);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(
        IntPtr hWnd,
        out RECT rect);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(
        IntPtr hWnd,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(
        string className,
        string windowName);

    [DllImport("shell32.dll")]
    public static extern int Shell_NotifyIconGetRect(
        ref NOTIFYICONIDENTIFIER identifier,
        out RECT iconLocation);
}
'@

function Test-WindowCloaked {
    param([IntPtr]$Window)

    [int]$cloaked = 0
    $result =
        [CompanionCleanVmNative]::DwmGetWindowAttribute(
            $Window,
            [CompanionCleanVmNative]::DWMWA_CLOAKED,
            [ref]$cloaked,
            [Runtime.InteropServices.Marshal]::SizeOf(
                [int]
            )
        )
    return $result -ge 0 -and $cloaked -ne 0
}

function Get-AltTabRepresentative {
    param(
        [IntPtr]$Window,
        [bool]$AppWindow
    )

    if ($AppWindow) {
        return $Window
    }

    $walk =
        [CompanionCleanVmNative]::GetAncestor(
            $Window,
            [CompanionCleanVmNative]::GA_ROOTOWNER
        )
    if ($walk -eq [IntPtr]::Zero) {
        $walk = $Window
    }

    $visited =
        New-Object 'System.Collections.Generic.HashSet[long]'
    for ($index = 0; $index -lt 64; $index++) {
        if (-not $visited.Add($walk.ToInt64())) {
            break
        }
        $popup =
            [CompanionCleanVmNative]::GetLastActivePopup(
                $walk
            )
        if (
            $popup -eq [IntPtr]::Zero -or
            $popup -eq $walk
        ) {
            break
        }

        $popupStyle =
            [CompanionCleanVmNative]::GetWindowLongPtrSafe(
                $popup,
                [CompanionCleanVmNative]::GWL_EXSTYLE
            )
        $popupToolWindow =
            ($popupStyle -band
                [CompanionCleanVmNative]::WS_EX_TOOLWINDOW) -ne 0
        $popupNoActivate =
            ($popupStyle -band
                [CompanionCleanVmNative]::WS_EX_NOACTIVATE) -ne 0
        if (
            [CompanionCleanVmNative]::IsWindowVisible(
                $popup
            ) -and
            -not (Test-WindowCloaked -Window $popup) -and
            -not $popupToolWindow -and
            -not $popupNoActivate
        ) {
            return $popup
        }
        $walk = $popup
    }

    return $walk
}

function Get-WindowEvidence {
    param([int[]]$ProcessIds)

    $windows =
        New-Object System.Collections.ArrayList
    $callback =
        [CompanionCleanVmNative+EnumWindowsProc] {
            param(
                [IntPtr]$window,
                [IntPtr]$ignored
            )

            [uint32]$processId = 0
            [void][CompanionCleanVmNative]::
                GetWindowThreadProcessId(
                    $window,
                    [ref]$processId
                )
            if ($ProcessIds -notcontains [int]$processId) {
                return $true
            }

            $visible =
                [CompanionCleanVmNative]::IsWindowVisible($window)
            $exStyle =
                [CompanionCleanVmNative]::
                    GetWindowLongPtrSafe(
                        $window,
                        [CompanionCleanVmNative]::GWL_EXSTYLE
                    )
            $owner =
                [CompanionCleanVmNative]::GetWindow(
                    $window,
                    [CompanionCleanVmNative]::GW_OWNER
                )
            $toolWindow =
                ($exStyle -band
                    [CompanionCleanVmNative]::WS_EX_TOOLWINDOW) -ne 0
            $appWindow =
                ($exStyle -band
                    [CompanionCleanVmNative]::WS_EX_APPWINDOW) -ne 0
            $noActivate =
                ($exStyle -band
                    [CompanionCleanVmNative]::WS_EX_NOACTIVATE) -ne 0
            $cloaked =
                Test-WindowCloaked -Window $window
            $shellVisible =
                $visible -and -not $cloaked
            $altTabRepresentative =
                Get-AltTabRepresentative `
                    -Window $window `
                    -AppWindow $appWindow
            $altTabCandidate =
                $shellVisible -and
                -not $toolWindow -and
                -not $noActivate -and
                $altTabRepresentative -eq $window
            $taskbarCandidate =
                $shellVisible -and
                (
                    $appWindow -or
                    (
                        $owner -eq [IntPtr]::Zero -and
                        -not $toolWindow -and
                        -not $noActivate
                    )
                )

            $length =
                [CompanionCleanVmNative]::
                    GetWindowTextLengthW($window)
            $builder =
                New-Object System.Text.StringBuilder(
                    [Math]::Max(1, $length + 1)
                )
            [void][CompanionCleanVmNative]::GetWindowTextW(
                $window,
                $builder,
                $builder.Capacity
            )
            $rect =
                New-Object CompanionCleanVmNative+RECT
            [void][CompanionCleanVmNative]::GetWindowRect(
                $window,
                [ref]$rect
            )
            [void]$windows.Add(
                [ordered]@{
                    hwnd = $window.ToInt64()
                    processId = [int]$processId
                    title = $builder.ToString()
                    visible = $visible
                    cloaked = $cloaked
                    owner = $owner.ToInt64()
                    exStyle = $exStyle
                    toolWindow = $toolWindow
                    appWindow = $appWindow
                    noActivate = $noActivate
                    altTabRepresentative =
                        $altTabRepresentative.ToInt64()
                    altTabCandidate = $altTabCandidate
                    taskbarCandidate = $taskbarCandidate
                    bounds = [ordered]@{
                        left = $rect.Left
                        top = $rect.Top
                        right = $rect.Right
                        bottom = $rect.Bottom
                    }
                }
            )
            return $true
        }
    [void][CompanionCleanVmNative]::EnumWindows(
        $callback,
        [IntPtr]::Zero
    )
    return @($windows)
}

function Get-TrayEvidence {
    $hostWindow =
        [CompanionCleanVmNative]::FindWindowW(
            'CodexCompanion.NotificationAreaHost',
            $null
        )
    if ($hostWindow -eq [IntPtr]::Zero) {
        return [ordered]@{
            present = $false
            hostHwnd = 0
            hresult = -1
        }
    }
    $identifier =
        New-Object CompanionCleanVmNative+NOTIFYICONIDENTIFIER
    $identifier.cbSize =
        [Runtime.InteropServices.Marshal]::SizeOf($identifier)
    $identifier.hWnd = $hostWindow
    $identifier.uID = 1
    $identifier.guidItem =
        [Guid]'9B3C42CB-4B7F-4A08-B675-071708948C88'
    $rect =
        New-Object CompanionCleanVmNative+RECT
    $hresult =
        [CompanionCleanVmNative]::Shell_NotifyIconGetRect(
            [ref]$identifier,
            [ref]$rect
        )
    return [ordered]@{
        present = $hresult -eq 0
        hostHwnd = $hostWindow.ToInt64()
        hresult = $hresult
        bounds = [ordered]@{
            left = $rect.Left
            top = $rect.Top
            right = $rect.Right
            bottom = $rect.Bottom
        }
    }
}

function Get-CompanionUpdateCompletionEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Transaction,

        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath,

        [int]$PreviousProcessId,

        [string]$ExpectedVersion,

        [int]$ExpectedBuild
    )

    $result = $null
    $resultValid = $false
    if (
        Test-Path `
            -LiteralPath $Transaction.resultPath `
            -PathType Leaf
    ) {
        try {
            $result =
                Get-Content `
                    -LiteralPath $Transaction.resultPath `
                    -Raw |
                ConvertFrom-Json
            $resultValid =
                Test-CompanionUpdateResult `
                    -Result $result `
                    -RequestId (
                        [string]$Transaction.request.requestId
                    ) `
                    -TransactionRoot $Transaction.root
        } catch {
            $result = $null
            $resultValid = $false
        }
    }

    $helperProcesses = @(
        Get-ExactUpdaterProcesses `
            -ExecutablePath $Transaction.helperPath
    )
    $appProcesses = @(
        Get-ExactCompanionProcesses `
            -ExecutablePath $ExecutablePath
    )
    $relaunchedProcess =
        $appProcesses.Count -eq 1 -and
        [int]$appProcesses[0].ProcessId -ne
            $PreviousProcessId

    $productVersion = ''
    $fileVersion = ''
    $versionIdentity = $false
    if (
        Test-Path `
            -LiteralPath $ExecutablePath `
            -PathType Leaf
    ) {
        $versionInfo =
            (Get-Item -LiteralPath $ExecutablePath).VersionInfo
        $productVersion =
            [string]$versionInfo.ProductVersion
        $fileVersion =
            [string]$versionInfo.FileVersion
        $versionIdentity =
            Test-CompanionVersionIdentity `
                -ProductVersion $productVersion `
                -FileVersion $fileVersion `
                -ExpectedVersion $ExpectedVersion `
                -ExpectedBuild $ExpectedBuild
    }

    $tray = Get-TrayEvidence
    $rollbackRemoved =
        -not (
            Test-Path `
                -LiteralPath $Transaction.rollbackRoot
        )
    $helperExited =
        $helperProcesses.Count -eq 0

    return [ordered]@{
        complete =
            $resultValid -and
            $helperExited -and
            $rollbackRemoved -and
            $relaunchedProcess -and
            $versionIdentity -and
            [bool]$tray.present
        resultValid = $resultValid
        result = $result
        helperExited = $helperExited
        helperProcessIds = @(
            $helperProcesses |
                ForEach-Object {
                    [int]$_.ProcessId
                }
        )
        rollbackRemoved = $rollbackRemoved
        rollbackRoot = $Transaction.rollbackRoot
        relaunchedProcess = $relaunchedProcess
        previousProcessId = $PreviousProcessId
        processId = if ($appProcesses.Count -eq 1) {
            [int]$appProcesses[0].ProcessId
        } else {
            0
        }
        exactProcessCount = $appProcesses.Count
        versionIdentity = $versionIdentity
        productVersion = $productVersion
        fileVersion = $fileVersion
        tray = $tray
    }
}

function Save-DesktopScreenshot {
    param([string]$Path)

    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $bounds =
        [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap =
        New-Object System.Drawing.Bitmap(
            $bounds.Width,
            $bounds.Height
        )
    $graphics =
        [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen(
            $bounds.Left,
            $bounds.Top,
            0,
            0,
            $bitmap.Size
        )
        $bitmap.Save(
            $Path,
            [System.Drawing.Imaging.ImageFormat]::Png
        )
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Get-CompanionDurableStateRoots {
    return [ordered]@{
        settings =
            Join-Path `
                $env:LOCALAPPDATA `
                'DaSilverFire\Codex Companion\CodexCompanion.ini'
        credentials =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\Credentials'
        pairings =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\Security\paired-devices.v1.json'
        goals =
            Join-Path `
                $env:USERPROFILE `
                '.codex\goals_1.sqlite'
        privacy =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\PrivacyCanaries'
        pets =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\Pets'
        legacySettings =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\settings.json'
        petWindow =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\pet-window-frame.json'
        localInstallationIdentity =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\Security\installation-id'
        relayState =
            Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion\Security\relay-state.v1.dpapi'
        roamingInstallationIdentity =
            Join-Path `
                $env:APPDATA `
                'DaSilverFire\Codex Companion\Security\installation-id'
        tlsIdentity =
            Join-Path `
                $env:APPDATA `
                (
                    'DaSilverFire\Codex Companion\' +
                    'mobile-nearby-tls-fallback.p12.dpapi'
                )
    }
}

function Get-CompanionPersistenceMarkers {
    return [ordered]@{
        prompt = 'PROMPT_CANARY_83A974D17F4B'
        credential =
            'sk-clean-vm-CREDENTIAL_CANARY_5E2B48C1A79D'
        attachment = 'ATTACHMENT_CANARY_39C68F2AD014'
        privateKey = 'PRIVATE_KEY_CANARY_B17F96E43C52'
    }
}

function Protect-CompanionFixtureBytes {
    param(
        [byte[]]$Plaintext,
        [string]$Entropy
    )

    Add-Type -AssemblyName System.Security
    return [System.Security.Cryptography.ProtectedData]::Protect(
        $Plaintext,
        [System.Text.Encoding]::UTF8.GetBytes($Entropy),
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser
    )
}

function Unprotect-CompanionFixtureBytes {
    param(
        [byte[]]$Ciphertext,
        [string]$Entropy
    )

    Add-Type -AssemblyName System.Security
    return [System.Security.Cryptography.ProtectedData]::Unprotect(
        $Ciphertext,
        [System.Text.Encoding]::UTF8.GetBytes($Entropy),
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser
    )
}

function Initialize-CompanionSqliteInterop {
    if ('CompanionCleanVmSqlite' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class CompanionCleanVmSqlite
{
    private const int SQLITE_OK = 0;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ExecCallback(
        IntPtr context,
        int columnCount,
        IntPtr values,
        IntPtr names);

    [DllImport(
        "winsqlite3.dll",
        CallingConvention = CallingConvention.Cdecl,
        CharSet = CharSet.Unicode)]
    private static extern int sqlite3_open16(
        [MarshalAs(UnmanagedType.LPWStr)] string path,
        out IntPtr database);

    [DllImport(
        "winsqlite3.dll",
        EntryPoint = "sqlite3_exec",
        CallingConvention = CallingConvention.Cdecl,
        CharSet = CharSet.Ansi)]
    private static extern int sqlite3_exec_no_callback(
        IntPtr database,
        [MarshalAs(UnmanagedType.LPStr)] string sql,
        IntPtr callback,
        IntPtr context,
        out IntPtr error);

    [DllImport(
        "winsqlite3.dll",
        EntryPoint = "sqlite3_exec",
        CallingConvention = CallingConvention.Cdecl,
        CharSet = CharSet.Ansi)]
    private static extern int sqlite3_exec_with_callback(
        IntPtr database,
        [MarshalAs(UnmanagedType.LPStr)] string sql,
        ExecCallback callback,
        IntPtr context,
        out IntPtr error);

    [DllImport(
        "winsqlite3.dll",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern int sqlite3_close_v2(
        IntPtr database);

    [DllImport(
        "winsqlite3.dll",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern void sqlite3_free(
        IntPtr value);

    [DllImport(
        "winsqlite3.dll",
        CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr sqlite3_errmsg(
        IntPtr database);

    private static IntPtr Open(string path)
    {
        IntPtr database;
        int code = sqlite3_open16(path, out database);
        if (code != SQLITE_OK || database == IntPtr.Zero)
        {
            string message = database == IntPtr.Zero
                ? "SQLite could not open the database."
                : Marshal.PtrToStringAnsi(
                    sqlite3_errmsg(database));
            if (database != IntPtr.Zero)
            {
                sqlite3_close_v2(database);
            }
            throw new InvalidOperationException(message);
        }
        return database;
    }

    private static void ThrowExecError(
        IntPtr database,
        int code,
        IntPtr error)
    {
        if (code == SQLITE_OK)
        {
            return;
        }
        string message = error != IntPtr.Zero
            ? Marshal.PtrToStringAnsi(error)
            : Marshal.PtrToStringAnsi(
                sqlite3_errmsg(database));
        if (error != IntPtr.Zero)
        {
            sqlite3_free(error);
        }
        throw new InvalidOperationException(message);
    }

    public static void Execute(
        string path,
        string sql)
    {
        IntPtr database = Open(path);
        try
        {
            IntPtr error;
            int code = sqlite3_exec_no_callback(
                database,
                sql,
                IntPtr.Zero,
                IntPtr.Zero,
                out error);
            ThrowExecError(database, code, error);
        }
        finally
        {
            sqlite3_close_v2(database);
        }
    }

    public static string ScalarText(
        string path,
        string sql)
    {
        IntPtr database = Open(path);
        try
        {
            string value = null;
            ExecCallback callback = delegate(
                IntPtr context,
                int columnCount,
                IntPtr values,
                IntPtr names)
            {
                if (columnCount > 0 && values != IntPtr.Zero)
                {
                    IntPtr item =
                        Marshal.ReadIntPtr(values, 0);
                    value = item == IntPtr.Zero
                        ? null
                        : Marshal.PtrToStringAnsi(item);
                }
                return 0;
            };
            IntPtr error;
            int code = sqlite3_exec_with_callback(
                database,
                sql,
                callback,
                IntPtr.Zero,
                out error);
            GC.KeepAlive(callback);
            ThrowExecError(database, code, error);
            return value;
        }
        finally
        {
            sqlite3_close_v2(database);
        }
    }
}
'@
}

function Initialize-CompanionGoalsFixture {
    param([string]$Path)

    Initialize-CompanionSqliteInterop
    $parent = Split-Path -Parent $Path
    New-Item `
        -ItemType Directory `
        -Path $parent `
        -Force | Out-Null
    [CompanionCleanVmSqlite]::Execute(
        $Path,
        @'
create table if not exists thread_goals (
    thread_id text primary key,
    goal_id text not null,
    objective text not null,
    status text not null,
    token_budget integer,
    tokens_used integer not null default 0,
    time_used_seconds integer not null default 0,
    created_at_ms integer not null,
    updated_at_ms integer not null
);
insert or replace into thread_goals (
    thread_id,
    goal_id,
    objective,
    status,
    token_budget,
    tokens_used,
    time_used_seconds,
    created_at_ms,
    updated_at_ms
) values (
    'clean-vm-thread',
    'clean-vm-goal',
    'Preserve Companion state across update',
    'paused',
    50000,
    1234,
    45,
    1785050000000,
    1785050000000
);
'@
    )
}

function Initialize-CompanionDurableStateFixture {
    $roots =
        Get-CompanionDurableStateRoots
    $markers =
        Get-CompanionPersistenceMarkers
    $credentialService =
        'companion.openai-api-key'
    $credentialPath =
        Join-Path `
            $roots.credentials `
            "$credentialService.bin"
    $privacyPaths = @(
        Join-Path $roots.privacy 'prompt.txt'
        Join-Path $roots.privacy 'attachment.bin'
        Join-Path $roots.privacy 'private-key.pem'
    )
    foreach ($path in @(
        $roots.settings
        $credentialPath
        $roots.pairings
    ) + $privacyPaths) {
        if (Test-Path -LiteralPath $path) {
            throw (
                'The clean-VM persistence seed target already exists: ' +
                [System.IO.Path]::GetFileName($path)
            )
        }
    }

    New-Item `
        -ItemType Directory `
        -Path (
            Split-Path -Parent $roots.settings
        ) `
        -Force | Out-Null
    [System.IO.File]::WriteAllText(
        $roots.settings,
        @'
[appearance]
backdrop=solid-black

[pet]
selectedId=fixture-preserved-pet
animationSpeedScale=1.15
animationSpeedTimingVersion=2
visible=false
hideControlsUntilHover=true
allowAutonomousMovement=false

[mobile]
enabled=false
keepAvailableWhileDisplayOff=false
allowNearbyOnPublicNetworks=false
relayMode=automatic
customRelayUrl=

[chat]
selectedModelId=lumo:automatic
'@,
        $utf8WithoutBom
    )

    New-Item `
        -ItemType Directory `
        -Path $roots.credentials `
        -Force | Out-Null
    $credentialPlaintext =
        [System.Text.Encoding]::UTF8.GetBytes(
            $markers.credential
        )
    try {
        $credentialCiphertext =
            Protect-CompanionFixtureBytes `
                -Plaintext $credentialPlaintext `
                -Entropy $credentialService
        [System.IO.File]::WriteAllBytes(
            $credentialPath,
            $credentialCiphertext
        )
    } finally {
        if ($null -ne $credentialPlaintext) {
            [Array]::Clear(
                $credentialPlaintext,
                0,
                $credentialPlaintext.Length
            )
        }
    }

    New-Item `
        -ItemType Directory `
        -Path (
            Split-Path -Parent $roots.pairings
        ) `
        -Force | Out-Null
    $pairingSource =
        [System.Text.Encoding]::UTF8.GetBytes(
            'PAIRING_FIXTURE_SECRET_1875BEA4'
        )
    $sha256 =
        [System.Security.Cryptography.SHA256]::Create()
    try {
        $pairingSecret =
            $sha256.ComputeHash($pairingSource)
    } finally {
        $sha256.Dispose()
        [Array]::Clear(
            $pairingSource,
            0,
            $pairingSource.Length
        )
    }
    try {
        $pairingCiphertext =
            Protect-CompanionFixtureBytes `
                -Plaintext $pairingSecret `
                -Entropy (
                    'Codex Companion paired-device secret v1'
                )
        Write-JsonAtomic `
            -Path $roots.pairings `
            -Value (
                [ordered]@{
                    version = 1
                    records = @(
                        [ordered]@{
                            deviceID =
                                'clean-vm-mobile-device'
                            displayName =
                                'Clean VM iPhone'
                            secretProtected =
                                [Convert]::ToBase64String(
                                    $pairingCiphertext
                                )
                            pairedAtMilliseconds =
                                1785050000000
                            relayURLString =
                                'https://relay.invalid/clean-vm'
                        }
                    )
                }
            )
    } finally {
        if ($null -ne $pairingSecret) {
            [Array]::Clear(
                $pairingSecret,
                0,
                $pairingSecret.Length
            )
        }
    }

    Initialize-CompanionGoalsFixture `
        -Path $roots.goals

    New-Item `
        -ItemType Directory `
        -Path $roots.privacy `
        -Force | Out-Null
    [System.IO.File]::WriteAllText(
        $privacyPaths[0],
        $markers.prompt,
        $utf8WithoutBom
    )
    $attachmentBytes =
        [System.Text.Encoding]::UTF8.GetBytes(
            "binary-prefix-$($markers.attachment)-binary-suffix"
        )
    [System.IO.File]::WriteAllBytes(
        $privacyPaths[1],
        $attachmentBytes
    )
    [System.IO.File]::WriteAllText(
        $privacyPaths[2],
        (
            "-----BEGIN PRIVATE KEY-----`n" +
            "$($markers.privateKey)`n" +
            '-----END PRIVATE KEY-----'
        ),
        $utf8WithoutBom
    )
}

function Get-CompanionSeededStateEvidence {
    $roots =
        Get-CompanionDurableStateRoots
    $markers =
        Get-CompanionPersistenceMarkers
    $settings = $false
    $petSelection = $false
    $credential = $false
    $pairings = $false
    $goals = $false
    $privacyCanaries = $false

    try {
        $settingsText =
            [System.IO.File]::ReadAllText(
                $roots.settings
            )
        $settings =
            $settingsText -cmatch '(?m)^backdrop=solid-black$' -and
            $settingsText -cmatch '(?m)^selectedModelId=lumo:automatic$' -and
            $settingsText -cmatch '(?m)^enabled=false$'
        $petSelection =
            $settingsText -cmatch '(?m)^selectedId=fixture-preserved-pet$'
    } catch {
    }

    try {
        $credentialService =
            'companion.openai-api-key'
        $credentialPath =
            Join-Path `
                $roots.credentials `
                "$credentialService.bin"
        $credentialPlaintext =
            Unprotect-CompanionFixtureBytes `
                -Ciphertext (
                    [System.IO.File]::ReadAllBytes(
                        $credentialPath
                    )
                ) `
                -Entropy $credentialService
        try {
            $credential =
                [System.Text.Encoding]::UTF8.GetString(
                    $credentialPlaintext
                ) -ceq $markers.credential
        } finally {
            [Array]::Clear(
                $credentialPlaintext,
                0,
                $credentialPlaintext.Length
            )
        }
    } catch {
    }

    try {
        $pairingDocument =
            Get-Content `
                -LiteralPath $roots.pairings `
                -Raw |
            ConvertFrom-Json
        $records =
            @($pairingDocument.records)
        if (
            [int]$pairingDocument.version -eq 1 -and
            $records.Count -eq 1 -and
            [string]$records[0].deviceID -ceq
                'clean-vm-mobile-device'
        ) {
            $pairingPlaintext =
                Unprotect-CompanionFixtureBytes `
                    -Ciphertext (
                        [Convert]::FromBase64String(
                            [string]$records[0].secretProtected
                        )
                    ) `
                    -Entropy (
                        'Codex Companion paired-device secret v1'
                    )
            try {
                $pairings =
                    $pairingPlaintext.Length -eq 32
            } finally {
                [Array]::Clear(
                    $pairingPlaintext,
                    0,
                    $pairingPlaintext.Length
                )
            }
        }
    } catch {
    }

    try {
        Initialize-CompanionSqliteInterop
        $goals =
            [CompanionCleanVmSqlite]::ScalarText(
                $roots.goals,
                (
                    'select objective from thread_goals ' +
                    "where thread_id='clean-vm-thread';"
                )
            ) -ceq
            'Preserve Companion state across update'
    } catch {
    }

    try {
        $privacyEvidence =
            Get-CompanionSensitiveLogEvidence `
                -Paths @(
                    Join-Path $roots.privacy 'prompt.txt'
                    Join-Path $roots.privacy 'attachment.bin'
                    Join-Path $roots.privacy 'private-key.pem'
                ) `
                -Markers $markers
        $privacyCanaries =
            @($privacyEvidence.missingFiles).Count -eq 0 -and
            @($privacyEvidence.unreadableFiles).Count -eq 0 -and
            @($privacyEvidence.matchedCategories).Count -eq 3 -and
            'prompt' -in $privacyEvidence.matchedCategories -and
            'attachment' -in $privacyEvidence.matchedCategories -and
            'privateKey' -in $privacyEvidence.matchedCategories
    } catch {
    }

    return [ordered]@{
        settings = $settings
        petSelection = $petSelection
        credential = $credential
        pairings = $pairings
        goals = $goals
        privacyCanaries = $privacyCanaries
    }
}

function Get-UserDataSnapshot {
    return @(
        Get-CompanionDurableStateSnapshot `
            -Roots (
                Get-CompanionDurableStateRoots
            )
    )
}

function Stop-CompanionProcesses {
    foreach ($process in @(
        Get-CimInstance Win32_Process |
            Where-Object {
                $_.Name -in @(
                    'CodexCompanion.exe'
                    'CodexCompanionUpdater.exe'
                )
            }
    )) {
        Stop-Process `
            -Id $process.ProcessId `
            -Force `
            -ErrorAction SilentlyContinue
    }
}

if ($PersistenceProbe) {
    Initialize-CompanionDurableStateFixture
    $probeState =
        Get-CompanionSeededStateEvidence
    $probeSnapshot = @(
        Get-UserDataSnapshot
    )
    $requiredRoots = @(
        'settings'
        'credentials'
        'pairings'
        'goals'
        'privacy'
    )
    $probePreserved =
        Test-CompanionDurableStatePreserved `
            -Baseline $probeSnapshot `
            -Current $probeSnapshot `
            -RequiredRoots $requiredRoots
    $probePassed =
        $probePreserved -and
        @(
            $probeState.Keys |
                Where-Object {
                    -not [bool]$probeState[$_]
                }
        ).Count -eq 0
    $probeReport = [ordered]@{
        passed = $probePassed
        semantic = $probeState
        roots = @(
            $probeSnapshot.root |
                Sort-Object -Unique
        )
        fileCount = $probeSnapshot.Count
    }
    Write-JsonAtomic `
        -Path (
            Join-Path `
                $EvidenceRoot `
                'persistence-probe.json'
        ) `
        -Value $probeReport
    $probeReport |
        ConvertTo-Json -Depth 6 -Compress
    if (-not $probePassed) {
        exit 2
    }
    exit 0
}

function Invoke-AutomationElement {
    param(
        [IntPtr]$RootWindow,
        [string]$Name,
        [int]$Seconds
    )

    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $deadline =
        [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $root =
            [System.Windows.Automation.AutomationElement]::
                FromHandle($RootWindow)
        $condition =
            New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::
                    NameProperty,
                $Name
            )
        $elements = $root.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants,
            $condition
        )
        foreach ($element in $elements) {
            $pattern = $null
            if (
                $element.TryGetCurrentPattern(
                    [System.Windows.Automation.InvokePattern]::
                        Pattern,
                    [ref]$pattern
                )
            ) {
                $pattern.Invoke()
                return $true
            }
            if (
                $element.TryGetCurrentPattern(
                    [System.Windows.Automation.SelectionItemPattern]::
                        Pattern,
                    [ref]$pattern
                )
            ) {
                $pattern.Select()
                return $true
            }
        }
        Start-Sleep -Milliseconds 300
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

$identity =
    [Security.Principal.WindowsIdentity]::GetCurrent()
$principal =
    New-Object Security.Principal.WindowsPrincipal($identity)
$isAdministrator =
    $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator
    )
Add-Check `
    -Id 'guest.standard_user' `
    -Passed (-not $isAdministrator) `
    -Detail $identity.Name

$os = Get-CimInstance Win32_OperatingSystem
$osSupported =
    [int]$os.BuildNumber -ge 22000 -and
    [string]$env:PROCESSOR_ARCHITECTURE -eq 'AMD64'
Add-Check `
    -Id 'guest.windows_11_x64' `
    -Passed $osSupported `
    -Detail (
        "$($os.Caption) build $($os.BuildNumber) " +
        "$env:PROCESSOR_ARCHITECTURE"
    )

$appPath = Join-Path $InstallRoot 'bin\CodexCompanion.exe'
$updaterPath =
    Join-Path $InstallRoot 'bin\CodexCompanionUpdater.exe'
$uninstallerPath = Join-Path $InstallRoot 'unins000.exe'
$shortcutPath = Join-Path `
    $env:APPDATA `
    'Microsoft\Windows\Start Menu\Programs\Codex Companion\Codex Companion.lnk'
$uninstallRegistryPath =
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
    '{9B3C42CB-4B7F-4A08-B675-071708948C88}_is1'

$sourceSha256 = ''
if (
    -not [string]::IsNullOrWhiteSpace($SourceInstallerPath) -and
    (Test-Path -LiteralPath $SourceInstallerPath -PathType Leaf)
) {
    $sourceSha256 = (
        Get-FileHash `
            -LiteralPath $SourceInstallerPath `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $sourceMatches =
        [string]::IsNullOrWhiteSpace($ExpectedSourceSha256) -or
        $sourceSha256 -eq
            $ExpectedSourceSha256.ToLowerInvariant()
    Add-Check `
        -Id 'installer.source_sha256' `
        -Passed $sourceMatches `
        -Detail $sourceSha256
}

$windowEvidence = @()
$trayEvidence = $null
$signatureEvidence = @()
$shortcutEvidence = $null
$executableIconEvidence = $null
$versionEvidence = $null
$updateEvidence = $null
$seededStateEvidence = $null
$sensitiveLogEvidence = $null
$userData = @()
$screenshotPath = ''
$primaryProcessId = 0

try {
    if ($Mode -in @('Installed', 'Update')) {
        $installParent =
            Split-Path -Parent $InstallRoot
        $installName =
            Split-Path -Leaf $InstallRoot
        $rollbackDirectories = @(
            if (
                Test-Path `
                    -LiteralPath $installParent `
                    -PathType Container
            ) {
                Get-ChildItem `
                    -LiteralPath $installParent `
                    -Directory `
                    -Filter (
                        $installName + '.rollback.*'
                    ) `
                    -Force
            }
        )
        Add-Check `
            -Id 'installed.no_rollback_directories' `
            -Passed (
                $rollbackDirectories.Count -eq 0
            ) `
            -Detail (
                "$($rollbackDirectories.Count) rollback directories remain"
            )

        if ($SeedDurableState) {
            Initialize-CompanionDurableStateFixture
            Add-Check `
                -Id 'user_data.seeded_fixture' `
                -Passed $true `
                -Detail (
                    'Seeded settings, pet selection, credential, ' +
                    'pairing, goal, and privacy canaries.'
                )
        }

        foreach ($path in @(
            $appPath
            $updaterPath
            $uninstallerPath
        )) {
            Add-Check `
                -Id (
                    'installed.file.' +
                    [System.IO.Path]::GetFileName($path)
                ) `
                -Passed (
                    Test-Path -LiteralPath $path -PathType Leaf
                ) `
                -Detail $path
        }

        if (Test-Path -LiteralPath $appPath -PathType Leaf) {
            $versionInfo =
                (Get-Item -LiteralPath $appPath).VersionInfo
            $versionEvidence = [ordered]@{
                productVersion =
                    [string]$versionInfo.ProductVersion
                fileVersion =
                    [string]$versionInfo.FileVersion
            }
            if ($Mode -eq 'Installed') {
                $versionPassed =
                    [string]::IsNullOrWhiteSpace($ExpectedVersion) -or
                    (
                        Test-CompanionVersionIdentity `
                            -ProductVersion (
                                $versionEvidence.productVersion
                            ) `
                            -FileVersion (
                                $versionEvidence.fileVersion
                            ) `
                            -ExpectedVersion $ExpectedVersion `
                            -ExpectedBuild $ExpectedBuild
                    )
                Add-Check `
                    -Id 'installed.version' `
                    -Passed $versionPassed `
                    -Detail (
                        "$($versionEvidence.productVersion) / " +
                        $versionEvidence.fileVersion
                    )
            } elseif ($Mode -eq 'Update') {
                $initialVersionPassed =
                    Test-CompanionVersionIdentity `
                        -ProductVersion (
                            $versionEvidence.productVersion
                        ) `
                        -FileVersion (
                            $versionEvidence.fileVersion
                        ) `
                        -ExpectedVersion (
                            $InitialExpectedVersion
                        ) `
                        -ExpectedBuild (
                            $InitialExpectedBuild
                        )
                Add-Check `
                    -Id 'update.initial_version' `
                    -Passed $initialVersionPassed `
                    -Detail (
                        "$($versionEvidence.productVersion) / " +
                        "$($versionEvidence.fileVersion), expected " +
                        "$InitialExpectedVersion.$InitialExpectedBuild"
                    )
            }
        }

        foreach ($path in @(
            $appPath
            $updaterPath
            $uninstallerPath
        )) {
            $signature =
                Get-SignatureEvidence -Path $path
            $signatureEvidence += $signature
            $expectedSigner =
                ([string]$ExpectedSignerSha256).
                    Replace(' ', '').
                    ToLowerInvariant()
            $signaturePassed =
                $signature.status -eq 'Valid' -and
                (
                    [string]::IsNullOrWhiteSpace($expectedSigner) -or
                    $signature.signerSha256 -eq $expectedSigner
                )
            Add-Check `
                -Id (
                    'installed.signature.' +
                    [System.IO.Path]::GetFileName($path)
                ) `
                -Passed $signaturePassed `
                -Detail (
                    "$($signature.status) " +
                    $signature.signerSha256
                )
        }

        $shortcutFound =
            Test-Path -LiteralPath $shortcutPath -PathType Leaf
        $shortcutTargetPassed = $false
        $shortcutIconPassed = $false
        if ($shortcutFound) {
            $shell =
                New-Object -ComObject WScript.Shell
            $shortcut =
                $shell.CreateShortcut($shortcutPath)
            $shortcutEvidence = [ordered]@{
                path = 'Start Menu\Programs\Codex Companion\Codex Companion.lnk'
                target = [string]$shortcut.TargetPath
                iconLocation = [string]$shortcut.IconLocation
                workingDirectory = [string]$shortcut.WorkingDirectory
            }
            $shortcutTargetPassed =
                Test-CompanionPathEqual `
                    -Left $shortcut.TargetPath `
                    -Right $appPath
            $shortcutIconPassed =
                Test-CompanionShortcutIdentity `
                    -TargetPath $shortcut.TargetPath `
                    -IconLocation $shortcut.IconLocation `
                    -WorkingDirectory $shortcut.WorkingDirectory `
                    -ExecutablePath $appPath
        }
        Add-Check `
            -Id 'installed.start_menu' `
            -Passed $shortcutTargetPassed `
            -Detail 'Start Menu shortcut targets CodexCompanion.exe'
        Add-Check `
            -Id 'installed.start_menu_icon' `
            -Passed $shortcutIconPassed `
            -Detail (
                'Start Menu shortcut uses the installed executable, ' +
                'working directory, and icon index 0.'
            )

        $executableIconPath =
            Join-Path $EvidenceRoot 'executable-icon.png'
        $executableIconPassed = $false
        $executableIconEvidence = [ordered]@{
            file =
                [System.IO.Path]::GetFileName(
                    $executableIconPath
                )
            width = 0
            height = 0
            sha256 = ''
            error = ''
        }
        $executableIcon = $null
        $executableIconBitmap = $null
        try {
            Add-Type -AssemblyName System.Drawing
            $executableIcon =
                [System.Drawing.Icon]::ExtractAssociatedIcon(
                    $appPath
                )
            if ($null -ne $executableIcon) {
                $executableIconBitmap =
                    $executableIcon.ToBitmap()
                $executableIconBitmap.Save(
                    $executableIconPath,
                    [System.Drawing.Imaging.ImageFormat]::Png
                )
                $executableIconEvidence.width =
                    [int]$executableIcon.Width
                $executableIconEvidence.height =
                    [int]$executableIcon.Height
                if (
                    Test-Path `
                        -LiteralPath $executableIconPath `
                        -PathType Leaf
                ) {
                    $executableIconEvidence.sha256 =
                        (
                            Get-FileHash `
                                -LiteralPath $executableIconPath `
                                -Algorithm SHA256
                        ).Hash.ToLowerInvariant()
                    $executableIconPassed =
                        $executableIconEvidence.width -gt 0 -and
                        $executableIconEvidence.height -gt 0 -and
                        (
                            Get-Item `
                                -LiteralPath $executableIconPath
                        ).Length -gt 0
                }
            }
        } catch {
            $executableIconEvidence.error =
                $_.Exception.Message
        } finally {
            if ($null -ne $executableIconBitmap) {
                $executableIconBitmap.Dispose()
            }
            if ($null -ne $executableIcon) {
                $executableIcon.Dispose()
            }
        }
        Add-Check `
            -Id 'installed.executable_icon' `
            -Passed $executableIconPassed `
            -Detail (
                "$($executableIconEvidence.width)x" +
                "$($executableIconEvidence.height) " +
                $executableIconEvidence.file
            )
        Add-Check `
            -Id 'installed.uninstall_registration' `
            -Passed (
                Test-Path -LiteralPath $uninstallRegistryPath
            ) `
            -Detail $uninstallRegistryPath

        if (Test-Path -LiteralPath $appPath -PathType Leaf) {
            Start-CompanionProcess `
                -ExecutablePath $appPath `
                -LaunchMode $Mode `
                -FeedUrl $UpdateFeedUrl
            $processes = Wait-Until `
                -Seconds $TimeoutSeconds `
                -Condition {
                    $exact =
                        Get-ExactCompanionProcesses `
                            -ExecutablePath $appPath
                    if ($exact.Count -eq 1) {
                        return $exact
                    }
                    return $null
                }
            $processStarted =
                $null -ne $processes -and
                @($processes).Count -eq 1
            Add-Check `
                -Id 'runtime.single_process' `
                -Passed $processStarted `
                -Detail (
                    if ($processStarted) {
                        "PID $($processes[0].ProcessId)"
                    } else {
                        'Expected exactly one installed Companion process.'
                    }
                )

            if ($processStarted) {
                $primaryProcessId =
                    [int]$processes[0].ProcessId
                $updaterProcesses = @(
                    Get-CimInstance `
                        Win32_Process `
                        -Filter (
                            "Name='CodexCompanionUpdater.exe'"
                        ) `
                        -ErrorAction SilentlyContinue
                )
                Add-Check `
                    -Id 'runtime.no_updater_helpers' `
                    -Passed (
                        $updaterProcesses.Count -eq 0
                    ) `
                    -Detail (
                        "$($updaterProcesses.Count) updater helpers remain"
                    )
                $settingsWindow = Wait-Until `
                    -Seconds $TimeoutSeconds `
                    -Condition {
                        $windows =
                            Get-WindowEvidence `
                                -ProcessIds @($primaryProcessId)
                        return @(
                            $windows |
                                Where-Object {
                                    $_.visible -and
                                    $_.title -eq
                                        'Codex Companion Settings'
                                }
                        ) |
                            Select-Object -First 1
                    }
                $settingsPassed =
                    -not $RequireSettingsVisible -or
                    $null -ne $settingsWindow
                Add-Check `
                    -Id 'runtime.settings_visible' `
                    -Passed $settingsPassed `
                    -Detail 'Codex Companion Settings'

                $windowEvidence =
                    Get-WindowEvidence `
                        -ProcessIds @($primaryProcessId)
                $taskbarCandidates = @(
                    $windowEvidence |
                        Where-Object { $_.taskbarCandidate }
                )
                $altTabCandidates = @(
                    $windowEvidence |
                        Where-Object { $_.altTabCandidate }
                )
                Add-Check `
                    -Id 'runtime.no_taskbar_window' `
                    -Passed ($taskbarCandidates.Count -eq 0) `
                    -Detail (
                        "$($taskbarCandidates.Count) taskbarCandidate windows"
                    )
                Add-Check `
                    -Id 'runtime.no_alt_tab_window' `
                    -Passed ($altTabCandidates.Count -eq 0) `
                    -Detail (
                        "$($altTabCandidates.Count) altTabCandidate windows"
                    )

                $trayEvidence = Get-TrayEvidence
                Add-Check `
                    -Id 'runtime.notification_area_icon' `
                    -Passed ([bool]$trayEvidence.present) `
                    -Detail (
                        "Shell_NotifyIconGetRect HRESULT " +
                        $trayEvidence.hresult
                    )

                $screenshotPath = Join-Path `
                    $EvidenceRoot `
                    "desktop-$Mode-$DpiPercent.png"
                Save-DesktopScreenshot `
                    -Path $screenshotPath
                Add-Check `
                    -Id 'evidence.desktop_screenshot' `
                    -Passed (
                        Test-Path -LiteralPath $screenshotPath -PathType Leaf
                    ) `
                    -Detail (
                        [System.IO.Path]::GetFileName($screenshotPath)
                    )

                if (
                    $DriveUpdateUi -and
                    $null -ne $settingsWindow
                ) {
                    $updaterTransactionRoot =
                        Join-Path `
                            ([System.IO.Path]::GetTempPath()) `
                            'CodexCompanionUpdater'
                    $transactionsBeforeUpdate = @(
                        Get-CompanionUpdaterTransactionRoots `
                            -UpdaterRoot $updaterTransactionRoot
                    )
                    $preUpdateProcessId = $primaryProcessId
                    $settingsHwnd =
                        [IntPtr][int64]$settingsWindow.hwnd
                    $updatesSelected =
                        Invoke-AutomationElement `
                            -RootWindow $settingsHwnd `
                            -Name 'Updates' `
                            -Seconds 20
                    $checkInvoked =
                        $updatesSelected -and
                        (
                            Invoke-AutomationElement `
                                -RootWindow $settingsHwnd `
                                -Name 'Check for Updates' `
                                -Seconds 20
                        )
                    $downloadInvoked =
                        $checkInvoked -and
                        (
                            Invoke-AutomationElement `
                                -RootWindow $settingsHwnd `
                                -Name 'Download Verified Update' `
                                -Seconds $TimeoutSeconds
                        )
                    $installInvoked =
                        $downloadInvoked -and
                        (
                            Invoke-AutomationElement `
                                -RootWindow $settingsHwnd `
                                -Name 'Install and Relaunch' `
                                -Seconds (
                                    [Math]::Max(
                                        $TimeoutSeconds,
                                        600
                                    )
                                )
                        )
                    Add-Check `
                        -Id 'update.ui_state_machine' `
                        -Passed $installInvoked `
                        -Detail (
                            'Updates -> Check for Updates -> ' +
                            'Download Verified Update -> Install and Relaunch'
                        )
                    if ($installInvoked) {
                        $updateTransaction = Wait-Until `
                            -Seconds 60 `
                            -PollMilliseconds 250 `
                            -Condition {
                                Find-CompanionUpdateTransaction `
                                    -UpdaterRoot (
                                        $updaterTransactionRoot
                                    ) `
                                    -ExcludedRoots (
                                        $transactionsBeforeUpdate
                                    ) `
                                    -ExpectedVersion (
                                        $ExpectedVersion
                                    ) `
                                    -ExpectedBuild $ExpectedBuild `
                                    -InstallRoot $InstallRoot
                            }
                        Add-Check `
                            -Id 'update.transaction_request' `
                            -Passed (
                                $null -ne $updateTransaction
                            ) `
                            -Detail (
                                if ($null -ne $updateTransaction) {
                                    $updateTransaction.root
                                } else {
                                    'No matching new updater transaction was found.'
                                }
                            )

                        if ($null -ne $updateTransaction) {
                            $completion = Wait-Until `
                                -Seconds 600 `
                                -PollMilliseconds 1000 `
                                -Condition {
                                    $current =
                                        Get-CompanionUpdateCompletionEvidence `
                                            -Transaction (
                                                $updateTransaction
                                            ) `
                                            -ExecutablePath $appPath `
                                            -PreviousProcessId (
                                                $preUpdateProcessId
                                            ) `
                                            -ExpectedVersion (
                                                $ExpectedVersion
                                            ) `
                                            -ExpectedBuild (
                                                $ExpectedBuild
                                            )
                                    if ($current.complete) {
                                        return $current
                                    }
                                    return $null
                                }
                            if ($null -eq $completion) {
                                $completion =
                                    Get-CompanionUpdateCompletionEvidence `
                                        -Transaction (
                                            $updateTransaction
                                        ) `
                                        -ExecutablePath $appPath `
                                        -PreviousProcessId (
                                            $preUpdateProcessId
                                        ) `
                                        -ExpectedVersion (
                                            $ExpectedVersion
                                        ) `
                                        -ExpectedBuild (
                                            $ExpectedBuild
                                        )
                            }
                            $updateEvidence = [ordered]@{
                                transactionRoot =
                                    $updateTransaction.root
                                request =
                                    $updateTransaction.request
                                completion = $completion
                            }
                            $versionEvidence = [ordered]@{
                                productVersion =
                                    $completion.productVersion
                                fileVersion =
                                    $completion.fileVersion
                            }
                            $primaryProcessId =
                                [int]$completion.processId

                            Add-Check `
                                -Id 'update.installed_expected_version' `
                                -Passed (
                                    [bool]$completion.versionIdentity
                                ) `
                                -Detail (
                                    "$($completion.productVersion) / " +
                                    $completion.fileVersion
                                )
                            Add-Check `
                                -Id 'update.result_acknowledged' `
                                -Passed (
                                    [bool]$completion.resultValid
                                ) `
                                -Detail (
                                    $updateTransaction.resultPath
                                )
                            Add-Check `
                                -Id 'update.helper_exited' `
                                -Passed (
                                    [bool]$completion.helperExited
                                ) `
                                -Detail (
                                    "$(@($completion.helperProcessIds).Count) " +
                                    'matching updater helpers remain'
                                )
                            Add-Check `
                                -Id 'update.rollback_removed' `
                                -Passed (
                                    [bool]$completion.rollbackRemoved
                                ) `
                                -Detail (
                                    $completion.rollbackRoot
                                )
                            Add-Check `
                                -Id 'update.relaunched_process' `
                                -Passed (
                                    [bool]$completion.relaunchedProcess
                                ) `
                                -Detail (
                                    "old PID $preUpdateProcessId, " +
                                    "new PID $($completion.processId), " +
                                    "$($completion.exactProcessCount) exact processes"
                                )
                            Add-Check `
                                -Id 'update.tray_recovered' `
                                -Passed (
                                    [bool]$completion.tray.present
                                ) `
                                -Detail (
                                    'Shell_NotifyIconGetRect HRESULT ' +
                                    $completion.tray.hresult
                                )
                            $trayEvidence = $completion.tray
                        } else {
                            $updateEvidence = [ordered]@{
                                transactionRoot = ''
                                request = $null
                                completion = $null
                            }
                            foreach ($missingCheck in @(
                                'update.installed_expected_version'
                                'update.result_acknowledged'
                                'update.helper_exited'
                                'update.rollback_removed'
                                'update.relaunched_process'
                                'update.tray_recovered'
                            )) {
                                Add-Check `
                                    -Id $missingCheck `
                                    -Passed $false `
                                    -Detail (
                                        'A matching updater transaction was not available.'
                                    )
                            }
                        }
                    }
                } elseif ($null -ne $settingsWindow) {
                    $originalSettingsHwnd =
                        [int64]$settingsWindow.hwnd
                    $settingsHwnd =
                        [IntPtr]$originalSettingsHwnd
                    [void][CompanionCleanVmNative]::PostMessageW(
                        $settingsHwnd,
                        [CompanionCleanVmNative]::WM_CLOSE,
                        [IntPtr]::Zero,
                        [IntPtr]::Zero
                    )
                    $settingsClosed = Wait-Until `
                        -Seconds 15 `
                        -Condition {
                            $windows =
                                Get-WindowEvidence `
                                    -ProcessIds @($primaryProcessId)
                            return -not (
                                $windows |
                                    Where-Object {
                                        $_.visible -and
                                        $_.title -eq
                                            'Codex Companion Settings'
                                    } |
                                    Select-Object -First 1
                            )
                        }
                    $processRemains =
                        @(
                            Get-ExactCompanionProcesses `
                                -ExecutablePath $appPath
                        ).Count -eq 1
                    Add-Check `
                        -Id 'runtime.settings_close_only' `
                        -Passed (
                            [bool]$settingsClosed -and
                            $processRemains
                        ) `
                        -Detail (
                            'WM_CLOSE hides Settings while the tray process remains.'
                        )

                    Start-CompanionProcess `
                        -ExecutablePath $appPath `
                        -LaunchMode 'Installed' `
                        -FeedUrl ''
                    $reactivated = Wait-Until `
                        -Seconds $TimeoutSeconds `
                        -Condition {
                            $processCount = @(
                                Get-ExactCompanionProcesses `
                                    -ExecutablePath $appPath
                            ).Count
                            $windows =
                                Get-WindowEvidence `
                                    -ProcessIds @($primaryProcessId)
                            $settings = $windows |
                                Where-Object {
                                    $_.visible -and
                                    $_.title -eq
                                        'Codex Companion Settings'
                                } |
                                Select-Object -First 1
                            if (
                                $processCount -eq 1 -and
                                $null -ne $settings
                            ) {
                                return $settings
                            }
                            return $null
                        }
                    Add-Check `
                        -Id 'runtime.second_launch_activates_settings' `
                        -Passed ($null -ne $reactivated) `
                        -Detail (
                            'Second launch reuses one process and reopens Settings.'
                        )
                    $reactivatedSettingsHwnd =
                        if ($null -eq $reactivated) {
                            [int64]0
                        } else {
                            [int64]$reactivated.hwnd
                        }
                    Add-Check `
                        -Id 'runtime.second_launch_reuses_settings_hwnd' `
                        -Passed (
                            $originalSettingsHwnd -ne 0 -and
                            $reactivatedSettingsHwnd -eq
                                $originalSettingsHwnd
                        ) `
                        -Detail (
                            "original HWND $originalSettingsHwnd, " +
                            "reactivated HWND $reactivatedSettingsHwnd"
                        )
                }
            }
        }
    } else {
        Add-Check `
            -Id 'uninstall.install_root_removed' `
            -Passed (
                -not (Test-Path -LiteralPath $InstallRoot)
            ) `
            -Detail $InstallRoot
        Add-Check `
            -Id 'uninstall.start_menu_removed' `
            -Passed (
                -not (Test-Path -LiteralPath $shortcutPath)
            ) `
            -Detail 'Start Menu shortcut removed'
        Add-Check `
            -Id 'uninstall.registration_removed' `
            -Passed (
                -not (Test-Path -LiteralPath $uninstallRegistryPath)
            ) `
            -Detail $uninstallRegistryPath
        $remainingProcesses = @(
            Get-CimInstance Win32_Process |
                Where-Object {
                    $_.Name -in @(
                        'CodexCompanion.exe'
                        'CodexCompanionUpdater.exe'
                    )
                }
        )
        Add-Check `
            -Id 'uninstall.no_processes' `
            -Passed ($remainingProcesses.Count -eq 0) `
            -Detail (
                "$($remainingProcesses.Count) Companion processes remain"
            )
    }

    if (-not $LeaveRunning) {
        Stop-CompanionProcesses
        Start-Sleep -Milliseconds 500
    }

    if ($RequireSeededDurableState) {
        $seededStateEvidence =
            Get-CompanionSeededStateEvidence
        foreach ($field in @(
            'settings'
            'petSelection'
            'credential'
            'pairings'
            'goals'
            'privacyCanaries'
        )) {
            Add-Check `
                -Id (
                    'user_data.seeded_' +
                    $field
                ) `
                -Passed (
                    [bool]$seededStateEvidence[$field]
                ) `
                -Detail (
                    "$field persisted in its native format."
                )
        }
    }

    $userData = @(
        Get-UserDataSnapshot
    )
    Write-JsonAtomic `
        -Path (
            Join-Path $EvidenceRoot 'user-data-snapshot.json'
        ) `
        -Value $userData
    if ($RequireUserData) {
        Add-Check `
            -Id 'user_data.present' `
            -Passed ($userData.Count -gt 0) `
            -Detail "$($userData.Count) hashed user-data files"
    }
    if (
        -not [string]::IsNullOrWhiteSpace($BaselineUserDataPath)
    ) {
        $baselineUserData = @(
            Get-Content `
                -LiteralPath $BaselineUserDataPath `
                -Raw |
            ConvertFrom-Json
        )
        $requiredRoots = if (
            $RequireSeededDurableState
        ) {
            @(
                'settings'
                'credentials'
                'pairings'
                'goals'
                'privacy'
            )
        } else {
            @()
        }
        Add-Check `
            -Id 'user_data.preserved' `
            -Passed (
                Test-CompanionDurableStatePreserved `
                    -Baseline $baselineUserData `
                    -Current $userData `
                    -RequiredRoots $requiredRoots
            ) `
            -Detail 'Baseline user-data hashes remain present.'
    }

    if ($RequireSensitiveLogClean) {
        $installerLogPath = ''
        if (
            $null -ne $updateEvidence -and
            $null -ne $updateEvidence.completion -and
            $null -ne $updateEvidence.completion.result
        ) {
            $installerLogPath =
                [string](
                    $updateEvidence.completion.result.installerLogPath
                )
        }
        $sensitiveLogEvidence =
            Get-CompanionSensitiveLogEvidence `
                -Paths @($installerLogPath) `
                -Markers (
                    Get-CompanionPersistenceMarkers
                )
        Write-JsonAtomic `
            -Path (
                Join-Path `
                    $EvidenceRoot `
                    'sensitive-log-evidence.json'
            ) `
            -Value $sensitiveLogEvidence
        $sensitiveDetail = if (
            [bool]$sensitiveLogEvidence.clean
        ) {
            (
                "$($sensitiveLogEvidence.scannedFileCount) " +
                'updater log scanned; no sensitive canaries found.'
            )
        } else {
            (
                'matched categories: ' +
                (
                    @(
                        $sensitiveLogEvidence.matchedCategories
                    ) -join ', '
                ) +
                '; missing files: ' +
                (
                    @(
                        $sensitiveLogEvidence.missingFiles
                    ) -join ', '
                ) +
                '; unreadable files: ' +
                (
                    @(
                        $sensitiveLogEvidence.unreadableFiles
                    ) -join ', '
                )
            )
        }
        Add-Check `
            -Id 'update.sensitive_log_clean' `
            -Passed (
                [bool]$sensitiveLogEvidence.clean
            ) `
            -Detail $sensitiveDetail
    }
} catch {
    Add-Check `
        -Id 'verification.unhandled_exception' `
        -Passed $false `
        -Detail $_.Exception.Message
} finally {
    if (-not $LeaveRunning) {
        Stop-CompanionProcesses
    }
}

$passed =
    @($checks | Where-Object { -not $_.passed }).Count -eq 0
$report = [ordered]@{
    schemaVersion = 1
    passed = $passed
    mode = $Mode
    dpiPercent = $DpiPercent
    recordedAt = [DateTime]::UtcNow.ToString(
        'yyyy-MM-ddTHH:mm:ssZ',
        [Globalization.CultureInfo]::InvariantCulture
    )
    identity = $identity.Name
    os = [ordered]@{
        caption = [string]$os.Caption
        build = [int]$os.BuildNumber
        architecture = [string]$env:PROCESSOR_ARCHITECTURE
    }
    sourceSha256 = $sourceSha256
    expectedVersion = $ExpectedVersion
    expectedBuild = $ExpectedBuild
    initialExpectedVersion = $InitialExpectedVersion
    initialExpectedBuild = $InitialExpectedBuild
    updateFeedUrl = $UpdateFeedUrl
    version = $versionEvidence
    signatures = $signatureEvidence
    shortcut = $shortcutEvidence
    executableIcon = $executableIconEvidence
    tray = $trayEvidence
    update = $updateEvidence
    seededState = $seededStateEvidence
    sensitiveLog = $sensitiveLogEvidence
    windows = $windowEvidence
    screenshot = if (
        [string]::IsNullOrWhiteSpace($screenshotPath)
    ) {
        ''
    } else {
        [System.IO.Path]::GetFileName($screenshotPath)
    }
    userDataFileCount = $userData.Count
    checks = @($checks)
}
$reportPath = Join-Path `
    $EvidenceRoot `
    "verification-$Mode-$DpiPercent.json"
Write-JsonAtomic `
    -Path $reportPath `
    -Value $report
Write-JsonAtomic `
    -Path (
        Join-Path $EvidenceRoot 'complete.json'
    ) `
    -Value $report

if (-not $passed) {
    exit 2
}
