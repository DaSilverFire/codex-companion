[CmdletBinding()]
param(
    [string]$ExecutablePath,

    [string]$WindowVerifierPath,

    [string]$NativeCommandPath,

    [string]$EvidenceRoot,

    [ValidateRange(15, 60)]
    [int]$CaptureRate = 30,

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
        scenario = 'pet-drag-reversal'
        directions = @(
            'left'
            'right'
            'left'
        )
        stationaryAfterReversal = $true
        isolatedProfile = $true
        exactWindowCapture = $true
        productionSettingsPreserved = $true
        qtTestSettingsPreserved = $true
        recordsMeasuredCadence = $true
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

function Wait-Until {
    param(
        [scriptblock]$Condition,
        [int]$Seconds = $TimeoutSeconds,
        [int]$PollMilliseconds = 100
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
        "$resolved.pet-drag-restore-$PID.partial"
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
$resolvedEvidenceRoot =
    Resolve-RequiredDirectory `
        -Name 'EvidenceRoot' `
        -Path $EvidenceRoot
$resolvedWindowVerifier = ''
if (-not [string]::IsNullOrWhiteSpace($WindowVerifierPath)) {
    $resolvedWindowVerifier =
        Resolve-RequiredFile `
            -Name 'WindowVerifierPath' `
            -Path $WindowVerifierPath
}
$resolvedNativeCommand = ''
if (-not [string]::IsNullOrWhiteSpace($NativeCommandPath)) {
    $resolvedNativeCommand =
        Resolve-RequiredFile `
            -Name 'NativeCommandPath' `
            -Path $NativeCommandPath
}

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
$qtTestSettingsPath =
    Join-Path `
        (
            [Environment]::GetFolderPath(
                [Environment+SpecialFolder]::LocalApplicationData)
        ) `
        'qttest\DaSilverFire\Codex Companion\CodexCompanion.ini'
$qtTestSettingsBefore =
    Get-FileFingerprint `
        -Path $qtTestSettingsPath

$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public sealed class CompanionPetDragWindow
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

public sealed class CompanionPetDragPoint
{
    public int X { get; set; }
    public int Y { get; set; }
}

public static class CompanionPetDragNative
{
    private const uint PrintWindowRenderFullContent = 0x00000002;
    private const uint InputMouse = 0;
    private const uint MouseEventLeftDown = 0x0002;
    private const uint MouseEventLeftUp = 0x0004;
    private const uint WM_CLOSE = 0x0010;

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

    [StructLayout(LayoutKind.Sequential)]
    private struct MouseInput
    {
        public int Dx;
        public int Dy;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public UIntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct InputUnion
    {
        [FieldOffset(0)]
        public MouseInput Mouse;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Input
    {
        public uint Type;
        public InputUnion Data;
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
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out Point point);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern uint SendInput(
        uint inputCount,
        Input[] inputs,
        int inputSize);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(
        IntPtr window,
        uint message,
        IntPtr wParam,
        IntPtr lParam);

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

    public static CompanionPetDragWindow[] List(uint processId)
    {
        var windows = new List<CompanionPetDragWindow>();
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
            windows.Add(new CompanionPetDragWindow
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

    public static CompanionPetDragPoint Cursor()
    {
        Point point;
        if (!GetCursorPos(out point))
        {
            throw new InvalidOperationException(
                "GetCursorPos failed.");
        }
        return new CompanionPetDragPoint
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

    public static void Focus(
        uint processId,
        string hwndText)
    {
        SetForegroundWindow(
            ResolveWindow(processId, hwndText));
    }

    public static void Close(
        uint processId,
        string hwndText)
    {
        if (!PostMessage(
                ResolveWindow(processId, hwndText),
                WM_CLOSE,
                IntPtr.Zero,
                IntPtr.Zero))
        {
            throw new InvalidOperationException(
                "PostMessage(WM_CLOSE) failed.");
        }
    }

    public static void SendLeftButton(bool down)
    {
        var input = new Input
        {
            Type = InputMouse,
            Data = new InputUnion
            {
                Mouse = new MouseInput
                {
                    Flags =
                        down
                            ? MouseEventLeftDown
                            : MouseEventLeftUp
                }
            }
        };
        var inputs = new[] { input };
        if (SendInput(
                1,
                inputs,
                Marshal.SizeOf(typeof(Input))) != 1)
        {
            throw new InvalidOperationException(
                "SendInput failed.");
        }
    }
}
'@

Add-Type `
    -TypeDefinition $nativeSource `
    -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

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
        [CompanionPetDragNative]::List(
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

function Wait-StablePetWindow {
    param(
        [int]$ProcessId,
        [string]$Hwnd = ''
    )

    $deadline =
        [DateTime]::UtcNow.AddSeconds(
            $TimeoutSeconds)
    $previous = $null
    $sameCount = 0
    do {
        $current =
            Get-Windows -ProcessId $ProcessId |
            Where-Object {
                $_.Visible -and
                $_.Title -eq 'Codex Companion' -and
                $_.ClassName -like 'Qt*' -and
                $_.Width -ge 100 -and
                $_.Width -le 260 -and
                $_.Height -ge 130 -and
                $_.Height -le 340 -and
                (
                    [string]::IsNullOrWhiteSpace($Hwnd) -or
                    $_.Hwnd -eq $Hwnd
                )
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
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    return $null
}

function Find-AutomationElement {
    param(
        [int]$ProcessId,
        [string]$Name
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
                        $element.Current.ProcessId -eq $ProcessId -and
                        [string]$element.Current.Name -eq $Name
                    ) {
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
        [string]$Name
    )

    $element =
        Find-AutomationElement `
            -ProcessId $ProcessId `
            -Name $Name
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

function Write-IsolatedSettings {
    param(
        [string]$LocalData,
        [string]$RoamingData,
        [string]$QtTestSettingsPath
    )

    $workArea =
        [System.Windows.Forms.Screen]::
            PrimaryScreen.WorkingArea
    $windowX =
        $workArea.Left +
        [Math]::Max(
            40,
            [int][Math]::Round(
                ($workArea.Width - 124) / 2.0))
    $windowY =
        $workArea.Top +
        [Math]::Max(
            40,
            [int][Math]::Round(
                ($workArea.Height - 164) / 2.0))
    $contents = @"
[appearance]
backdrop=solid-black

[pet]
selectedId=qa-drag-pet
animationSpeedScale=1.15
animationSpeedTimingVersion=2
visible=true
windowPosition=@Point($windowX $windowY)
hideControlsUntilHover=false
allowAutonomousMovement=false

[mobile]
enabled=false
keepAvailableWhileDisplayOff=false
allowNearbyOnPublicNetworks=false
relayMode=disabled
customRelayUrl=

[chat]
selectedModelId=lumo:automatic
"@
    $paths = @(
        (
            Join-Path `
                $LocalData `
                'qttest\DaSilverFire\Codex Companion\CodexCompanion.ini'
        )
        (
            Join-Path `
                $LocalData `
                'DaSilverFire\Codex Companion\CodexCompanion.ini'
        )
        (
            Join-Path `
                $RoamingData `
                'qttest\DaSilverFire\Codex Companion\CodexCompanion.ini'
        )
        $QtTestSettingsPath
    )
    $encoding =
        New-Object System.Text.UTF8Encoding($false)
    foreach ($path in $paths) {
        New-Item `
            -ItemType Directory `
            -Path (
                Split-Path -Parent $path
            ) `
            -Force |
            Out-Null
        [System.IO.File]::WriteAllText(
            $path,
            $contents,
            $encoding
        )
    }
    return $paths
}

function Write-IsolatedPetPackage {
    param([string]$LocalData)

    $petDirectory =
        Join-Path `
            $LocalData `
            'Codex Companion\Pets\qa-drag-pet'
    New-Item `
        -ItemType Directory `
        -Path $petDirectory `
        -Force |
        Out-Null

    $columns = 4
    $rows = 3
    $frameSize = 32
    $bitmap =
        [System.Drawing.Bitmap]::new(
            $columns * $frameSize,
            $rows * $frameSize,
            [System.Drawing.Imaging.PixelFormat]::
                Format32bppArgb)
    try {
        $graphics =
            [System.Drawing.Graphics]::FromImage(
                $bitmap)
        try {
            $graphics.Clear(
                [System.Drawing.Color]::Transparent)
            for ($row = 0; $row -lt $rows; $row += 1) {
                for (
                    $column = 0;
                    $column -lt $columns;
                    $column += 1
                ) {
                    $red = 72 + ($row * 54)
                    $green = 96 + ($column * 34)
                    $blue = 210 - ($row * 42)
                    $brush =
                        [System.Drawing.SolidBrush]::new(
                            [System.Drawing.Color]::
                                FromArgb(
                                    255,
                                    $red,
                                    $green,
                                    $blue))
                    try {
                        $x =
                            ($column * $frameSize) +
                            3 +
                            ($column * 2)
                        $y =
                            ($row * $frameSize) +
                            5 +
                            (($column + $row) % 3)
                        $graphics.FillEllipse(
                            $brush,
                            $x,
                            $y,
                            20,
                            20)
                    } finally {
                        $brush.Dispose()
                    }
                }
            }
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save(
            (
                Join-Path `
                    $petDirectory `
                    'spritesheet.png'
            ),
            [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }

    Write-JsonAtomic `
        -Path (
            Join-Path `
                $petDirectory `
                'pet.json'
        ) `
        -Value (
            [ordered]@{
                id = 'qa-drag-pet'
                displayName = 'QA Drag Pet'
                spritesheetPath = 'spritesheet.png'
                spriteColumns = $columns
                spriteRows = $rows
                animationFrameCounts =
                    [ordered]@{
                        idle = $columns
                        'running-right' = $columns
                        'running-left' = $columns
                        running = $columns
                    }
            }
        )
}

function Start-IsolatedCompanion {
    param([string]$ProfileRoot)

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
    Write-IsolatedPetPackage `
        -LocalData $localData
    $settingsPaths =
        Write-IsolatedSettings `
            -LocalData $localData `
            -RoamingData $roamingData `
            -QtTestSettingsPath $qtTestSettingsPath

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
    $startInfo.Environment['QT_SCALE_FACTOR'] = '1'
    $startInfo.Environment[
        'QT_SCALE_FACTOR_ROUNDING_POLICY'
    ] = 'PassThrough'
    $startInfo.Environment[
        'CODEX_COMPANION_TEST_STANDARD_PATHS'
    ] = '1'
    $startInfo.Environment[
        'CODEX_COMPANION_TEST_STARTUP_ROUTE'
    ] = 'none'

    $process =
        [System.Diagnostics.Process]::Start(
            $startInfo)
    if ($null -eq $process) {
        throw 'Could not launch the isolated Companion.'
    }
    $started = Wait-Until `
        -Condition {
            $process.Refresh()
            if ($process.HasExited) {
                throw (
                    'The isolated Companion exited with code ' +
                    "$($process.ExitCode)."
                )
            }
            $settings =
                Wait-VisibleWindow `
                    -ProcessId $process.Id `
                    -Title 'Codex Companion Settings'
            $pet =
                Wait-StablePetWindow `
                    -ProcessId $process.Id
            if ($null -ne $settings -and $null -ne $pet) {
                return [pscustomobject]@{
                    Process = $process
                    Settings = $settings
                    Pet = $pet
                    SettingsPaths = $settingsPaths
                }
            }
            return $null
        }
    if ($null -eq $started) {
        Stop-CompanionProcess -ProcessId $process.Id
        throw 'The isolated Companion did not show Settings and the pet.'
    }
    return $started
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
    [CompanionPetDragNative]::Close(
        [uint32]$process.Id,
        [string]$settings.Hwnd
    )
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

function Invoke-WindowVerifier {
    param(
        [int]$ProcessId,
        [string]$JsonPath
    )

    if ([string]::IsNullOrWhiteSpace($resolvedWindowVerifier)) {
        return [ordered]@{
            ran = $false
            file = ''
            windowCount = 0
            taskbarCandidateCount = 0
            altTabCandidateCount = 0
        }
    }
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
            'The native window verifier failed with exit code ' +
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
        ran = $true
        file =
            [System.IO.Path]::GetFileName($JsonPath)
        windowCount = @($windows).Count
        taskbarCandidateCount =
            $taskbarCandidates.Count
        altTabCandidateCount =
            $altTabCandidates.Count
    }
}

function Save-DragFrame {
    param(
        [int]$ProcessId,
        [string]$Hwnd,
        [string]$FramesRoot,
        [object]$State,
        [string]$Phase,
        [string]$ExpectedDirection,
        [int]$DelayMilliseconds
    )

    if ($DelayMilliseconds -gt 0) {
        $State.NextCaptureDueMilliseconds +=
            $DelayMilliseconds
        $remainingMilliseconds =
            $State.NextCaptureDueMilliseconds -
            $State.Stopwatch.ElapsedMilliseconds
        if ($remainingMilliseconds -gt 0) {
            Start-Sleep `
                -Milliseconds $remainingMilliseconds
        }
    }
    $window =
        Get-Windows -ProcessId $ProcessId |
        Where-Object {
            $_.Visible -and
            $_.Hwnd -eq $Hwnd
        } |
        Select-Object -First 1
    if ($null -eq $window) {
        throw "The pet window disappeared during phase $Phase."
    }
    $fileName =
        'frame-{0:D4}.png' -f $State.Index
    $path =
        Join-Path $FramesRoot $fileName
    [CompanionPetDragNative]::Capture(
        [uint32]$ProcessId,
        $Hwnd,
        $path
    )
    $image =
        [System.Drawing.Image]::FromFile($path)
    try {
        if (
            $image.Width -ne $window.Width -or
            $image.Height -ne $window.Height
        ) {
            throw (
                "Capture dimensions for $fileName do not match " +
                "$($window.Width)x$($window.Height)."
            )
        }
    } finally {
        $image.Dispose()
    }
    $cursor =
        [CompanionPetDragNative]::Cursor()
    $record =
        [pscustomobject]@{
            index = $State.Index
            file = "frames/$fileName"
            elapsedMilliseconds =
                $State.Stopwatch.ElapsedMilliseconds
            phase = $Phase
            expectedDirection = $ExpectedDirection
            pointerX = $cursor.X
            pointerY = $cursor.Y
            windowLeft = $window.Left
            windowTop = $window.Top
            windowWidth = $window.Width
            windowHeight = $window.Height
            sha256 = (
                Get-FileHash `
                    -LiteralPath $path `
                    -Algorithm SHA256
            ).Hash.ToLowerInvariant()
        }
    [void]$State.Records.Add($record)
    $State.Index += 1
    return $record
}

function Get-LastPhaseRecord {
    param(
        [object[]]$Records,
        [string]$Phase
    )

    return $Records |
        Where-Object { $_.phase -eq $Phase } |
        Select-Object -Last 1
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
        '-TemporarilyStopRunningInstance to restore it after verification.'
    )
}

$profileRoot =
    Join-Path $resolvedEvidenceRoot 'profile'
$framesRoot =
    Join-Path $resolvedEvidenceRoot 'frames'
New-Item `
    -ItemType Directory `
    -Path $framesRoot `
    -Force |
    Out-Null

$fatalError = ''
$activeProcessId = 0
$isolatedProcess = $null
$leftButtonHeld = $false
$originalCursor =
    [CompanionPetDragNative]::Cursor()
$restoration = [ordered]@{
    restored = $false
    processId = 0
    settingsClosed = $false
}
$productionSettingsBaseline =
    $productionSettingsBefore
$productionSettingsSnapshot =
    Get-FileSnapshot `
        -Path $productionSettingsPath
$productionSettingsAfterVerification =
    $productionSettingsBefore
$productionSettingsAfterSnapshotRestore =
    $productionSettingsBefore
$productionSettingsAfterRestoration =
    $productionSettingsBefore
$productionSettingsPreserved = $false
$qtTestSettingsBaseline =
    $qtTestSettingsBefore
$qtTestSettingsSnapshot =
    Get-FileSnapshot `
        -Path $qtTestSettingsPath
$qtTestSettingsAfterVerification =
    $qtTestSettingsBefore
$qtTestSettingsAfterSnapshotRestore =
    $qtTestSettingsBefore
$qtTestSettingsPreserved = $false
$windowPolicy = [ordered]@{
    before = $null
    after = $null
}
$captureState =
    [pscustomobject]@{
        Index = 0
        Records =
            New-Object System.Collections.ArrayList
        Stopwatch =
            [System.Diagnostics.Stopwatch]::StartNew()
        NextCaptureDueMilliseconds = 0
    }
$isolatedSettingsPaths = @()
$dragGeometry = [ordered]@{}
$captureDurationMilliseconds = 0
$measuredCaptureRate = 0.0

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
    $qtTestSettingsBaseline =
        Get-FileFingerprint `
            -Path $qtTestSettingsPath
    $qtTestSettingsSnapshot =
        Get-FileSnapshot `
            -Path $qtTestSettingsPath

    Remove-IsolatedProfile `
        -Path $profileRoot `
        -Root $resolvedEvidenceRoot
    $started =
        Start-IsolatedCompanion `
            -ProfileRoot $profileRoot
    $isolatedProcess = $started.Process
    $activeProcessId = $isolatedProcess.Id
    $isolatedSettingsPaths =
        @($started.SettingsPaths)
    $pet = $started.Pet

    [CompanionPetDragNative]::Close(
        [uint32]$isolatedProcess.Id,
        [string]$started.Settings.Hwnd
    )
    $settingsHidden = Wait-Until `
        -Condition {
            $visible = @(
                Get-Windows `
                    -ProcessId $isolatedProcess.Id |
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
        throw 'Close Settings did not hide the isolated Settings window.'
    }
    $pet =
        Wait-StablePetWindow `
            -ProcessId $isolatedProcess.Id `
            -Hwnd $pet.Hwnd
    if ($null -eq $pet) {
        throw 'The pet was not stable after Settings closed.'
    }

    $windowPolicy.before =
        Invoke-WindowVerifier `
            -ProcessId $isolatedProcess.Id `
            -JsonPath (
                Join-Path `
                    $resolvedEvidenceRoot `
                    'window-verifier-before.json'
            )

    $frameDelay =
        [Math]::Max(
            1,
            [int][Math]::Round(
                1000.0 / $CaptureRate))
    $pointerLocalX =
        [Math]::Min(
            $pet.Width - 12,
            [Math]::Max(
                12,
                [int][Math]::Round(
                    $pet.Width / 2.0)))
    $pointerLocalY =
        [Math]::Min(
            $pet.Height - 12,
            [Math]::Max(
                56,
                [int][Math]::Round(
                    $pet.Height * 0.62)))
    $startPointerX =
        $pet.Left + $pointerLocalX
    $startPointerY =
        $pet.Top + $pointerLocalY
    [CompanionPetDragNative]::Focus(
        [uint32]$isolatedProcess.Id,
        [string]$pet.Hwnd
    )
    [CompanionPetDragNative]::MoveCursor(
        $startPointerX,
        $startPointerY
    )
    $captureState.NextCaptureDueMilliseconds = 0
    $captureState.Stopwatch.Restart()
    [void](
        Save-DragFrame `
            -ProcessId $isolatedProcess.Id `
            -Hwnd $pet.Hwnd `
            -FramesRoot $framesRoot `
            -State $captureState `
            -Phase 'ready' `
            -ExpectedDirection '' `
            -DelayMilliseconds 120
    )

    [CompanionPetDragNative]::SendLeftButton($true)
    $leftButtonHeld = $true
    [void](
        Save-DragFrame `
            -ProcessId $isolatedProcess.Id `
            -Hwnd $pet.Hwnd `
            -FramesRoot $framesRoot `
            -State $captureState `
            -Phase 'pressed' `
            -ExpectedDirection 'right' `
            -DelayMilliseconds $frameDelay
    )

    $phases = @(
        [pscustomobject]@{
            Name = 'left-prime'
            Direction = 'left'
            Offsets = @(-4, -8, -12, -16, -20)
        }
        [pscustomobject]@{
            Name = 'stationary-before-right-reversal'
            Direction = 'left'
            Offsets = @(-20, -20, -20, -20, -20, -20)
        }
        [pscustomobject]@{
            Name = 'right-reversal'
            Direction = 'right'
            Offsets = @(-19)
        }
        [pscustomobject]@{
            Name = 'stationary-after-right-reversal'
            Direction = 'right'
            Offsets = @(
                -19, -19, -19, -19, -19, -19,
                -19, -19, -19, -19, -19, -19
            )
        }
        [pscustomobject]@{
            Name = 'right-continuation'
            Direction = 'right'
            Offsets = @(-14, -9, -4, 1, 6, 12, 18, 24)
        }
        [pscustomobject]@{
            Name = 'stationary-before-left-reversal'
            Direction = 'right'
            Offsets = @(24, 24, 24, 24, 24, 24)
        }
        [pscustomobject]@{
            Name = 'left-reversal'
            Direction = 'left'
            Offsets = @(23)
        }
        [pscustomobject]@{
            Name = 'stationary-after-left-reversal'
            Direction = 'left'
            Offsets = @(
                23, 23, 23, 23, 23, 23,
                23, 23, 23, 23, 23, 23
            )
        }
        [pscustomobject]@{
            Name = 'left-continuation'
            Direction = 'left'
            Offsets = @(18, 12, 6, 0, -6, -12, -18, -24)
        }
    )

    foreach ($phase in $phases) {
        foreach ($offset in $phase.Offsets) {
            [CompanionPetDragNative]::MoveCursor(
                $startPointerX + $offset,
                $startPointerY
            )
            [void](
                Save-DragFrame `
                    -ProcessId $isolatedProcess.Id `
                    -Hwnd $pet.Hwnd `
                    -FramesRoot $framesRoot `
                    -State $captureState `
                    -Phase $phase.Name `
                    -ExpectedDirection $phase.Direction `
                    -DelayMilliseconds $frameDelay
            )
        }
    }

    [CompanionPetDragNative]::SendLeftButton($false)
    $leftButtonHeld = $false
    foreach ($releaseSample in 1..3) {
        [void](
            Save-DragFrame `
                -ProcessId $isolatedProcess.Id `
                -Hwnd $pet.Hwnd `
                -FramesRoot $framesRoot `
                -State $captureState `
                -Phase 'released' `
                -ExpectedDirection '' `
                -DelayMilliseconds $frameDelay
        )
    }

    $records =
        @($captureState.Records)
    if ($records.Count -ge 2) {
        $captureDurationMilliseconds =
            [int64]$records[-1].elapsedMilliseconds -
            [int64]$records[0].elapsedMilliseconds
        if ($captureDurationMilliseconds -gt 0) {
            $measuredCaptureRate =
                ($records.Count - 1) * 1000.0 /
                $captureDurationMilliseconds
        }
    }
    $minimumMeasuredCaptureRate =
        [Math]::Min(
            15.0,
            $CaptureRate * 0.5)
    if (
        $measuredCaptureRate -lt
            $minimumMeasuredCaptureRate
    ) {
        throw (
            'The measured capture rate was ' +
            ('{0:F2}' -f $measuredCaptureRate) +
            ' fps, below the required ' +
            ('{0:F2}' -f $minimumMeasuredCaptureRate) +
            ' fps.'
        )
    }
    $ready =
        Get-LastPhaseRecord `
            -Records $records `
            -Phase 'ready'
    $leftPrime =
        Get-LastPhaseRecord `
            -Records $records `
            -Phase 'left-prime'
    $rightContinuation =
        Get-LastPhaseRecord `
            -Records $records `
            -Phase 'right-continuation'
    $leftContinuation =
        Get-LastPhaseRecord `
            -Records $records `
            -Phase 'left-continuation'
    if (
        $null -eq $ready -or
        $null -eq $leftPrime -or
        $null -eq $rightContinuation -or
        $null -eq $leftContinuation
    ) {
        throw 'The drag telemetry is incomplete.'
    }
    if (
        $leftPrime.windowLeft -gt
            ($ready.windowLeft - 8)
    ) {
        throw 'The pet did not move left during left-prime.'
    }
    if (
        $rightContinuation.windowLeft -lt
            ($leftPrime.windowLeft + 25)
    ) {
        throw 'The pet did not reverse and move right.'
    }
    if (
        $leftContinuation.windowLeft -gt
            ($rightContinuation.windowLeft - 25)
    ) {
        throw 'The pet did not reverse and move left again.'
    }

    $uniqueCaptureCount =
        @(
            $records.sha256 |
                Sort-Object -Unique
        ).Count
    if ($uniqueCaptureCount -lt 6) {
        throw (
            'The rendered capture sequence contained fewer than six ' +
            'unique frames.'
        )
    }

    $dragGeometry = [ordered]@{
        readyLeft = $ready.windowLeft
        leftPrimeLeft = $leftPrime.windowLeft
        rightContinuationLeft =
            $rightContinuation.windowLeft
        leftContinuationLeft =
            $leftContinuation.windowLeft
        leftTravel =
            $leftPrime.windowLeft -
            $ready.windowLeft
        rightTravel =
            $rightContinuation.windowLeft -
            $leftPrime.windowLeft
        finalLeftTravel =
            $leftContinuation.windowLeft -
            $rightContinuation.windowLeft
        uniqueCaptureCount = $uniqueCaptureCount
    }

    $windowPolicy.after =
        Invoke-WindowVerifier `
            -ProcessId $isolatedProcess.Id `
            -JsonPath (
                Join-Path `
                    $resolvedEvidenceRoot `
                    'window-verifier-after.json'
            )

    $records |
        Export-Csv `
            -LiteralPath (
                Join-Path `
                    $resolvedEvidenceRoot `
                    'frame-inventory.csv'
            ) `
            -NoTypeInformation `
            -Encoding UTF8
} catch {
    $fatalError = $_.Exception.Message
} finally {
    if ($leftButtonHeld) {
        try {
            [CompanionPetDragNative]::
                SendLeftButton($false)
        } catch {
            if ([string]::IsNullOrWhiteSpace($fatalError)) {
                $fatalError = $_.Exception.Message
            } else {
                $fatalError +=
                    ' Mouse release failed: ' +
                    $_.Exception.Message
            }
        }
    }
    try {
        [CompanionPetDragNative]::MoveCursor(
            $originalCursor.X,
            $originalCursor.Y
        )
    } catch {
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $_.Exception.Message
        } else {
            $fatalError +=
                ' Cursor restoration failed: ' +
                $_.Exception.Message
        }
    }
    if ($activeProcessId -ne 0) {
        try {
            Stop-CompanionProcess `
                -ProcessId $activeProcessId
        } catch {
            if ([string]::IsNullOrWhiteSpace($fatalError)) {
                $fatalError = $_.Exception.Message
            } else {
                $fatalError +=
                    ' Isolated process cleanup failed: ' +
                    $_.Exception.Message
            }
        }
    }
    $productionSettingsAfterVerification =
        Get-FileFingerprint `
            -Path $productionSettingsPath
    $verificationChangedProductionSettings =
        -not (
            Test-FingerprintEqual `
                -Before $productionSettingsBaseline `
                -After $productionSettingsAfterVerification
        )
    if ($verificationChangedProductionSettings) {
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
            'The pet drag verifier changed the production settings file; the snapshot was restored.'
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
    $qtTestSettingsAfterVerification =
        Get-FileFingerprint `
            -Path $qtTestSettingsPath
    try {
        Restore-FileSnapshot `
            -Path $qtTestSettingsPath `
            -Snapshot $qtTestSettingsSnapshot
    } catch {
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $_.Exception.Message
        } else {
            $fatalError +=
                ' Qt test settings restoration failed: ' +
                $_.Exception.Message
        }
    }
    $qtTestSettingsAfterSnapshotRestore =
        Get-FileFingerprint `
            -Path $qtTestSettingsPath
    $qtTestSettingsPreserved =
        Test-FingerprintEqual `
            -Before $qtTestSettingsBaseline `
            -After $qtTestSettingsAfterSnapshotRestore
    if (-not $qtTestSettingsPreserved) {
        $message =
            'The Qt test settings snapshot could not be restored.'
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $message
        } else {
            $fatalError += ' ' + $message
        }
    }
    try {
        Remove-IsolatedProfile `
            -Path $profileRoot `
            -Root $resolvedEvidenceRoot
    } catch {
        if ([string]::IsNullOrWhiteSpace($fatalError)) {
            $fatalError = $_.Exception.Message
        } else {
            $fatalError +=
                ' Isolated profile cleanup failed: ' +
                $_.Exception.Message
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
}

$records =
    @($captureState.Records)
$phaseCounts = [ordered]@{}
foreach ($group in @(
    $records |
        Group-Object phase
)) {
    $phaseCounts[$group.Name] =
        $group.Count
}
$summary = [ordered]@{
    schemaVersion = 1
    scenario = 'pet-drag-reversal'
    passed =
        [string]::IsNullOrWhiteSpace($fatalError) -and
        $records.Count -ge 60
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
    requestedCaptureRate = $CaptureRate
    measuredCaptureRate =
        [Math]::Round(
            $measuredCaptureRate,
            3)
    captureDurationMilliseconds =
        $captureDurationMilliseconds
    captureCount = $records.Count
    phaseCounts = $phaseCounts
    dragGeometry = $dragGeometry
    exactWindowCapture = $true
    isolatedProfile = $true
    isolatedSettingsPaths =
        @($isolatedSettingsPaths)
    temporarilyStoppedPreviousInstance =
        $previousInstanceWasRunning
    restoration = $restoration
    windowPolicy = $windowPolicy
    productionSettingsBefore =
        $productionSettingsBefore
    productionSettingsBaseline =
        $productionSettingsBaseline
    productionSettingsAfterVerification =
        $productionSettingsAfterVerification
    productionSettingsAfterSnapshotRestore =
        $productionSettingsAfterSnapshotRestore
    productionSettingsAfterRestoration =
        $productionSettingsAfterRestoration
    productionSettingsPreserved =
        $productionSettingsPreserved
    qtTestSettingsBefore =
        $qtTestSettingsBefore
    qtTestSettingsBaseline =
        $qtTestSettingsBaseline
    qtTestSettingsAfterVerification =
        $qtTestSettingsAfterVerification
    qtTestSettingsAfterSnapshotRestore =
        $qtTestSettingsAfterSnapshotRestore
    qtTestSettingsPreserved =
        $qtTestSettingsPreserved
    frameInventory = 'frame-inventory.csv'
    frames = $records
    fatalError = $fatalError
}
$summaryPath =
    Join-Path `
        $resolvedEvidenceRoot `
        'pet-drag-reversal-summary.json'
Write-JsonAtomic `
    -Path $summaryPath `
    -Value $summary
Write-Output $summaryPath

if (-not $summary.passed) {
    exit 2
}
