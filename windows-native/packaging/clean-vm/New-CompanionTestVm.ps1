[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$VmName,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$BaseVhdxPath,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$VmRoot,

    [string]$SwitchName,

    [ValidateRange(2, 64)]
    [int]$ProcessorCount = 4,

    [ValidateRange(2147483648, 34359738368)]
    [UInt64]$MemoryStartupBytes = 4294967296,

    [string]$BaselineCheckpoint = 'CompanionCleanBaseline',

    [PSCredential]$GuestCredential,

    [switch]$ReplaceExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-ElevatedHost {
    $identity =
        [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal =
        New-Object Security.Principal.WindowsPrincipal($identity)
    if (
        -not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator
        )
    ) {
        throw (
            'Creating the Companion clean Windows 11 VM requires an ' +
            'elevated PowerShell host with Hyper-V management rights.'
        )
    }
}

function Assert-HyperVCmdlets {
    $required = @(
        'Get-VM'
        'New-VM'
        'Remove-VM'
        'New-VHD'
        'Set-VM'
        'Set-VMMemory'
        'Set-VMProcessor'
        'Set-VMFirmware'
        'Get-VMNetworkAdapter'
        'Get-VMSwitch'
        'Connect-VMNetworkAdapter'
        'Disconnect-VMNetworkAdapter'
        'Enable-VMIntegrationService'
        'Start-VM'
        'Stop-VM'
        'Checkpoint-VM'
        'Get-VMSnapshot'
        'New-PSSession'
    )
    foreach ($name in $required) {
        if ($null -eq (Get-Command $name -ErrorAction SilentlyContinue)) {
            throw (
                "The Hyper-V command $name is unavailable. Enable the " +
                'Hyper-V management tools before provisioning the clean VM.'
            )
        }
    }
}

function Assert-BoundedPath {
    param(
        [string]$Root,
        [string]$Candidate
    )

    $resolvedRoot =
        [System.IO.Path]::GetFullPath($Root).TrimEnd(
            [System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar
        )
    $resolvedCandidate =
        [System.IO.Path]::GetFullPath($Candidate)
    $prefix =
        $resolvedRoot +
        [System.IO.Path]::DirectorySeparatorChar
    if (
        -not $resolvedCandidate.StartsWith(
            $prefix,
            [System.StringComparison]::OrdinalIgnoreCase
        )
    ) {
        throw "The VM path escaped its configured root: $resolvedCandidate"
    }
    return $resolvedCandidate
}

function Wait-CompanionGuestSession {
    param(
        [string]$Name,
        [PSCredential]$Credential,
        [TimeSpan]$Timeout = [TimeSpan]::FromMinutes(5)
    )

    $deadline = [DateTime]::UtcNow.Add($Timeout)
    do {
        try {
            return New-PSSession `
                -VMName $Name `
                -Credential $Credential `
                -ErrorAction Stop
        } catch {
            Start-Sleep -Seconds 3
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw (
        'PowerShell Direct could not connect to the clean VM. The base ' +
        'Windows 11 image must be bootable and contain the supplied ' +
        'standard user.'
    )
}

Assert-ElevatedHost
Assert-HyperVCmdlets

if (-not (Test-Path -LiteralPath $BaseVhdxPath -PathType Leaf)) {
    throw "-BaseVhdxPath was not found: $BaseVhdxPath"
}
$resolvedBaseVhdx =
    (Resolve-Path -LiteralPath $BaseVhdxPath).ProviderPath
if (
    [System.IO.Path]::GetExtension($resolvedBaseVhdx) -ne '.vhdx'
) {
    throw '-BaseVhdxPath must identify a Windows 11 VHDX image.'
}

if (-not (Test-Path -LiteralPath $VmRoot -PathType Container)) {
    New-Item `
        -ItemType Directory `
        -Path $VmRoot `
        -Force | Out-Null
}
$resolvedVmRoot =
    (Resolve-Path -LiteralPath $VmRoot).ProviderPath
$vmDirectory = Assert-BoundedPath `
    -Root $resolvedVmRoot `
    -Candidate (Join-Path $resolvedVmRoot $VmName)
$differencingDisk = Assert-BoundedPath `
    -Root $resolvedVmRoot `
    -Candidate (Join-Path $vmDirectory 'os.vhdx')

$existingVm = Get-VM -Name $VmName -ErrorAction SilentlyContinue
if ($null -ne $existingVm) {
    if (-not $ReplaceExisting) {
        throw (
            "A VM named $VmName already exists. Pass -ReplaceExisting " +
            'only when discarding that dedicated test VM is intended.'
        )
    }
    if ($PSCmdlet.ShouldProcess($VmName, 'Remove existing test VM')) {
        if ($existingVm.State -ne 'Off') {
            Stop-VM `
                -Name $VmName `
                -TurnOff `
                -Force
        }
        Remove-VM `
            -Name $VmName `
            -Force
    }
}

if (Test-Path -LiteralPath $vmDirectory) {
    if (-not $ReplaceExisting) {
        throw (
            "The dedicated VM directory already exists: $vmDirectory"
        )
    }
    $checkedVmDirectory = Assert-BoundedPath `
        -Root $resolvedVmRoot `
        -Candidate $vmDirectory
    if (
        $PSCmdlet.ShouldProcess(
            $checkedVmDirectory,
            'Remove existing dedicated VM files'
        )
    ) {
        Remove-Item `
            -LiteralPath $checkedVmDirectory `
            -Recurse `
            -Force
    }
}

if (
    -not $PSCmdlet.ShouldProcess(
        $vmDirectory,
        'Create Companion clean Windows 11 VM'
    )
) {
    return
}

New-Item `
    -ItemType Directory `
    -Path $vmDirectory `
    -Force | Out-Null
New-VHD `
    -Path $differencingDisk `
    -ParentPath $resolvedBaseVhdx `
    -Differencing | Out-Null

$vm = New-VM `
    -Name $VmName `
    -Generation 2 `
    -Path $vmDirectory `
    -VHDPath $differencingDisk `
    -MemoryStartupBytes $MemoryStartupBytes
Set-VM `
    -VM $vm `
    -AutomaticCheckpointsEnabled $false `
    -AutomaticStartAction Nothing `
    -AutomaticStopAction ShutDown `
    -CheckpointType Standard
Set-VMMemory `
    -VMName $VmName `
    -DynamicMemoryEnabled $true `
    -MinimumBytes 2147483648 `
    -StartupBytes $MemoryStartupBytes `
    -MaximumBytes 8589934592
Set-VMProcessor `
    -VMName $VmName `
    -Count $ProcessorCount
Set-VMFirmware `
    -VMName $VmName `
    -EnableSecureBoot On `
    -SecureBootTemplate MicrosoftWindows

if (
    $null -ne (
        Get-Command Set-VMKeyProtector -ErrorAction SilentlyContinue
    ) -and
    $null -ne (
        Get-Command Enable-VMTPM -ErrorAction SilentlyContinue
    )
) {
    Set-VMKeyProtector `
        -VMName $VmName `
        -NewLocalKeyProtector
    Enable-VMTPM -VMName $VmName
}

Enable-VMIntegrationService `
    -VMName $VmName `
    -Name 'Guest Service Interface'

if ([string]::IsNullOrWhiteSpace($SwitchName)) {
    Get-VMNetworkAdapter -VMName $VmName |
        Disconnect-VMNetworkAdapter
} else {
    $switch = Get-VMSwitch `
        -Name $SwitchName `
        -ErrorAction SilentlyContinue
    if ($null -eq $switch) {
        throw "The requested Hyper-V switch was not found: $SwitchName"
    }
    Connect-VMNetworkAdapter `
        -VMName $VmName `
        -SwitchName $SwitchName
    Get-VMNetworkAdapter -VMName $VmName |
        Disconnect-VMNetworkAdapter
}

$guestFacts = $null
$checkpointCreated = $false
if ($null -ne $GuestCredential) {
    Start-VM -Name $VmName | Out-Null
    $session = Wait-CompanionGuestSession `
        -Name $VmName `
        -Credential $GuestCredential
    try {
        $guestFacts = Invoke-Command `
            -Session $session `
            -ScriptBlock {
                $identity =
                    [Security.Principal.WindowsIdentity]::GetCurrent()
                $principal =
                    New-Object Security.Principal.WindowsPrincipal(
                        $identity
                    )
                $isAdministrator =
                    $principal.IsInRole(
                        [Security.Principal.WindowsBuiltInRole]::
                            Administrator
                    )
                $os = Get-CimInstance Win32_OperatingSystem
                $build = [int]$os.BuildNumber
                $architecture = [string]$env:PROCESSOR_ARCHITECTURE
                $unexpectedTools = @(
                    'qmake.exe'
                    'cmake.exe'
                    'ninja.exe'
                    'python.exe'
                    'iscc.exe'
                    'dotnet.exe'
                ) |
                    ForEach-Object {
                        Get-Command $_ -ErrorAction SilentlyContinue
                    } |
                    Where-Object { $null -ne $_ } |
                    ForEach-Object { $_.Name }

                [pscustomobject]@{
                    identity = $identity.Name
                    isAdministrator = $isAdministrator
                    caption = [string]$os.Caption
                    build = $build
                    architecture = $architecture
                    unexpectedTools = @($unexpectedTools)
                }
            }

        if ($guestFacts.isAdministrator) {
            throw (
                'The supplied guest account is not a standard user. ' +
                'Clean release verification must not run as an administrator.'
            )
        }
        if (
            $guestFacts.build -lt 22000 -or
            $guestFacts.caption -notmatch 'Windows 11' -or
            $guestFacts.architecture -ne 'AMD64'
        ) {
            throw (
                'The base image must be Windows 11 x64 build 22000 or newer.'
            )
        }
        if (@($guestFacts.unexpectedTools).Count -gt 0) {
            throw (
                'The clean VM contains developer/runtime tools that must ' +
                'not be preinstalled: ' +
                (@($guestFacts.unexpectedTools) -join ', ')
            )
        }
    } finally {
        if ($null -ne $session) {
            Remove-PSSession $session
        }
    }

    Stop-VM `
        -Name $VmName `
        -Force
    $deadline =
        [DateTime]::UtcNow.AddMinutes(2)
    do {
        $state =
            (Get-VM -Name $VmName).State
        if ($state -eq 'Off') {
            break
        }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    if ((Get-VM -Name $VmName).State -ne 'Off') {
        throw 'The clean VM did not stop before its baseline checkpoint.'
    }

    $existingCheckpoint = Get-VMSnapshot `
        -VMName $VmName `
        -Name $BaselineCheckpoint `
        -ErrorAction SilentlyContinue
    if ($null -ne $existingCheckpoint) {
        throw (
            "The baseline checkpoint already exists: $BaselineCheckpoint"
        )
    }
    Checkpoint-VM `
        -Name $VmName `
        -SnapshotName $BaselineCheckpoint
    $checkpointCreated = $true
}

[ordered]@{
    schemaVersion = 1
    vmName = $VmName
    generation = 2
    secureBoot = $true
    networkDisconnected = $true
    baselineCheckpoint = if ($checkpointCreated) {
        $BaselineCheckpoint
    } else {
        ''
    }
    guest = $guestFacts
} | ConvertTo-Json -Depth 6
