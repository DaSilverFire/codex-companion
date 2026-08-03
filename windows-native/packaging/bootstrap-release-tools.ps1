[CmdletBinding()]
param(
    [string]$OutputPath,
    [string]$InnoRoot = $env:INNO_ROOT,
    [string]$QtRoot = $env:QT_ROOT,
    [string]$WindowsSdkRoot = $env:WindowsSdkDir,
    [switch]$InstallMissing
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$sourceRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..\..')
)
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path `
        $sourceRoot `
        'work\release\release-tools.json'
}

function Select-FirstFile {
    param([string[]]$Candidates)

    foreach ($candidate in $Candidates) {
        if (
            -not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)
        ) {
            return (Resolve-Path -LiteralPath $candidate).ProviderPath
        }
    }
    return ''
}

function Find-InnoCompiler {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($InnoRoot)) {
        $candidates += Join-Path $InnoRoot 'ISCC.exe'
    }
    $candidates += @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 7\ISCC.exe')
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 7\ISCC.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
        (Join-Path $env:ProgramFiles 'Inno Setup 7\ISCC.exe')
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    )
    $found = Select-FirstFile -Candidates $candidates
    if (
        [string]::IsNullOrWhiteSpace($found) -and
        $InstallMissing
    ) {
        & winget install `
            --id JRSoftware.InnoSetup `
            --exact `
            --silent `
            --accept-package-agreements `
            --accept-source-agreements
        if ($LASTEXITCODE -ne 0) {
            throw "winget could not install Inno Setup: $LASTEXITCODE"
        }
        $found = Select-FirstFile -Candidates $candidates
    }
    if ([string]::IsNullOrWhiteSpace($found)) {
        throw (
            'ISCC.exe was not found. Install Inno Setup or set INNO_ROOT.'
        )
    }
    return $found
}

function Find-WinDeployQt {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($QtRoot)) {
        $candidates += Join-Path $QtRoot 'bin\windeployqt.exe'
        $candidates += Join-Path $QtRoot 'windeployqt.exe'
    }
    $command = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        $candidates += $command.Source
    }
    $candidates += 'C:\Qt\6.11.1\msvc2022_64\bin\windeployqt.exe'
    $found = Select-FirstFile -Candidates $candidates
    if ([string]::IsNullOrWhiteSpace($found)) {
        throw (
            'windeployqt.exe was not found. Set QT_ROOT to the Qt kit.'
        )
    }
    return $found
}

function Find-SignTool {
    $roots = @()
    if (-not [string]::IsNullOrWhiteSpace($WindowsSdkRoot)) {
        $roots += Join-Path $WindowsSdkRoot 'bin'
    }
    $roots += @(
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin')
        (Join-Path $env:ProgramFiles 'Windows Kits\10\bin')
    )

    $candidates = @()
    foreach ($root in $roots | Select-Object -Unique) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            continue
        }
        $versionDirectories = Get-ChildItem `
            -LiteralPath $root `
            -Directory `
            -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name -match '^\d+\.\d+\.\d+\.\d+$'
            } |
            Sort-Object {
                [version]$_.Name
            } -Descending
        foreach ($directory in $versionDirectories) {
            $candidates += Join-Path `
                $directory.FullName `
                'x64\signtool.exe'
        }
        $candidates += Join-Path $root 'x64\signtool.exe'
    }
    $found = Select-FirstFile -Candidates $candidates
    if ([string]::IsNullOrWhiteSpace($found)) {
        throw (
            'The x64 Windows SDK signtool.exe was not found.'
        )
    }
    return $found
}

function Get-NumericFileVersion {
    param([string]$Path)

    $item = Get-Item -LiteralPath $Path
    $version = $item.VersionInfo.ProductVersion
    if ([string]::IsNullOrWhiteSpace($version)) {
        $version = $item.VersionInfo.FileVersion
    }
    $match = [regex]::Match([string]$version, '\d+(?:\.\d+){1,3}')
    if (-not $match.Success) {
        throw "Could not determine a numeric tool version: $Path"
    }
    return $match.Value
}

function Get-InnoCompilerVersion {
    param([string]$Path)

    $installedUninstaller = Join-Path `
        (Split-Path -Parent $Path) `
        'unins000.exe'
    if (Test-Path -LiteralPath $installedUninstaller -PathType Leaf) {
        $installedVersion = Get-NumericFileVersion `
            -Path $installedUninstaller
        if ($installedVersion -ne '0.0.0.0') {
            return $installedVersion
        }
    }
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Path
    $startInfo.Arguments = '/?'
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "Could not start the Inno Setup compiler."
    }
    $banner = (
        $process.StandardOutput.ReadToEnd() +
        $process.StandardError.ReadToEnd()
    )
    $process.WaitForExit()
    $match = [regex]::Match(
        $banner,
        'Compiler engine version:\s*(\d+(?:\.\d+){1,3})',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    if (-not $match.Success) {
        throw "Could not determine the Inno Setup compiler version."
    }
    return $match.Groups[1].Value
}

$isccPath = Find-InnoCompiler
$winDeployQtPath = Find-WinDeployQt
$signToolPath = Find-SignTool
$innoVersion = Get-InnoCompilerVersion -Path $isccPath
$qtVersion = (
    & $winDeployQtPath --version 2>&1 |
    Select-Object -First 1
).ToString().Trim()
if ($LASTEXITCODE -ne 0 -or $qtVersion -notmatch '^\d+(?:\.\d+){1,3}$') {
    throw "Could not determine the Qt deployment tool version."
}
$sdkMatch = [regex]::Match(
    $signToolPath,
    '[\\/](\d+\.\d+\.\d+\.\d+)[\\/]x64[\\/]signtool\.exe$',
    [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
)
$windowsSdkVersion = if ($sdkMatch.Success) {
    $sdkMatch.Groups[1].Value
} else {
    Get-NumericFileVersion -Path $signToolPath
}

$tools = [ordered]@{
    schemaVersion = 1
    inno = [ordered]@{
        path = $isccPath
        version = $innoVersion
    }
    qt = [ordered]@{
        path = $winDeployQtPath
        version = $qtVersion
    }
    windowsSdk = [ordered]@{
        path = $signToolPath
        version = $windowsSdkVersion
    }
}

$outputDirectory = Split-Path -Parent $OutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
    New-Item `
        -ItemType Directory `
        -Path $outputDirectory `
        -Force | Out-Null
}
$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $OutputPath,
    (($tools | ConvertTo-Json -Depth 5) + "`n"),
    $utf8WithoutBom
)

Write-Output (Resolve-Path -LiteralPath $OutputPath).ProviderPath
