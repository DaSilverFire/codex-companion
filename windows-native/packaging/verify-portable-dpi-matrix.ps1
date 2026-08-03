[CmdletBinding()]
param(
    [string]$ExecutablePath,

    [string]$WindowVerifierPath,

    [string]$NativeCommandPath,

    [string]$EvidenceRoot,

    [ValidateSet(100, 125, 150, 200)]
    [int[]]$DpiPercent = @(100, 125, 150, 200),

    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 45,

    [switch]$TemporarilyStopRunningInstance,

    [switch]$ContractProbe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($ContractProbe) {
    [ordered]@{
        schemaVersion = 1
        scalePercent = @(100, 125, 150, 200)
        surfaces = @(
            'settings'
            'pet'
            'processes'
            'chat'
        )
        startupRoutes = @(
            'none'
            'processes'
            'local-chat'
        )
        physicalCursorHover = $true
        restoresCursor = $true
        isolatedProfile = $true
        restoresPreviousInstance = $true
    } |
        ConvertTo-Json -Compress
    exit 0
}

function Resolve-RequiredFile {
    param(
        [string]$Name,
        [string]$Path
    )

    if (
        [string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)
    ) {
        throw "$Name was not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Resolve-RequiredDirectory {
    param(
        [string]$Name,
        [string]$Path
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name is required."
    }
    $resolved = [System.IO.Path]::GetFullPath($Path)
    New-Item -ItemType Directory -Path $resolved -Force |
        Out-Null
    return $resolved
}

function Write-JsonAtomic {
    param(
        [string]$Path,
        [object]$Value
    )

    $encoding =
        New-Object System.Text.UTF8Encoding($false)
    $temporaryPath = "$Path.partial"
    [System.IO.File]::WriteAllText(
        $temporaryPath,
        (($Value | ConvertTo-Json -Depth 12) + "`n"),
        $encoding
    )
    Move-Item `
        -LiteralPath $temporaryPath `
        -Destination $Path `
        -Force
}

function Wait-Until {
    param(
        [scriptblock]$Condition,
        [int]$Seconds = $TimeoutSeconds,
        [int]$PollMilliseconds = 200
    )

    $deadline =
        [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $value = & $Condition
        if ($null -ne $value) {
            return $value
        }
        Start-Sleep -Milliseconds $PollMilliseconds
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Test-PathInsideRoot {
    param(
        [string]$Path,
        [string]$Root
    )

    $resolvedPath =
        [System.IO.Path]::GetFullPath($Path).
            TrimEnd('\')
    $resolvedRoot =
        [System.IO.Path]::GetFullPath($Root).
            TrimEnd('\')
    return $resolvedPath.StartsWith(
        $resolvedRoot + '\',
        [System.StringComparison]::OrdinalIgnoreCase
    )
}

function Remove-IsolatedProfile {
    param(
        [string]$Path,
        [string]$Root
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if (-not (Test-PathInsideRoot -Path $Path -Root $Root)) {
        throw "The isolated profile escaped the evidence root: $Path"
    }
    Remove-Item `
        -LiteralPath $Path `
        -Recurse `
        -Force
}

function Get-QtTestStatePaths {
    $localData =
        [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::LocalApplicationData)
    $roamingData =
        [Environment]::GetFolderPath(
            [Environment+SpecialFolder]::ApplicationData)
    $roots = @(
        (Join-Path $localData 'qttest')
        (Join-Path $roamingData 'qttest')
    )
    $relativePaths = @(
        (Join-Path 'DaSilverFire' 'Codex Companion')
        'Codex Companion'
    )

    foreach ($root in $roots) {
        foreach ($relativePath in $relativePaths) {
            [pscustomobject]@{
                Root =
                    [System.IO.Path]::GetFullPath($root)
                Path =
                    [System.IO.Path]::GetFullPath(
                        (Join-Path $root $relativePath))
            }
        }
    }
}

function Remove-QtTestState {
    foreach ($entry in @(Get-QtTestStatePaths)) {
        if (-not (Test-Path -LiteralPath $entry.Path)) {
            continue
        }
        if (
            -not (
                Test-PathInsideRoot `
                    -Path $entry.Path `
                    -Root $entry.Root
            )
        ) {
            throw "The Qt test state escaped its qttest root: $($entry.Path)"
        }
        Remove-Item `
            -LiteralPath $entry.Path `
            -Recurse `
            -Force
    }
}

function Get-FileFingerprint {
    param([string]$Path)

    $resolved =
        [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        return [ordered]@{
            path = $resolved
            exists = $false
            length = 0
            sha256 = ''
        }
    }

    $file = Get-Item -LiteralPath $resolved
    return [ordered]@{
        path = $resolved
        exists = $true
        length = $file.Length
        sha256 = (
            Get-FileHash `
                -LiteralPath $resolved `
                -Algorithm SHA256
        ).Hash.ToLowerInvariant()
    }
}

function Test-FingerprintEqual {
    param(
        [object]$Before,
        [object]$After
    )

    if ($Before.exists -ne $After.exists) {
        return $false
    }
    if (-not $Before.exists) {
        return $true
    }
    return (
        $Before.length -eq $After.length -and
        [string]$Before.sha256 -eq [string]$After.sha256
    )
}

function Get-FileSnapshot {
    param([string]$Path)

    $resolved =
        [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        return [ordered]@{
            exists = $false
            bytes = [byte[]]@()
        }
    }
    return [ordered]@{
        exists = $true
        bytes = [System.IO.File]::ReadAllBytes($resolved)
    }
}

function Restore-FileSnapshot {
    param(
        [string]$Path,
        [object]$Snapshot
    )

    $resolved =
        [System.IO.Path]::GetFullPath($Path)
    if (-not $Snapshot.exists) {
        if (Test-Path -LiteralPath $resolved -PathType Leaf) {
            Remove-Item -LiteralPath $resolved -Force
        }
        return
    }

    $directory =
        Split-Path -Parent $resolved
    New-Item -ItemType Directory -Path $directory -Force |
        Out-Null
    $temporaryPath =
        "$resolved.dpi-matrix-restore-$PID.partial"
    [System.IO.File]::WriteAllBytes(
        $temporaryPath,
        [byte[]]$Snapshot.bytes
    )
    Move-Item `
        -LiteralPath $temporaryPath `
        -Destination $resolved `
        -Force
}

$resolvedExecutable =
    Resolve-RequiredFile `
        -Name 'ExecutablePath' `
        -Path $ExecutablePath
$resolvedWindowVerifier =
    Resolve-RequiredFile `
        -Name 'WindowVerifierPath' `
        -Path $WindowVerifierPath
$resolvedNativeCommand = ''
if (-not [string]::IsNullOrWhiteSpace($NativeCommandPath)) {
    $resolvedNativeCommand =
        Resolve-RequiredFile `
            -Name 'NativeCommandPath' `
            -Path $NativeCommandPath
}
$resolvedEvidenceRoot =
    Resolve-RequiredDirectory `
        -Name 'EvidenceRoot' `
        -Path $EvidenceRoot
$productionSettingsPath =
    Join-Path `
        (
            [Environment]::GetFolderPath(
                [Environment+SpecialFolder]::LocalApplicationData)
        ) `
        'DaSilverFire\Codex Companion\CodexCompanion.ini'
$productionSettingsBefore =
    Get-FileFingerprint `
        -Path $productionSettingsPath

$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public sealed class CompanionPortableDpiWindow
{
    public string Hwnd { get; set; }
    public uint ProcessId { get; set; }
    public string Title { get; set; }
    public string ClassName { get; set; }
    public bool Visible { get; set; }
    public int Left { get; set; }
    public int Top { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
}

public sealed class CompanionPortableDpiPoint
{
    public int X { get; set; }
    public int Y { get; set; }
}

public static class CompanionPortableDpiNative
{
    private const uint PrintWindowRenderFullContent = 0x00000002;
    private delegate bool EnumWindowsProc(
        IntPtr window,
        IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(
        EnumWindowsProc callback,
        IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr window,
        out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLength(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(
        IntPtr window,
        StringBuilder text,
        int maximumLength);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(
        IntPtr window,
        StringBuilder className,
        int maximumLength);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(
        IntPtr window,
        out Rect rectangle);

    [DllImport("user32.dll")]
    private static extern bool PrintWindow(
        IntPtr window,
        IntPtr destination,
        uint flags);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point point);

    [DllImport("user32.dll")]
    private static extern bool SetCursorPos(int x, int y);

    private static IntPtr ResolveWindow(
        uint processId,
        string hwndText)
    {
        var value = hwndText.StartsWith(
            "0x",
            StringComparison.OrdinalIgnoreCase)
            ? hwndText.Substring(2)
            : hwndText;
        long rawHandle;
        if (!Int64.TryParse(
                value,
                System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture,
                out rawHandle))
        {
            throw new ArgumentException("Invalid window handle.");
        }

        var window = new IntPtr(rawHandle);
        uint ownerProcessId;
        GetWindowThreadProcessId(window, out ownerProcessId);
        if (ownerProcessId != processId)
        {
            throw new InvalidOperationException(
                "Window is not owned by the target process.");
        }
        return window;
    }

    public static CompanionPortableDpiWindow[] List(uint processId)
    {
        var windows = new List<CompanionPortableDpiWindow>();
        EnumWindows(delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcessId;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId != processId)
            {
                return true;
            }

            Rect rectangle;
            if (!GetWindowRect(window, out rectangle))
            {
                return true;
            }

            var titleLength = GetWindowTextLength(window);
            var title = new StringBuilder(
                Math.Max(1, titleLength + 1));
            GetWindowText(window, title, title.Capacity);
            var className = new StringBuilder(256);
            GetClassName(
                window,
                className,
                className.Capacity);
            windows.Add(new CompanionPortableDpiWindow
            {
                Hwnd =
                    "0x" + window.ToInt64().ToString("X"),
                ProcessId = ownerProcessId,
                Title = title.ToString(),
                ClassName = className.ToString(),
                Visible = IsWindowVisible(window),
                Left = rectangle.Left,
                Top = rectangle.Top,
                Width = rectangle.Right - rectangle.Left,
                Height = rectangle.Bottom - rectangle.Top
            });
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }

    public static void Capture(
        uint processId,
        string hwndText,
        string path)
    {
        var window = ResolveWindow(processId, hwndText);

        Rect rectangle;
        if (!GetWindowRect(window, out rectangle))
        {
            throw new InvalidOperationException(
                "GetWindowRect failed.");
        }
        var width = rectangle.Right - rectangle.Left;
        var height = rectangle.Bottom - rectangle.Top;
        if (width <= 0 || height <= 0)
        {
            throw new InvalidOperationException(
                "Window geometry is empty.");
        }

        using (var bitmap = new Bitmap(
            width,
            height,
            PixelFormat.Format32bppArgb))
        using (var graphics = Graphics.FromImage(bitmap))
        {
            var target = graphics.GetHdc();
            bool captured;
            try
            {
                captured = PrintWindow(
                    window,
                    target,
                    PrintWindowRenderFullContent);
            }
            finally
            {
                graphics.ReleaseHdc(target);
            }
            if (!captured)
            {
                throw new InvalidOperationException(
                    "PrintWindow failed.");
            }
            bitmap.Save(path, ImageFormat.Png);
        }
    }

    public static CompanionPortableDpiPoint Cursor()
    {
        Point point;
        if (!GetCursorPos(out point))
        {
            throw new InvalidOperationException(
                "GetCursorPos failed.");
        }
        return new CompanionPortableDpiPoint
        {
            X = point.X,
            Y = point.Y
        };
    }

    public static void MoveCursor(int x, int y)
    {
        if (!SetCursorPos(x, y))
        {
            throw new InvalidOperationException(
                "SetCursorPos failed.");
        }
    }

    public static CompanionPortableDpiPoint MoveCursorToWindowPoint(
        uint processId,
        string hwndText,
        int localX,
        int localY)
    {
        var window = ResolveWindow(processId, hwndText);
        Rect rectangle;
        if (!GetWindowRect(window, out rectangle))
        {
            throw new InvalidOperationException(
                "GetWindowRect failed.");
        }
        var width = rectangle.Right - rectangle.Left;
        var height = rectangle.Bottom - rectangle.Top;
        if (
            localX < 0 ||
            localY < 0 ||
            localX >= width ||
            localY >= height)
        {
            throw new ArgumentOutOfRangeException(
                "Cursor coordinates must stay inside the target window.");
        }
        var target = new CompanionPortableDpiPoint
        {
            X = rectangle.Left + localX,
            Y = rectangle.Top + localY
        };
        MoveCursor(target.X, target.Y);
        return target;
    }

}
'@

Add-Type `
    -TypeDefinition $nativeSource `
    -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing

$originalCursor =
    [CompanionPortableDpiNative]::Cursor()

function Get-ExactCompanionProcesses {
    $expected =
        [System.IO.Path]::GetFullPath(
            $resolvedExecutable)
    return @(
        Get-CimInstance `
            Win32_Process `
            -Filter "Name='CodexCompanion.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                -not [string]::IsNullOrWhiteSpace(
                    [string]$_.ExecutablePath) -and
                [System.IO.Path]::GetFullPath(
                    [string]$_.ExecutablePath
                ).Equals(
                    $expected,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            }
    )
}

function Get-OtherCompanionProcesses {
    $expected =
        [System.IO.Path]::GetFullPath(
            $resolvedExecutable)
    return @(
        Get-CimInstance `
            Win32_Process `
            -Filter "Name='CodexCompanion.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                [string]::IsNullOrWhiteSpace(
                    [string]$_.ExecutablePath) -or
                -not [System.IO.Path]::GetFullPath(
                    [string]$_.ExecutablePath
                ).Equals(
                    $expected,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            }
    )
}

function Stop-CompanionProcess {
    param([int]$ProcessId)

    Stop-Process `
        -Id $ProcessId `
        -Force `
        -ErrorAction SilentlyContinue
    $stopped = Wait-Until `
        -Seconds 10 `
        -Condition {
            if (
                $null -eq (
                    Get-Process `
                        -Id $ProcessId `
                        -ErrorAction SilentlyContinue
                )
            ) {
                return $true
            }
            return $null
        }
    if ($null -eq $stopped) {
        throw "Companion process $ProcessId did not exit."
    }
}

function Get-Windows {
    param([int]$ProcessId)

    return @(
        [CompanionPortableDpiNative]::List(
            [uint32]$ProcessId)
    )
}

function Wait-VisibleWindow {
    param(
        [int]$ProcessId,
        [string]$Title,
        [scriptblock]$AdditionalMatch
    )

    return Wait-Until `
        -Condition {
            $matches = @(
                Get-Windows -ProcessId $ProcessId |
                    Where-Object {
                        $_.Visible -and
                        $_.Title -eq $Title
                    }
            )
            foreach ($match in $matches) {
                if (
                    $null -eq $AdditionalMatch -or
                    (& $AdditionalMatch $match)
                ) {
                    return $match
                }
            }
            return $null
        }
}

function Wait-StableVisibleWindow {
    param(
        [int]$ProcessId,
        [string]$Title,
        [scriptblock]$AdditionalMatch
    )

    $deadline =
        [DateTime]::UtcNow.AddSeconds(
            $TimeoutSeconds)
    $previous = $null
    $sameCount = 0
    do {
        $current = @(
            Get-Windows -ProcessId $ProcessId |
                Where-Object {
                    $_.Visible -and
                    $_.Title -eq $Title
                }
        ) |
            Where-Object {
                $null -eq $AdditionalMatch -or
                (& $AdditionalMatch $_)
            } |
            Select-Object -First 1
        if ($null -ne $current) {
            if (
                $null -ne $previous -and
                $previous.Hwnd -eq $current.Hwnd -and
                $previous.Width -eq $current.Width -and
                $previous.Height -eq $current.Height
            ) {
                $sameCount += 1
            } else {
                $sameCount = 0
            }
            $previous = $current
            if ($sameCount -ge 2) {
                return $current
            }
        }
        Start-Sleep -Milliseconds 120
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Find-AutomationElement {
    param(
        [int]$ProcessId,
        [string]$Name,
        [switch]$Prefix
    )

    return Wait-Until `
        -Condition {
            $elements =
                [System.Windows.Automation.AutomationElement]::
                    RootElement.FindAll(
                        [System.Windows.Automation.TreeScope]::
                            Descendants,
                        [System.Windows.Automation.Condition]::
                            TrueCondition
                    )
            for (
                $index = 0;
                $index -lt $elements.Count;
                $index += 1
            ) {
                $element = $elements.Item($index)
                try {
                    if (
                        $element.Current.ProcessId -ne $ProcessId
                    ) {
                        continue
                    }
                    $candidate =
                        [string]$element.Current.Name
                    $matches = if ($Prefix) {
                        $candidate.StartsWith(
                            $Name,
                            [System.StringComparison]::
                                OrdinalIgnoreCase
                        )
                    } else {
                        $candidate.Equals(
                            $Name,
                            [System.StringComparison]::
                                OrdinalIgnoreCase
                        )
                    }
                    if ($matches) {
                        return $element
                    }
                } catch {
                }
            }
            return $null
        }
}

function Invoke-AutomationElement {
    param(
        [int]$ProcessId,
        [string]$Name,
        [switch]$Prefix
    )

    $element =
        Find-AutomationElement `
            -ProcessId $ProcessId `
            -Name $Name `
            -Prefix:$Prefix
    if ($null -eq $element) {
        throw "Automation element was not found: $Name"
    }
    $pattern = $null
    if (
        -not $element.TryGetCurrentPattern(
            [System.Windows.Automation.InvokePattern]::
                Pattern,
            [ref]$pattern
        )
    ) {
        throw "Automation element has no InvokePattern: $Name"
    }
    ([System.Windows.Automation.InvokePattern]$pattern).
        Invoke()
}

function Get-ImageEvidence {
    param(
        [string]$Path,
        [object]$Window
    )

    $image =
        [System.Drawing.Image]::FromFile($Path)
    try {
        if (
            $image.Width -ne $Window.Width -or
            $image.Height -ne $Window.Height
        ) {
            throw (
                "Capture dimensions for $Path do not match " +
                "$($Window.Width)x$($Window.Height)."
            )
        }
        return [ordered]@{
            file =
                [System.IO.Path]::GetFileName($Path)
            width = $image.Width
            height = $image.Height
            sha256 = (
                Get-FileHash `
                    -LiteralPath $Path `
                    -Algorithm SHA256
            ).Hash.ToLowerInvariant()
        }
    } finally {
        $image.Dispose()
    }
}

function Save-WindowCapture {
    param(
        [int]$ProcessId,
        [object]$Window,
        [string]$Path
    )

    [CompanionPortableDpiNative]::Capture(
        [uint32]$ProcessId,
        [string]$Window.Hwnd,
        $Path
    )
    return Get-ImageEvidence `
        -Path $Path `
        -Window $Window
}

function Test-Near {
    param(
        [int]$Actual,
        [int]$Expected,
        [int]$Tolerance = 1
    )

    return [Math]::Abs(
        $Actual - $Expected
    ) -le $Tolerance
}

function Assert-ScaleGeometry {
    param(
        [int]$Scale,
        [object]$Settings,
        [object]$Pet,
        [object]$Processes,
        [object]$Chat
    )

    $factor = $Scale / 100.0
    $round = {
        param([double]$Value)
        [int][Math]::Round(
            $Value,
            0,
            [MidpointRounding]::AwayFromZero
        )
    }

    $petWidth = & $round (124 * $factor)
    $petHeight = & $round (164 * $factor)
    if (
        -not (Test-Near $Pet.Width $petWidth) -or
        -not (Test-Near $Pet.Height $petHeight)
    ) {
        throw (
            "Pet geometry at $Scale percent was " +
            "$($Pet.Width)x$($Pet.Height), expected " +
            "$petWidth x $petHeight."
        )
    }

    $menuWidth = & $round (292 * $factor)
    if (
        -not (Test-Near $Processes.Width $menuWidth) -or
        $Processes.Height -lt (& $round (90 * $factor)) -or
        $Processes.Height -gt (& $round (416 * $factor))
    ) {
        throw (
            "Process geometry at $Scale percent was " +
            "$($Processes.Width)x$($Processes.Height)."
        )
    }

    $chatHeight = & $round (94 * $factor)
    if (
        -not (Test-Near $Chat.Width $menuWidth) -or
        -not (Test-Near $Chat.Height $chatHeight)
    ) {
        throw (
            "Chat geometry at $Scale percent was " +
            "$($Chat.Width)x$($Chat.Height), expected " +
            "$menuWidth x $chatHeight."
        )
    }

    $settingsContentWidth =
        & $round (560 * $factor)
    $settingsContentHeight =
        & $round (520 * $factor)
    if (
        $Settings.Width -lt $settingsContentWidth -or
        $Settings.Width -gt ($settingsContentWidth + 64) -or
        $Settings.Height -lt $settingsContentHeight -or
        $Settings.Height -gt ($settingsContentHeight + 96)
    ) {
        throw (
            "Settings geometry at $Scale percent was " +
            "$($Settings.Width)x$($Settings.Height)."
        )
    }
}

function Invoke-WindowVerifier {
    param(
        [int]$ProcessId,
        [string]$JsonPath
    )

    if ([string]::IsNullOrWhiteSpace($resolvedNativeCommand)) {
        & $resolvedWindowVerifier `
            --pid $ProcessId `
            --json $JsonPath
    } else {
        & $resolvedNativeCommand `
            $resolvedWindowVerifier `
            --pid $ProcessId `
            --json $JsonPath
    }
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw (
            "The native window verifier failed with exit code " +
            "$exitCode."
        )
    }

    $windows =
        Get-Content -LiteralPath $JsonPath -Raw |
        ConvertFrom-Json
    $taskbarCandidates = @(
        $windows |
            Where-Object { $_.taskbarCandidate }
    )
    $altTabCandidates = @(
        $windows |
            Where-Object { $_.altTabCandidate }
    )
    if (
        $taskbarCandidates.Count -ne 0 -or
        $altTabCandidates.Count -ne 0
    ) {
        throw (
            "$($taskbarCandidates.Count) taskbarCandidate and " +
            "$($altTabCandidates.Count) altTabCandidate windows."
        )
    }
    return [ordered]@{
        file =
            [System.IO.Path]::GetFileName($JsonPath)
        windowCount = @($windows).Count
        taskbarCandidateCount =
            $taskbarCandidates.Count
        altTabCandidateCount =
            $altTabCandidates.Count
    }
}

function Start-Companion {
    param(
        [int]$Scale,
        [string]$ProfileRoot,
        [ValidateSet(
            'none',
            'processes',
            'local-chat')]
        [string]$StartupRoute = 'none'
    )

    $localData =
        Join-Path $ProfileRoot 'LocalAppData'
    $roamingData =
        Join-Path $ProfileRoot 'AppData'
    $temporaryData =
        Join-Path $ProfileRoot 'Temp'
    New-Item `
        -ItemType Directory `
        -Path @(
            $localData
            $roamingData
            $temporaryData
        ) `
        -Force |
        Out-Null

    $factor =
        ($Scale / 100.0).ToString(
            '0.##',
            [Globalization.CultureInfo]::InvariantCulture
        )
    $startInfo =
        [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $resolvedExecutable
    $startInfo.WorkingDirectory =
        Split-Path -Parent $resolvedExecutable
    $startInfo.UseShellExecute = $false
    $startInfo.Environment['LOCALAPPDATA'] =
        $localData
    $startInfo.Environment['APPDATA'] =
        $roamingData
    $startInfo.Environment['TEMP'] =
        $temporaryData
    $startInfo.Environment['TMP'] =
        $temporaryData
    $startInfo.Environment['QT_SCALE_FACTOR'] =
        $factor
    $startInfo.Environment[
        'QT_SCALE_FACTOR_ROUNDING_POLICY'
    ] = 'PassThrough'
    $startInfo.Environment[
        'CODEX_COMPANION_TEST_STANDARD_PATHS'
    ] = '1'
    $startInfo.Environment[
        'CODEX_COMPANION_TEST_STARTUP_ROUTE'
    ] = $StartupRoute

    $process =
        [System.Diagnostics.Process]::Start(
            $startInfo)
    if ($null -eq $process) {
        throw "Could not launch Companion at $Scale percent."
    }
    $started = Wait-Until `
        -Condition {
            $process.Refresh()
            if ($process.HasExited) {
                throw (
                    "Companion exited at $Scale percent with code " +
                    "$($process.ExitCode)."
                )
            }
            $settings =
                Wait-VisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Codex Companion Settings'
            if ($null -ne $settings) {
                return $process
            }
            return $null
        }
    if ($null -eq $started) {
        Stop-CompanionProcess -ProcessId $process.Id
        throw "Companion did not show Settings at $Scale percent."
    }
    return $process
}

function Restore-PreviousInstance {
    $process =
        Start-Process `
            -FilePath $resolvedExecutable `
            -WorkingDirectory (
                Split-Path -Parent $resolvedExecutable
            ) `
            -PassThru
    $settings =
        Wait-VisibleWindow `
            -ProcessId $process.Id `
            -Title 'Codex Companion Settings'
    if ($null -eq $settings) {
        throw 'The previous Companion instance could not be restored.'
    }
    Invoke-AutomationElement `
        -ProcessId $process.Id `
        -Name 'Close Settings'
    $settingsHidden = Wait-Until `
        -Condition {
            $visible = @(
                Get-Windows -ProcessId $process.Id |
                    Where-Object {
                        $_.Visible -and
                        $_.Title -eq
                            'Codex Companion Settings'
                    }
            )
            if ($visible.Count -eq 0) {
                return $true
            }
            return $null
        }
    if ($null -eq $settingsHidden) {
        throw 'The restored Companion Settings window did not close.'
    }
    return [ordered]@{
        restored = $true
        processId = $process.Id
        settingsClosed = $true
    }
}

$otherProcesses = @(
    Get-OtherCompanionProcesses
)
if ($otherProcesses.Count -ne 0) {
    throw (
        'A different CodexCompanion.exe is running: ' +
        (
            @(
                $otherProcesses |
                    ForEach-Object {
                        [string]$_.ExecutablePath
                    }
            ) -join ', '
        )
    )
}

$existingProcesses = @(
    Get-ExactCompanionProcesses
)
if ($existingProcesses.Count -gt 1) {
    throw (
        'More than one exact packaged Companion process is running.'
    )
}
$previousInstanceWasRunning =
    $existingProcesses.Count -eq 1
if (
    $previousInstanceWasRunning -and
    -not $TemporarilyStopRunningInstance
) {
    throw (
        'The exact packaged Companion is running. Pass ' +
        '-TemporarilyStopRunningInstance to restore it after the matrix.'
    )
}

$results =
    New-Object System.Collections.ArrayList
$fatalError = ''
$restoration = [ordered]@{
    restored = $false
    processId = 0
    settingsClosed = $false
}
$productionSettingsAfterMatrix =
    $productionSettingsBefore
$productionSettingsBaseline =
    $productionSettingsBefore
$productionSettingsSnapshot =
    Get-FileSnapshot `
        -Path $productionSettingsPath
$productionSettingsAfterSnapshotRestore =
    $productionSettingsBefore
$productionSettingsAfterRestoration =
    $productionSettingsBefore
$productionSettingsPreserved = $false
$activeProcessId = 0
$cursorAfter = $originalCursor
$cursorRestored = $false

try {
    if ($previousInstanceWasRunning) {
        Stop-CompanionProcess `
            -ProcessId (
                [int]$existingProcesses[0].ProcessId
            )
    }
    $productionSettingsBaseline =
        Get-FileFingerprint `
            -Path $productionSettingsPath
    $productionSettingsSnapshot =
        Get-FileSnapshot `
            -Path $productionSettingsPath

    foreach ($scale in @($DpiPercent)) {
        Remove-QtTestState
        $scaleRoot =
            Join-Path `
                $resolvedEvidenceRoot `
                ("scale-" + $scale)
        New-Item `
            -ItemType Directory `
            -Path $scaleRoot `
            -Force |
            Out-Null
        $profileRoot =
            Join-Path $scaleRoot 'profile'
        $process = $null
        $factor = $scale / 100.0
        $processIds = [ordered]@{
            base = 0
            processes = 0
            chat = 0
        }
        $processHoverCursor = [ordered]@{
            target = $null
            observed = $null
            stationary = $false
        }
        $windowVerifiers = [ordered]@{}
        try {
            $process =
                Start-Companion `
                    -Scale $scale `
                    -ProfileRoot (
                        Join-Path `
                            $profileRoot `
                            'base'
                    ) `
                    -StartupRoute 'none'
            $activeProcessId = $process.Id
            $processIds.base = $process.Id

            $settings =
                Wait-StableVisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Codex Companion Settings'
            $pet =
                Wait-StableVisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Codex Companion' `
                    -AdditionalMatch {
                        param($window)
                        $window.ClassName -like
                            'Qt*QWindowToolSaveBits' -and
                        $window.Width -lt (
                            256 * ($scale / 100.0)
                        )
                    }
            if ($null -eq $settings -or $null -eq $pet) {
                throw (
                    "Settings or pet was not stable at $scale percent."
                )
            }

            $settingsCapture =
                Save-WindowCapture `
                    -ProcessId $process.Id `
                    -Window $settings `
                    -Path (
                        Join-Path $scaleRoot 'settings.png'
                    )
            $petCapture =
                Save-WindowCapture `
                    -ProcessId $process.Id `
                    -Window $pet `
                    -Path (
                        Join-Path $scaleRoot 'pet.png'
                    )
            $windowVerifiers.base =
                Invoke-WindowVerifier `
                    -ProcessId $process.Id `
                    -JsonPath (
                        Join-Path `
                            $scaleRoot `
                            'window-verifier-base.json'
                    )

            Stop-CompanionProcess `
                -ProcessId $process.Id
            $process = $null
            $activeProcessId = 0
            Remove-QtTestState

            $process =
                Start-Companion `
                    -Scale $scale `
                    -ProfileRoot (
                        Join-Path `
                            $profileRoot `
                            'processes'
                    ) `
                    -StartupRoute 'processes'
            $activeProcessId = $process.Id
            $processIds.processes = $process.Id
            $processes =
                Wait-StableVisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Codex Processes'
            if ($null -eq $processes) {
                throw (
                    "Codex Processes was not stable at $scale percent."
                )
            }
            $processCapture =
                Save-WindowCapture `
                    -ProcessId $process.Id `
                    -Window $processes `
                    -Path (
                        Join-Path $scaleRoot 'processes.png'
                    )

            $processHoverLocalX =
                [int][Math]::Round(
                    $processes.Width / 2.0)
            $processHoverLocalY =
                [int][Math]::Round(
                    52 * $factor)
            $processHoverCursor.target =
                [CompanionPortableDpiNative]::
                    MoveCursorToWindowPoint(
                    [uint32]$process.Id,
                    [string]$processes.Hwnd,
                    $processHoverLocalX,
                    $processHoverLocalY
                )
            for (
                $sample = 0;
                $sample -lt 24;
                $sample += 1
            ) {
                Start-Sleep -Milliseconds 40
            }
            $processHoverCursor.observed =
                [CompanionPortableDpiNative]::Cursor()
            $processHoverCursor.stationary =
                $processHoverCursor.target.X -eq
                    $processHoverCursor.observed.X -and
                $processHoverCursor.target.Y -eq
                    $processHoverCursor.observed.Y
            if (-not $processHoverCursor.stationary) {
                throw (
                    'The physical cursor did not remain stationary ' +
                    "during process hover at $scale percent."
                )
            }
            $hoveredProcesses =
                Get-Windows -ProcessId $process.Id |
                Where-Object {
                    $_.Visible -and
                    $_.Hwnd -eq $processes.Hwnd
                } |
                Select-Object -First 1
            if ($null -eq $hoveredProcesses) {
                throw (
                    "The hovered process window disappeared at " +
                    "$scale percent."
                )
            }
            $processHoverCapture =
                Save-WindowCapture `
                    -ProcessId $process.Id `
                    -Window $hoveredProcesses `
                    -Path (
                        Join-Path `
                            $scaleRoot `
                            'processes-hover.png'
                    )
            $windowVerifiers.processes =
                Invoke-WindowVerifier `
                    -ProcessId $process.Id `
                    -JsonPath (
                        Join-Path `
                            $scaleRoot `
                            'window-verifier-processes.json'
                    )

            Stop-CompanionProcess `
                -ProcessId $process.Id
            $process = $null
            $activeProcessId = 0
            Remove-QtTestState

            $process =
                Start-Companion `
                    -Scale $scale `
                    -ProfileRoot (
                        Join-Path `
                            $profileRoot `
                            'chat'
                    ) `
                    -StartupRoute 'local-chat'
            $activeProcessId = $process.Id
            $processIds.chat = $process.Id
            $chat =
                Wait-StableVisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Local Chat'
            if ($null -eq $chat) {
                throw (
                    "Local Chat was not stable at $scale percent."
                )
            }
            $chatCapture =
                Save-WindowCapture `
                    -ProcessId $process.Id `
                    -Window $chat `
                    -Path (
                        Join-Path $scaleRoot 'chat.png'
                    )
            $windowVerifiers.chat =
                Invoke-WindowVerifier `
                    -ProcessId $process.Id `
                    -JsonPath (
                        Join-Path `
                            $scaleRoot `
                            'window-verifier-chat.json'
                    )

            Assert-ScaleGeometry `
                -Scale $scale `
                -Settings $settings `
                -Pet $pet `
                -Processes $hoveredProcesses `
                -Chat $chat

            [void]$results.Add(
                [ordered]@{
                    scalePercent = $scale
                    scaleFactor = $scale / 100.0
                    processIds = $processIds
                    windows = [ordered]@{
                        settings = $settings
                        pet = $pet
                        processes = $hoveredProcesses
                        chat = $chat
                    }
                    captures = [ordered]@{
                        settings = $settingsCapture
                        pet = $petCapture
                        processes = $processCapture
                        processesHover =
                            $processHoverCapture
                        chat = $chatCapture
                    }
                    windowVerifiers =
                        $windowVerifiers
                    processHoverCursor =
                        $processHoverCursor
                    passed = $true
                }
            )
        } finally {
            if ($null -ne $process) {
                Stop-CompanionProcess `
                    -ProcessId $process.Id
            }
            $activeProcessId = 0
            Remove-IsolatedProfile `
                -Path $profileRoot `
                -Root $resolvedEvidenceRoot
            Remove-QtTestState
        }
    }
} catch {
    $fatalError = $_.Exception.Message
} finally {
    if ($activeProcessId -ne 0) {
        Stop-CompanionProcess `
            -ProcessId $activeProcessId
    }
    try {
        Remove-QtTestState
    } catch {
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $_.Exception.Message
        } else {
            $fatalError +=
                ' Qt test cleanup failed: ' +
                $_.Exception.Message
        }
    }
    $productionSettingsAfterMatrix =
        Get-FileFingerprint `
            -Path $productionSettingsPath
    $matrixChangedProductionSettings =
        -not (
            Test-FingerprintEqual `
                -Before $productionSettingsBaseline `
                -After $productionSettingsAfterMatrix
        )
    if ($matrixChangedProductionSettings) {
        try {
            Restore-FileSnapshot `
                -Path $productionSettingsPath `
                -Snapshot $productionSettingsSnapshot
        } catch {
            if ([string]::IsNullOrWhiteSpace($fatalError)) {
                $fatalError = $_.Exception.Message
            } else {
                $fatalError +=
                    ' Settings snapshot restoration failed: ' +
                    $_.Exception.Message
            }
        }
        $message =
            'The packaged DPI matrix changed the production settings file; the post-stop snapshot was restored.'
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $message
        } else {
            $fatalError += ' ' + $message
        }
    }
    $productionSettingsAfterSnapshotRestore =
        Get-FileFingerprint `
            -Path $productionSettingsPath
    $productionSettingsPreserved =
        Test-FingerprintEqual `
            -Before $productionSettingsBaseline `
            -After $productionSettingsAfterSnapshotRestore
    if (-not $productionSettingsPreserved) {
        $message =
            'The production settings snapshot could not be preserved before instance restoration.'
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $message
        } else {
            $fatalError += ' ' + $message
        }
    }
    if ($previousInstanceWasRunning) {
        try {
            $restoration =
                Restore-PreviousInstance
        } catch {
            if ([string]::IsNullOrWhiteSpace($fatalError)) {
                $fatalError = $_.Exception.Message
            } else {
                $fatalError +=
                    ' Restoration failed: ' +
                    $_.Exception.Message
            }
        }
    }
    $productionSettingsAfterRestoration =
        Get-FileFingerprint `
            -Path $productionSettingsPath
    try {
        [CompanionPortableDpiNative]::MoveCursor(
            [int]$originalCursor.X,
            [int]$originalCursor.Y
        )
        $cursorAfter =
            [CompanionPortableDpiNative]::Cursor()
        $cursorRestored =
            $cursorAfter.X -eq $originalCursor.X -and
            $cursorAfter.Y -eq $originalCursor.Y
        if (-not $cursorRestored) {
            throw (
                'The packaged DPI matrix could not restore ' +
                'the original cursor position.'
            )
        }
    } catch {
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $_.Exception.Message
        } else {
            $fatalError +=
                ' Cursor restoration failed: ' +
                $_.Exception.Message
        }
    }
}

$summary = [ordered]@{
    schemaVersion = 1
    passed =
        [string]::IsNullOrWhiteSpace($fatalError) -and
        $results.Count -eq @($DpiPercent).Count
    executable = $resolvedExecutable
    executableSha256 = (
        Get-FileHash `
            -LiteralPath $resolvedExecutable `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    recordedAtUtc =
        [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ssZ',
            [Globalization.CultureInfo]::InvariantCulture
        )
    scalePercent = @($DpiPercent)
    temporarilyStoppedPreviousInstance =
        $previousInstanceWasRunning
    restoration = $restoration
    productionSettingsBefore =
        $productionSettingsBefore
    productionSettingsBaseline =
        $productionSettingsBaseline
    productionSettingsAfterMatrix =
        $productionSettingsAfterMatrix
    productionSettingsAfterSnapshotRestore =
        $productionSettingsAfterSnapshotRestore
    productionSettingsAfterRestoration =
        $productionSettingsAfterRestoration
    productionSettingsPreserved =
        $productionSettingsPreserved
    cursorBefore = $originalCursor
    cursorAfter = $cursorAfter
    cursorRestored = $cursorRestored
    fatalError = $fatalError
    results = @($results)
}
$summaryPath =
    Join-Path `
        $resolvedEvidenceRoot `
        'dpi-matrix-summary.json'
Write-JsonAtomic `
    -Path $summaryPath `
    -Value $summary
Write-Output $summaryPath

if (-not $summary.passed) {
    exit 2
}
