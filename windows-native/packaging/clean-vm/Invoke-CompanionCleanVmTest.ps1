[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$VmName,

    [Parameter(Mandatory = $true)]
    [PSCredential]$GuestCredential,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$CurrentInstallerPath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$ManifestPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$ExpectedSignerSha256,

    [string]$PreviousInstallerPath,

    [string]$ExpectedPreviousVersion,

    [int]$ExpectedPreviousBuild = 0,

    [string]$ExpectedPreviousSha256,

    [string]$UpdateFeedUrl,

    [string]$VirtualSwitchName,

    [string]$RollbackHarnessPath,

    [string]$ExpectedRollbackHarnessSha256,

    [string]$EvidenceRoot,

    [string]$BaselineCheckpoint = 'CompanionCleanBaseline',

    [ValidateSet(100, 125, 150, 200)]
    [int[]]$DpiPercent = @(100, 125, 150, 200),

    [switch]$RequireUpdate,

    [switch]$RequireRollback
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

function Write-JsonFile {
    param(
        [string]$Path,
        [object]$Value
    )

    $utf8WithoutBom =
        New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText(
        $Path,
        (($Value | ConvertTo-Json -Depth 12) + "`n"),
        $utf8WithoutBom
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

function Wait-GuestSession {
    param(
        [TimeSpan]$Timeout = [TimeSpan]::FromMinutes(5)
    )

    $deadline = [DateTime]::UtcNow.Add($Timeout)
    do {
        try {
            return New-PSSession `
                -VMName $VmName `
                -Credential $GuestCredential `
                -ErrorAction Stop
        } catch {
            Start-Sleep -Seconds 3
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "PowerShell Direct could not connect to $VmName."
}

function Reset-CleanVm {
    $vm = Get-VM -Name $VmName -ErrorAction Stop
    if ($vm.State -ne 'Off') {
        Stop-VM `
            -Name $VmName `
            -TurnOff `
            -Force
    }
    Restore-VMSnapshot `
        -VMName $VmName `
        -Name $BaselineCheckpoint `
        -Confirm:$false
    Get-VMNetworkAdapter -VMName $VmName |
        Disconnect-VMNetworkAdapter
    Start-VM -Name $VmName | Out-Null

    $session = Wait-GuestSession
    try {
        $facts = Invoke-Command `
            -Session $session `
            -ScriptBlock {
                $identity =
                    [Security.Principal.WindowsIdentity]::GetCurrent()
                $principal =
                    New-Object Security.Principal.WindowsPrincipal(
                        $identity
                    )
                $explorer = Get-Process explorer `
                    -ErrorAction SilentlyContinue |
                    Select-Object -First 1
                [pscustomobject]@{
                    identity = $identity.Name
                    isAdministrator =
                        $principal.IsInRole(
                            [Security.Principal.WindowsBuiltInRole]::
                                Administrator
                        )
                    interactiveSession =
                        if ($null -ne $explorer) {
                            $explorer.SessionId
                        } else {
                            -1
                        }
                }
            }
        if ($facts.isAdministrator) {
            throw (
                'The clean-VM guest credential must identify a standard user.'
            )
        }
        if ($facts.interactiveSession -lt 1) {
            throw (
                'The clean VM must automatically log on the standard test ' +
                'user so visual, tray, taskbar, and DPI checks run on the ' +
                'interactive desktop.'
            )
        }
        return $session
    } catch {
        Remove-PSSession $session
        throw
    }
}

function Initialize-GuestStaging {
    param(
        [System.Management.Automation.Runspaces.PSSession]$Session,
        [string[]]$Files
    )

    $guestRoot = Invoke-Command `
        -Session $Session `
        -ScriptBlock {
            $root = Join-Path `
                $env:LOCALAPPDATA `
                'Codex Companion Clean VM'
            $localData =
                [System.IO.Path]::GetFullPath($env:LOCALAPPDATA).
                    TrimEnd('\')
            $resolved =
                [System.IO.Path]::GetFullPath($root)
            $prefix = $localData + '\'
            if (
                -not $resolved.StartsWith(
                    $prefix,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            ) {
                throw 'The guest staging path escaped LOCALAPPDATA.'
            }
            if (Test-Path -LiteralPath $resolved) {
                Remove-Item `
                    -LiteralPath $resolved `
                    -Recurse `
                    -Force
            }
            New-Item `
                -ItemType Directory `
                -Path $resolved `
                -Force | Out-Null
            $resolved
        }

    foreach ($file in $Files) {
        Copy-Item `
            -LiteralPath $file `
            -Destination $guestRoot `
            -ToSession $Session `
            -Force
    }
    return $guestRoot
}

function Invoke-SilentInstaller {
    param(
        [System.Management.Automation.Runspaces.PSSession]$Session,
        [string]$GuestInstallerPath
    )

    return Invoke-Command `
        -Session $Session `
        -ArgumentList $GuestInstallerPath `
        -ScriptBlock {
            param($InstallerPath)
            $process = Start-Process `
                -FilePath $InstallerPath `
                -ArgumentList @(
                    '/VERYSILENT'
                    '/SUPPRESSMSGBOXES'
                    '/NORESTART'
                    '/SP-'
                ) `
                -Wait `
                -PassThru
            $process.ExitCode
        }
}

function Invoke-SilentUninstall {
    param(
        [System.Management.Automation.Runspaces.PSSession]$Session
    )

    return Invoke-Command `
        -Session $Session `
        -ScriptBlock {
            $path = Join-Path `
                $env:LOCALAPPDATA `
                'Programs\Codex Companion\unins000.exe'
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                return 404
            }
            $process = Start-Process `
                -FilePath $path `
                -ArgumentList @(
                    '/VERYSILENT'
                    '/SUPPRESSMSGBOXES'
                    '/NORESTART'
                ) `
                -Wait `
                -PassThru
            $process.ExitCode
        }
}

function Invoke-InteractiveVerification {
    param(
        [System.Management.Automation.Runspaces.PSSession]$Session,
        [string]$GuestRoot,
        [string]$Phase,
        [string]$Mode,
        [int]$Scale,
        [string]$GuestSourceInstaller,
        [string]$ExpectedSourceSha256,
        [string]$BaselineUserDataPath,
        [bool]$DriveUpdateUi,
        [bool]$RequireUserData,
        [bool]$SeedDurableState = $false,
        [bool]$RequireSeededDurableState = $false,
        [bool]$RequireSensitiveLogClean = $false,
        [string]$ExpectedInstalledVersion,
        [int]$ExpectedInstalledBuild,
        [string]$InitialExpectedVersion,
        [int]$InitialExpectedBuild
    )

    $guestEvidence = Join-Path `
        $GuestRoot `
        ("evidence\" + $Phase + "-" + $Scale)
    $guestConfig = Join-Path `
        $GuestRoot `
        ("config-" + $Phase + "-" + $Scale + '.json')
    $guestVerifier = Join-Path `
        $GuestRoot `
        'verify-installed.ps1'
    $configuration = [ordered]@{
        mode = $Mode
        evidenceRoot = $guestEvidence
        expectedVersion = $ExpectedInstalledVersion
        expectedBuild = $ExpectedInstalledBuild
        initialExpectedVersion = $InitialExpectedVersion
        initialExpectedBuild = $InitialExpectedBuild
        expectedSignerSha256 =
            $ExpectedSignerSha256.ToLowerInvariant()
        sourceInstallerPath = $GuestSourceInstaller
        expectedSourceSha256 = $ExpectedSourceSha256
        baselineUserDataPath = $BaselineUserDataPath
        dpiPercent = $Scale
        timeoutSeconds = 180
        requireSettingsVisible = $Mode -ne 'Uninstalled'
        driveUpdateUi = $DriveUpdateUi
        requireUserData = $RequireUserData
        seedDurableState = $SeedDurableState
        requireSeededDurableState =
            $RequireSeededDurableState
        requireSensitiveLogClean =
            $RequireSensitiveLogClean
        leaveRunning = $false
    }
    $configuration =
        Set-CompanionUpdateFeedConfiguration `
            -Configuration $configuration `
            -DriveUpdateUi $DriveUpdateUi `
            -UpdateFeedUrl $UpdateFeedUrl
    $configurationJson =
        $configuration | ConvertTo-Json -Depth 6

    $interactiveSessionId = Invoke-Command `
        -Session $Session `
        -ArgumentList @(
            $guestConfig
            $configurationJson
            $guestEvidence
            $guestVerifier
            $Scale
        ) `
        -ScriptBlock {
            param(
                $ConfigPath,
                $ConfigJson,
                $EvidencePath,
                $VerifierPath,
                $Dpi
            )
            New-Item `
                -ItemType Directory `
                -Path $EvidencePath `
                -Force | Out-Null
            [System.IO.File]::WriteAllText(
                $ConfigPath,
                $ConfigJson,
                (New-Object System.Text.UTF8Encoding($false))
            )
            $logPixels = switch ($Dpi) {
                100 { 96 }
                125 { 120 }
                150 { 144 }
                200 { 192 }
                default {
                    throw "Unsupported DpiPercent: $Dpi"
                }
            }
            New-ItemProperty `
                -LiteralPath 'HKCU:\Control Panel\Desktop' `
                -Name Win8DpiScaling `
                -PropertyType DWord `
                -Value 1 `
                -Force | Out-Null
            New-ItemProperty `
                -LiteralPath 'HKCU:\Control Panel\Desktop' `
                -Name LogPixels `
                -PropertyType DWord `
                -Value $logPixels `
                -Force | Out-Null
            $runOnce =
                'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
            $command = (
                '"' +
                (Join-Path $PSHOME 'powershell.exe') +
                '" -NoProfile -NonInteractive -ExecutionPolicy Bypass ' +
                '-File "' +
                $VerifierPath +
                '" -ConfigurationPath "' +
                $ConfigPath +
                '"'
            )
            New-Item `
                -Path $runOnce `
                -Force | Out-Null
            Set-ItemProperty `
                -LiteralPath $runOnce `
                -Name 'CodexCompanionCleanVm' `
                -Value $command
            Remove-Item `
                -LiteralPath (
                    Join-Path $EvidencePath 'complete.json'
                ) `
                -Force `
                -ErrorAction SilentlyContinue
            $explorer = Get-Process explorer `
                -ErrorAction Stop |
                Select-Object -First 1
            $explorer.SessionId
        }

    Invoke-Command `
        -Session $Session `
        -ArgumentList $interactiveSessionId `
        -ScriptBlock {
            param($SessionId)
            & "$env:SystemRoot\System32\logoff.exe" $SessionId
        }
    Remove-PSSession $Session

    $deadline =
        [DateTime]::UtcNow.AddMinutes(15)
    $resultSession = $null
    do {
        if ($null -ne $resultSession) {
            Remove-PSSession $resultSession
            $resultSession = $null
        }
        try {
            $resultSession = Wait-GuestSession `
                -Timeout ([TimeSpan]::FromSeconds(30))
            $complete = Invoke-Command `
                -Session $resultSession `
                -ArgumentList $guestEvidence `
                -ScriptBlock {
                    param($EvidencePath)
                    $path = Join-Path $EvidencePath 'complete.json'
                    if (Test-Path -LiteralPath $path -PathType Leaf) {
                        return Get-Content -LiteralPath $path -Raw
                    }
                    return ''
                }
            if (-not [string]::IsNullOrWhiteSpace($complete)) {
                $hostEvidence = Join-Path `
                    $runEvidenceRoot `
                    ("$Phase-$Scale")
                New-Item `
                    -ItemType Directory `
                    -Path $hostEvidence `
                    -Force | Out-Null
                Copy-Item `
                    -FromSession $resultSession `
                    -Path (Join-Path $guestEvidence '*') `
                    -Destination $hostEvidence `
                    -Recurse `
                    -Force
                return [ordered]@{
                    session = $resultSession
                    report = $complete | ConvertFrom-Json
                    guestEvidence = $guestEvidence
                    hostEvidence = $hostEvidence
                }
            }
        } catch {
        }
        Start-Sleep -Seconds 5
    } while ([DateTime]::UtcNow -lt $deadline)

    if ($null -ne $resultSession) {
        Remove-PSSession $resultSession
    }
    throw (
        "Interactive clean-VM verification timed out for $Phase at " +
        "$Scale percent DPI."
    )
}

function Invoke-RollbackHarnessScenario {
    param(
        [System.Management.Automation.Runspaces.PSSession]$Session,
        [string]$HarnessPath,
        [string]$ArtifactRoot,
        [ValidateSet(
            'installer-failure',
            'acknowledgement-timeout'
        )]
        [string]$Scenario,
        [string]$ResultPath
    )

    $job = Invoke-Command `
        -Session $Session `
        -AsJob `
        -ArgumentList @(
            $HarnessPath
            $ArtifactRoot
            $Scenario
            $ResultPath
            $ExpectedSignerSha256
            $ExpectedPreviousVersion
            $ExpectedPreviousBuild
            $ExpectedPreviousSha256
        ) `
        -ScriptBlock {
            param(
                $Harness,
                $Root,
                $RequestedScenario,
                $RequestedResultPath,
                $Signer,
                $PreviousVersion,
                $PreviousBuild,
                $PreviousSha256
            )
            Remove-Item `
                -LiteralPath $RequestedResultPath `
                -Force `
                -ErrorAction SilentlyContinue
            $global:LASTEXITCODE = 0
            & $Harness `
                -ArtifactRoot $Root `
                -ExpectedSignerSha256 $Signer `
                -ExpectedPreviousVersion $PreviousVersion `
                -ExpectedPreviousBuild $PreviousBuild `
                -ExpectedPreviousSha256 $PreviousSha256 `
                -Scenarios @($RequestedScenario) `
                -ResultPath $RequestedResultPath |
                Out-Null
            [pscustomobject]@{
                exitCode =
                    if ($null -eq $LASTEXITCODE) {
                        0
                    } else {
                        [int]$LASTEXITCODE
                    }
                resultPath = $RequestedResultPath
            }
        }
    try {
        $completed =
            Wait-Job `
                -Job $job `
                -Timeout 900
        if ($null -eq $completed) {
            Stop-Job `
                -Job $job `
                -ErrorAction SilentlyContinue
            return [ordered]@{
                timedOut = $true
                state = [string]$job.State
                exitCode = -1
                resultPath = $ResultPath
            }
        }
        $payload = @(
            Receive-Job `
                -Job $job `
                -ErrorAction SilentlyContinue
        ) |
            Select-Object -Last 1
        return [ordered]@{
            timedOut = $false
            state = [string]$job.State
            exitCode = if ($null -eq $payload) {
                -1
            } else {
                [int]$payload.exitCode
            }
            resultPath = $ResultPath
        }
    } finally {
        Remove-Job `
            -Job $job `
            -Force `
            -ErrorAction SilentlyContinue
    }
}

$requiredCommands = @(
    'Get-VM'
    'Start-VM'
    'Stop-VM'
    'Get-VMSnapshot'
    'Restore-VMSnapshot'
    'Get-VMNetworkAdapter'
    'Disconnect-VMNetworkAdapter'
    'Connect-VMNetworkAdapter'
    'New-PSSession'
)
foreach ($commandName in $requiredCommands) {
    if (
        $null -eq (
            Get-Command $commandName -ErrorAction SilentlyContinue
        )
    ) {
        throw "The Hyper-V command is unavailable: $commandName"
    }
}

$currentInstaller = Resolve-RequiredFile `
    -Name 'CurrentInstallerPath' `
    -Path $CurrentInstallerPath
$resolvedManifest = Resolve-RequiredFile `
    -Name 'ManifestPath' `
    -Path $ManifestPath
$verifyScript = Resolve-RequiredFile `
    -Name 'verify-installed.ps1' `
    -Path (Join-Path $PSScriptRoot 'verify-installed.ps1')
$contractModule = Resolve-RequiredFile `
    -Name 'CleanVmContract.psm1' `
    -Path (Join-Path $PSScriptRoot 'CleanVmContract.psm1')
Import-Module `
    -Name $contractModule `
    -Force

$previousInstaller = ''
if (-not [string]::IsNullOrWhiteSpace($PreviousInstallerPath)) {
    $previousInstaller = Resolve-RequiredFile `
        -Name 'PreviousInstallerPath' `
        -Path $PreviousInstallerPath
}
$rollbackHarness = ''
if (-not [string]::IsNullOrWhiteSpace($RollbackHarnessPath)) {
    $rollbackHarness = Resolve-RequiredFile `
        -Name 'RollbackHarnessPath' `
        -Path $RollbackHarnessPath
}

$manifest =
    Get-Content -LiteralPath $resolvedManifest -Raw |
    ConvertFrom-Json
if (
    [string]::IsNullOrWhiteSpace([string]$manifest.version) -or
    [int]$manifest.build -lt 1 -or
    [string]::IsNullOrWhiteSpace([string]$manifest.downloadURL)
) {
    throw 'The release manifest is missing required signed fields.'
}
$downloadUri = [Uri][string]$manifest.downloadURL
if (
    -not $downloadUri.IsAbsoluteUri -or
    $downloadUri.Scheme -ne 'https'
) {
    throw 'The clean-VM release manifest downloadURL must use https.'
}
if (-not [string]::IsNullOrWhiteSpace($UpdateFeedUrl)) {
    $feedUri = [Uri]$UpdateFeedUrl
    if (
        -not $feedUri.IsAbsoluteUri -or
        $feedUri.Scheme -ne 'https'
    ) {
        throw '-UpdateFeedUrl must use https.'
    }
}

$currentHash = (
    Get-FileHash `
        -LiteralPath $currentInstaller `
        -Algorithm SHA256
).Hash.ToLowerInvariant()
$currentSize =
    (Get-Item -LiteralPath $currentInstaller).Length
if (
    $currentHash -ne ([string]$manifest.sha256).ToLowerInvariant() -or
    $currentSize -ne [int64]$manifest.size
) {
    throw (
        'The current installer does not match the signed manifest size ' +
        'and SHA-256.'
    )
}
$currentSignature =
    Get-AuthenticodeSignature -LiteralPath $currentInstaller
if ($currentSignature.Status -ne 'Valid') {
    throw 'The clean-VM installer must have a valid Authenticode signature.'
}
$currentSignerSha256 =
    Get-CertificateSha256 `
        -Certificate $currentSignature.SignerCertificate
if (
    $currentSignerSha256 -ne
        $ExpectedSignerSha256.ToLowerInvariant()
) {
    throw 'The clean-VM installer signer does not match the release policy.'
}
$currentVersionInfo =
    (Get-Item -LiteralPath $currentInstaller).VersionInfo
if (
    -not (
        Test-CompanionInstallerIdentity `
            -ProductVersion (
                [string]$currentVersionInfo.ProductVersion
            ) `
            -FileVersion (
                [string]$currentVersionInfo.FileVersion
            ) `
            -OriginalFilename (
                [string]$currentVersionInfo.OriginalFilename
            ) `
            -ProductName (
                [string]$currentVersionInfo.ProductName
            ) `
            -ExpectedVersion (
                [string]$manifest.version
            ) `
            -ExpectedBuild (
                [int]$manifest.build
            )
    )
) {
    throw 'The current installer metadata does not match the release manifest.'
}
$currentPeMachine =
    Get-CompanionPeMachine -Path $currentInstaller
if ($currentPeMachine -ne 0x014c) {
    throw 'The current installer does not use the expected Inno Setup PE machine.'
}

$previousInstallerEvidence = $null
if (-not [string]::IsNullOrWhiteSpace($previousInstaller)) {
    if (
        [string]::IsNullOrWhiteSpace($ExpectedPreviousVersion) -or
        $ExpectedPreviousBuild -lt 1 -or
        $ExpectedPreviousBuild -gt 65535 -or
        $ExpectedPreviousSha256 -cnotmatch '^[0-9A-Fa-f]{64}$'
    ) {
        throw (
            '-PreviousInstallerPath requires -ExpectedPreviousVersion, ' +
            '-ExpectedPreviousBuild, and -ExpectedPreviousSha256.'
        )
    }
    if (
        -not (
            Test-CompanionReleasePredecessor `
                -PreviousVersion $ExpectedPreviousVersion `
                -PreviousBuild $ExpectedPreviousBuild `
                -CurrentVersion (
                    [string]$manifest.version
                ) `
                -CurrentBuild (
                    [int]$manifest.build
                )
        )
    ) {
        throw 'The previous installer release identity is not older than N.'
    }

    $previousHash = (
        Get-FileHash `
            -LiteralPath $previousInstaller `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if (
        $previousHash -ne
            $ExpectedPreviousSha256.ToLowerInvariant()
    ) {
        throw 'The signed N-1 installer SHA-256 does not match.'
    }

    $previousSignature =
        Get-AuthenticodeSignature `
            -LiteralPath $previousInstaller
    if ($previousSignature.Status -ne 'Valid') {
        throw 'The signed N-1 installer has an invalid Authenticode signature.'
    }
    $previousSignerSha256 =
        Get-CertificateSha256 `
            -Certificate (
                $previousSignature.SignerCertificate
            )
    if (
        $previousSignerSha256 -ne
            $ExpectedSignerSha256.ToLowerInvariant()
    ) {
        throw 'The signed N-1 installer signer does not match N.'
    }

    $previousVersionInfo =
        (Get-Item -LiteralPath $previousInstaller).VersionInfo
    if (
        -not (
            Test-CompanionInstallerIdentity `
                -ProductVersion (
                    [string]$previousVersionInfo.ProductVersion
                ) `
                -FileVersion (
                    [string]$previousVersionInfo.FileVersion
                ) `
                -OriginalFilename (
                    [string]$previousVersionInfo.OriginalFilename
                ) `
                -ProductName (
                    [string]$previousVersionInfo.ProductName
                ) `
                -ExpectedVersion $ExpectedPreviousVersion `
                -ExpectedBuild $ExpectedPreviousBuild
        )
    ) {
        throw 'The signed N-1 installer metadata does not match its expected identity.'
    }
    $previousPeMachine =
        Get-CompanionPeMachine -Path $previousInstaller
    if (
        $previousPeMachine -ne 0x014c -or
        $previousPeMachine -ne $currentPeMachine
    ) {
        throw 'The signed N-1 installer PE machine does not match N.'
    }
    $previousInstallerEvidence = [ordered]@{
        version = $ExpectedPreviousVersion
        build = $ExpectedPreviousBuild
        sha256 = $previousHash
        signerSha256 = $previousSignerSha256
        peMachine = $previousPeMachine
        fileName =
            [System.IO.Path]::GetFileName(
                $previousInstaller
            )
    }
}

$rollbackHarnessEvidence = $null
if (-not [string]::IsNullOrWhiteSpace($rollbackHarness)) {
    if (
        $ExpectedRollbackHarnessSha256 -cnotmatch
            '^[0-9A-Fa-f]{64}$'
    ) {
        throw (
            '-RollbackHarnessPath requires ' +
            '-ExpectedRollbackHarnessSha256.'
        )
    }
    $rollbackExtension =
        [System.IO.Path]::GetExtension(
            $rollbackHarness
        ).ToLowerInvariant()
    if ($rollbackExtension -notin @('.ps1', '.exe')) {
        throw 'The rollback harness must be a signed .ps1 or .exe file.'
    }
    $rollbackHarnessHash = (
        Get-FileHash `
            -LiteralPath $rollbackHarness `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if (
        $rollbackHarnessHash -ne
            $ExpectedRollbackHarnessSha256.ToLowerInvariant()
    ) {
        throw 'The rollback harness SHA-256 does not match.'
    }
    $rollbackHarnessSignature =
        Get-AuthenticodeSignature `
            -LiteralPath $rollbackHarness
    if ($rollbackHarnessSignature.Status -ne 'Valid') {
        throw 'The rollback harness Authenticode signature is invalid.'
    }
    $rollbackHarnessSignerSha256 =
        Get-CertificateSha256 `
            -Certificate (
                $rollbackHarnessSignature.SignerCertificate
            )
    if (
        $rollbackHarnessSignerSha256 -ne
            $ExpectedSignerSha256.ToLowerInvariant()
    ) {
        throw 'The rollback harness signer does not match N.'
    }
    $rollbackHarnessEvidence = [ordered]@{
        fileName =
            [System.IO.Path]::GetFileName(
                $rollbackHarness
            )
        sha256 = $rollbackHarnessHash
        signerSha256 =
            $rollbackHarnessSignerSha256
    }
}

if (
    $RequireUpdate -and
    (
        [string]::IsNullOrWhiteSpace($previousInstaller) -or
        [string]::IsNullOrWhiteSpace($UpdateFeedUrl) -or
        $null -eq $previousInstallerEvidence
    )
) {
    throw (
        '-RequireUpdate requires -PreviousInstallerPath and ' +
        'its exact identity plus -UpdateFeedUrl.'
    )
}
if (
    $RequireRollback -and
    (
        [string]::IsNullOrWhiteSpace($rollbackHarness) -or
        [string]::IsNullOrWhiteSpace($previousInstaller) -or
        $null -eq $previousInstallerEvidence -or
        $null -eq $rollbackHarnessEvidence
    )
) {
    throw (
        '-RequireRollback requires -PreviousInstallerPath and a ' +
        'signed-fixture -RollbackHarnessPath.'
    )
}

if ([string]::IsNullOrWhiteSpace($EvidenceRoot)) {
    $EvidenceRoot = Join-Path `
        (Split-Path -Parent $currentInstaller) `
        'evidence\clean-vm'
}
$resolvedEvidenceRoot =
    [System.IO.Path]::GetFullPath($EvidenceRoot)
New-Item `
    -ItemType Directory `
    -Path $resolvedEvidenceRoot `
    -Force | Out-Null
$runId =
    [DateTime]::UtcNow.ToString(
        'yyyyMMddTHHmmssZ',
        [Globalization.CultureInfo]::InvariantCulture
    )
$runEvidenceRoot = Join-Path `
    $resolvedEvidenceRoot `
    $runId
New-Item `
    -ItemType Directory `
    -Path $runEvidenceRoot `
    -Force | Out-Null

$summaryChecks =
    New-Object System.Collections.ArrayList
function Add-SummaryCheck {
    param(
        [string]$Id,
        [string]$Status,
        [string]$Detail
    )

    [void]$summaryChecks.Add(
        [ordered]@{
            id = $Id
            status = $Status
            passed = $Status -eq 'passed'
            detail = $Detail
        }
    )
}

Add-SummaryCheck `
    -Id 'input.current_installer_authenticated' `
    -Status 'passed' `
    -Detail (
        "$($manifest.version).$($manifest.build) " +
        "$currentHash x64-target PE-0x014c"
    )
if ($null -ne $previousInstallerEvidence) {
    Add-SummaryCheck `
        -Id 'input.previous_installer_authenticated' `
        -Status 'passed' `
        -Detail (
            "$($previousInstallerEvidence.version)." +
            "$($previousInstallerEvidence.build) " +
            $previousInstallerEvidence.sha256
        )
}
if ($null -ne $rollbackHarnessEvidence) {
    Add-SummaryCheck `
        -Id 'input.rollback_harness_authenticated' `
        -Status 'passed' `
        -Detail (
            "$($rollbackHarnessEvidence.fileName) " +
            $rollbackHarnessEvidence.sha256
        )
}

$session = $null
$fatalError = ''
$rollbackScenarioEvidence = @()
try {
    $session = Reset-CleanVm
    $guestRoot = Initialize-GuestStaging `
        -Session $session `
        -Files @(
            $currentInstaller
            $resolvedManifest
            $verifyScript
            $contractModule
        )
    $guestCurrentInstaller = Join-Path `
        $guestRoot `
        ([System.IO.Path]::GetFileName($currentInstaller))
    $installExit = Invoke-SilentInstaller `
        -Session $session `
        -GuestInstallerPath $guestCurrentInstaller
    Add-SummaryCheck `
        -Id 'first_install.offline_standard_user' `
        -Status (
            if ($installExit -eq 0) {
                'passed'
            } else {
                'failed'
            }
        ) `
        -Detail "Installer exit code $installExit with VM network disconnected"
    if ($installExit -ne 0) {
        throw "The offline installer failed with exit code $installExit."
    }

    $baselineUserDataPath =
        Join-Path $guestRoot 'user-data-before-uninstall.json'
    foreach ($scale in $DpiPercent) {
        $verification = Invoke-InteractiveVerification `
            -Session $session `
            -GuestRoot $guestRoot `
            -Phase 'installed' `
            -Mode 'Installed' `
            -Scale $scale `
            -GuestSourceInstaller $guestCurrentInstaller `
            -ExpectedSourceSha256 $currentHash `
            -BaselineUserDataPath '' `
            -DriveUpdateUi $false `
            -RequireUserData $false `
            -ExpectedInstalledVersion (
                [string]$manifest.version
            ) `
            -ExpectedInstalledBuild (
                [int]$manifest.build
            ) `
            -InitialExpectedVersion '' `
            -InitialExpectedBuild 0
        $session = $verification.session
        Add-SummaryCheck `
            -Id "first_install.dpi_$scale" `
            -Status (
                if ($verification.report.passed) {
                    'passed'
                } else {
                    'failed'
                }
            ) `
            -Detail $verification.hostEvidence
        if ($scale -eq 100) {
            Invoke-Command `
                -Session $session `
                -ArgumentList @(
                    (Join-Path `
                        $verification.guestEvidence `
                        'user-data-snapshot.json')
                    $baselineUserDataPath
                ) `
                -ScriptBlock {
                    param($Source, $Destination)
                    Copy-Item `
                        -LiteralPath $Source `
                        -Destination $Destination `
                        -Force
                }
        }
    }

    $uninstallExit =
        Invoke-SilentUninstall -Session $session
    Add-SummaryCheck `
        -Id 'uninstall.exit' `
        -Status (
            if ($uninstallExit -eq 0) {
                'passed'
            } else {
                'failed'
            }
        ) `
        -Detail "Uninstaller exit code $uninstallExit"
    $uninstallVerification =
        Invoke-InteractiveVerification `
            -Session $session `
            -GuestRoot $guestRoot `
            -Phase 'uninstalled' `
            -Mode 'Uninstalled' `
            -Scale 100 `
            -GuestSourceInstaller $guestCurrentInstaller `
            -ExpectedSourceSha256 $currentHash `
            -BaselineUserDataPath $baselineUserDataPath `
            -DriveUpdateUi $false `
            -RequireUserData $true `
            -ExpectedInstalledVersion (
                [string]$manifest.version
            ) `
            -ExpectedInstalledBuild (
                [int]$manifest.build
            ) `
            -InitialExpectedVersion '' `
            -InitialExpectedBuild 0
    $session = $uninstallVerification.session
    Add-SummaryCheck `
        -Id 'uninstall.preserves_user_data' `
        -Status (
            if ($uninstallVerification.report.passed) {
                'passed'
            } else {
                'failed'
            }
        ) `
        -Detail $uninstallVerification.hostEvidence

    if (-not [string]::IsNullOrWhiteSpace($previousInstaller)) {
        Remove-PSSession $session
        $session = Reset-CleanVm
        $guestRoot = Initialize-GuestStaging `
            -Session $session `
            -Files @(
                $previousInstaller
                $currentInstaller
                $resolvedManifest
                $verifyScript
                $contractModule
            )
        $guestPreviousInstaller = Join-Path `
            $guestRoot `
            ([System.IO.Path]::GetFileName($previousInstaller))
        $guestCurrentInstaller = Join-Path `
            $guestRoot `
            ([System.IO.Path]::GetFileName($currentInstaller))
        $previousExit = Invoke-SilentInstaller `
            -Session $session `
            -GuestInstallerPath $guestPreviousInstaller
        if ($previousExit -ne 0) {
            throw "The signed N-1 installer failed with exit code $previousExit."
        }

        $beforeUpdateVerification =
            Invoke-InteractiveVerification `
                -Session $session `
                -GuestRoot $guestRoot `
                -Phase 'update-before' `
                -Mode 'Installed' `
                -Scale 100 `
                -GuestSourceInstaller (
                    $guestPreviousInstaller
                ) `
                -ExpectedSourceSha256 (
                    $previousInstallerEvidence.sha256
                ) `
                -BaselineUserDataPath '' `
                -DriveUpdateUi $false `
                -RequireUserData $true `
                -SeedDurableState $true `
                -RequireSeededDurableState $true `
                -RequireSensitiveLogClean $false `
                -ExpectedInstalledVersion (
                    $ExpectedPreviousVersion
                ) `
                -ExpectedInstalledBuild (
                    $ExpectedPreviousBuild
                ) `
                -InitialExpectedVersion '' `
                -InitialExpectedBuild 0
        $session =
            $beforeUpdateVerification.session
        Add-SummaryCheck `
            -Id 'update.seeded_n_minus_1_state' `
            -Status (
                if (
                    $beforeUpdateVerification.report.passed
                ) {
                    'passed'
                } else {
                    'failed'
                }
            ) `
            -Detail (
                $beforeUpdateVerification.hostEvidence
            )
        if (-not $beforeUpdateVerification.report.passed) {
            throw (
                'The seeded N-1 persistence baseline verification failed.'
            )
        }
        $updateBaselinePath =
            Join-Path `
                $beforeUpdateVerification.guestEvidence `
                'user-data-snapshot.json'

        if ([string]::IsNullOrWhiteSpace($VirtualSwitchName)) {
            Connect-VMNetworkAdapter -VMName $VmName
        } else {
            Connect-VMNetworkAdapter `
                -VMName $VmName `
                -SwitchName $VirtualSwitchName
        }
        $updateVerification =
            Invoke-InteractiveVerification `
                -Session $session `
                -GuestRoot $guestRoot `
                -Phase 'update' `
                -Mode 'Update' `
                -Scale 100 `
                -GuestSourceInstaller $guestCurrentInstaller `
                -ExpectedSourceSha256 $currentHash `
                -BaselineUserDataPath (
                    $updateBaselinePath
                ) `
                -DriveUpdateUi $true `
                -RequireUserData $true `
                -SeedDurableState $false `
                -RequireSeededDurableState $true `
                -RequireSensitiveLogClean $true `
                -ExpectedInstalledVersion (
                    [string]$manifest.version
                ) `
                -ExpectedInstalledBuild (
                    [int]$manifest.build
                ) `
                -InitialExpectedVersion (
                    $ExpectedPreviousVersion
                ) `
                -InitialExpectedBuild (
                    $ExpectedPreviousBuild
                )
        $session = $updateVerification.session
        Get-VMNetworkAdapter -VMName $VmName |
            Disconnect-VMNetworkAdapter
        Add-SummaryCheck `
            -Id 'update.n_minus_1_to_n' `
            -Status (
                if ($updateVerification.report.passed) {
                    'passed'
                } else {
                    'failed'
                }
            ) `
            -Detail $updateVerification.hostEvidence
    } else {
        Add-SummaryCheck `
            -Id 'update.n_minus_1_to_n' `
            -Status (
                if ($RequireUpdate) {
                    'failed'
                } else {
                    'skipped'
                }
            ) `
            -Detail 'No PreviousInstallerPath was supplied.'
    }

    $rollbackScenarioIds = [ordered]@{
        'installer-failure' =
            'rollback.installer-failure'
        'acknowledgement-timeout' =
            'rollback.acknowledgement-timeout'
    }
    $rollbackScenarioEvidence = @()
    $rollbackPassed = $true
    if (-not [string]::IsNullOrWhiteSpace($rollbackHarness)) {
        foreach ($scenario in $rollbackScenarioIds.Keys) {
            $scenarioPassed = $false
            $scenarioDetail = ''
            try {
                if ($null -ne $session) {
                    Remove-PSSession $session
                    $session = $null
                }
                $session = Reset-CleanVm
                $guestRoot = Initialize-GuestStaging `
                    -Session $session `
                    -Files @(
                        $previousInstaller
                        $currentInstaller
                        $resolvedManifest
                        $verifyScript
                        $contractModule
                        $rollbackHarness
                    )
                $guestPreviousInstaller = Join-Path `
                    $guestRoot `
                    (
                        [System.IO.Path]::GetFileName(
                            $previousInstaller
                        )
                    )
                $guestRollbackHarness = Join-Path `
                    $guestRoot `
                    (
                        [System.IO.Path]::GetFileName(
                            $rollbackHarness
                        )
                    )

                $previousExit = Invoke-SilentInstaller `
                    -Session $session `
                    -GuestInstallerPath (
                        $guestPreviousInstaller
                    )
                if ($previousExit -ne 0) {
                    throw (
                        "The signed N-1 installer failed with exit code " +
                        "$previousExit before $scenario."
                    )
                }

                $beforeVerification =
                    Invoke-InteractiveVerification `
                        -Session $session `
                        -GuestRoot $guestRoot `
                        -Phase (
                            "rollback-$scenario-before"
                        ) `
                        -Mode 'Installed' `
                        -Scale 100 `
                        -GuestSourceInstaller (
                            $guestPreviousInstaller
                        ) `
                        -ExpectedSourceSha256 (
                            $previousInstallerEvidence.sha256
                        ) `
                        -BaselineUserDataPath '' `
                        -DriveUpdateUi $false `
                        -RequireUserData $true `
                        -ExpectedInstalledVersion (
                            $ExpectedPreviousVersion
                        ) `
                        -ExpectedInstalledBuild (
                            $ExpectedPreviousBuild
                        ) `
                        -InitialExpectedVersion '' `
                        -InitialExpectedBuild 0
                $session = $beforeVerification.session
                if (-not $beforeVerification.report.passed) {
                    throw (
                        "The N-1 baseline verification failed before " +
                        "$scenario."
                    )
                }
                $baselinePath =
                    Join-Path `
                        $beforeVerification.guestEvidence `
                        'user-data-snapshot.json'
                $guestResultPath =
                    Join-Path `
                        $guestRoot `
                        "rollback-$scenario-result.json"
                $execution =
                    Invoke-RollbackHarnessScenario `
                        -Session $session `
                        -HarnessPath (
                            $guestRollbackHarness
                        ) `
                        -ArtifactRoot $guestRoot `
                        -Scenario $scenario `
                        -ResultPath $guestResultPath
                $resultJson = Invoke-Command `
                    -Session $session `
                    -ArgumentList $guestResultPath `
                    -ScriptBlock {
                        param($Path)
                        if (
                            Test-Path `
                                -LiteralPath $Path `
                                -PathType Leaf
                        ) {
                            return Get-Content `
                                -LiteralPath $Path `
                                -Raw
                        }
                        return ''
                    }
                $resultRecord = $null
                $resultValid = $false
                if (
                    -not [string]::IsNullOrWhiteSpace(
                        $resultJson
                    )
                ) {
                    try {
                        $resultRecord =
                            $resultJson |
                            ConvertFrom-Json
                        $resultValid =
                            Test-CompanionRollbackResult `
                                -Result $resultRecord `
                                -Scenario $scenario `
                                -ExpectedVersion (
                                    $ExpectedPreviousVersion
                                ) `
                                -ExpectedBuild (
                                    $ExpectedPreviousBuild
                                )
                    } catch {
                        $resultValid = $false
                    }
                }

                $afterVerification =
                    Invoke-InteractiveVerification `
                        -Session $session `
                        -GuestRoot $guestRoot `
                        -Phase (
                            "rollback-$scenario-after"
                        ) `
                        -Mode 'Installed' `
                        -Scale 100 `
                        -GuestSourceInstaller (
                            $guestPreviousInstaller
                        ) `
                        -ExpectedSourceSha256 (
                            $previousInstallerEvidence.sha256
                        ) `
                        -BaselineUserDataPath (
                            $baselinePath
                        ) `
                        -DriveUpdateUi $false `
                        -RequireUserData $true `
                        -ExpectedInstalledVersion (
                            $ExpectedPreviousVersion
                        ) `
                        -ExpectedInstalledBuild (
                            $ExpectedPreviousBuild
                        ) `
                        -InitialExpectedVersion '' `
                        -InitialExpectedBuild 0
                $session = $afterVerification.session

                $scenarioPassed =
                    -not $execution.timedOut -and
                    $execution.state -eq 'Completed' -and
                    $execution.exitCode -eq 0 -and
                    $resultValid -and
                    $afterVerification.report.passed
                $scenarioDetail = (
                    "job=$($execution.state), " +
                    "exit=$($execution.exitCode), " +
                    "result=$resultValid, " +
                    "post=$($afterVerification.report.passed)"
                )
                if ($resultValid) {
                    $hostResultPath =
                        Join-Path `
                            $runEvidenceRoot `
                            "rollback-$scenario-result.json"
                    Copy-Item `
                        -FromSession $session `
                        -LiteralPath $guestResultPath `
                        -Destination $hostResultPath `
                        -Force
                    $rollbackScenarioEvidence +=
                        [ordered]@{
                            scenario = $scenario
                            result =
                                [System.IO.Path]::GetFileName(
                                    $hostResultPath
                                )
                            before =
                                $beforeVerification.hostEvidence
                            after =
                                $afterVerification.hostEvidence
                        }
                }
            } catch {
                $scenarioDetail =
                    $_.Exception.Message
            }

            $rollbackPassed =
                $rollbackPassed -and
                $scenarioPassed
            Add-SummaryCheck `
                -Id $rollbackScenarioIds[$scenario] `
                -Status (
                    if ($scenarioPassed) {
                        'passed'
                    } else {
                        'failed'
                    }
                ) `
                -Detail $scenarioDetail
        }
        Add-SummaryCheck `
            -Id 'rollback.injected_failures' `
            -Status (
                if ($rollbackPassed) {
                    'passed'
                } else {
                    'failed'
                }
            ) `
            -Detail (
                'Both failure modes ran from separate clean snapshots.'
            )
    } else {
        $rollbackStatus =
            if ($RequireRollback) {
                'failed'
            } else {
                'skipped'
            }
        foreach ($scenario in $rollbackScenarioIds.Keys) {
            Add-SummaryCheck `
                -Id $rollbackScenarioIds[$scenario] `
                -Status $rollbackStatus `
                -Detail (
                    'No authenticated rollback fixture harness was supplied.'
                )
        }
        Add-SummaryCheck `
            -Id 'rollback.injected_failures' `
            -Status $rollbackStatus `
            -Detail (
                'No authenticated rollback fixture harness was supplied.'
            )
    }
} catch {
    $fatalError = $_.Exception.Message
    Add-SummaryCheck `
        -Id 'clean_vm.unhandled_exception' `
        -Status 'failed' `
        -Detail $fatalError
} finally {
    if ($null -ne $session) {
        Remove-PSSession $session
    }
    Get-VMNetworkAdapter `
        -VMName $VmName `
        -ErrorAction SilentlyContinue |
        Disconnect-VMNetworkAdapter `
            -ErrorAction SilentlyContinue
}

$failedChecks = @(
    $summaryChecks |
        Where-Object { $_.status -eq 'failed' }
)
$summary = [ordered]@{
    schemaVersion = 1
    passed = $failedChecks.Count -eq 0
    vmName = $VmName
    baselineCheckpoint = $BaselineCheckpoint
    version = [string]$manifest.version
    build = [int]$manifest.build
    installerSha256 = $currentHash
    installerSize = $currentSize
    currentPeMachine = $currentPeMachine
    previousInstaller = $previousInstallerEvidence
    rollbackHarness = $rollbackHarnessEvidence
    rollbackScenarios = @($rollbackScenarioEvidence)
    manifestSha256 = (
        Get-FileHash `
            -LiteralPath $resolvedManifest `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    signerSha256 = $currentSignerSha256
    updateFeedUrl = $UpdateFeedUrl
    fatalError = $fatalError
    dpiPercent = @($DpiPercent)
    checks = @($summaryChecks)
}
$summaryPath = Join-Path `
    $runEvidenceRoot `
    'clean-vm-summary.json'
Write-JsonFile `
    -Path $summaryPath `
    -Value $summary
Write-Output $summaryPath

if (-not $summary.passed) {
    exit 2
}
