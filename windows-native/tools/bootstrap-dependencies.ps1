param(
    [string]$QtRoot = $env:QT_ROOT,
    [string]$GitPath = $env:GIT_EXE
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($QtRoot) -or
    -not (Test-Path -LiteralPath (Join-Path $QtRoot 'bin\qt-cmake.bat'))) {
    throw 'QT_ROOT must point to Qt 6.11.1 msvc2022_64.'
}

$nativeRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $nativeRoot '.deps'
$downloadRoot = Join-Path $depsRoot 'downloads'
$sourceRoot = Join-Path $depsRoot 'source'
$buildRoot = Join-Path $depsRoot 'build'
$qtOverlay = Join-Path $depsRoot 'qt-6.11.1'
$qtVersion = '6.11.1'
$webSocketsCommit = '451920600d7f0b8a4b458bba56a2dd303e587026'
$webSocketsStampPath = Join-Path $qtOverlay 'qtwebsockets-overlay-stamp.json'
$webSocketsBuildStrategy = 'qt-cmake-single-config'
$imageFormatsArchiveName =
    '6.11.1-0-202605090529qtimageformats-Windows-Windows_11_24H2-' +
    'MSVC2022-Windows-Windows_11_24H2-X86_64.7z'
$imageFormatsArchiveUrl =
    'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/' +
    'qt6_6111/qt6_6111_msvc2022_64/' +
    'qt.qt6.6111.addons.qtimageformats.win64_msvc2022_64/' +
    $imageFormatsArchiveName
$imageFormatsArchiveSha256 =
    '8ba70600a3343fa0f1101fd304be32a58c3fe85fc6c6b316115e9d03a1be560a'
$imageFormatsStampPath = Join-Path $qtOverlay 'qtimageformats-overlay-stamp.json'
$imageFormatsPluginHashes = [ordered]@{
    'plugins\imageformats\qwebp.dll' =
        '5b3412d4df198847fa8953d7af857bbddc46e58d0cab53725821d245b65d802c'
    'plugins\imageformats\qwebpd.dll' =
        '5d8eb3945462ed09c48542fe95b3ea67bbbd9428bb07b38edda6c84ed13403a1'
}
$sourceStatePolicy = 'pristine-v1'
$gitOnPath = Get-Command git -ErrorAction SilentlyContinue
$gitCandidates = @(
    $GitPath
    $(if ($null -ne $gitOnPath) { $gitOnPath.Source })
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe'
)
$git = $gitCandidates |
    Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and
        (Test-Path -LiteralPath $_ -PathType Leaf)
    } |
    Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($git)) {
    throw 'Git executable not found on PATH, through GIT_EXE, or in Visual Studio 2022.'
}
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$qtCmake = Join-Path $QtRoot 'bin\qt-cmake.bat'

New-Item -ItemType Directory -Force -Path `
    $downloadRoot,$sourceRoot,$buildRoot,$qtOverlay | Out-Null

function Resolve-ContainedChildPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Root
    )

    $full = [IO.Path]::GetFullPath($Path)
    $rootFull = [IO.Path]::GetFullPath($Root)
    $rootFull = $rootFull.TrimEnd([char[]]@('\', '/')) +
        [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith(
            $rootFull,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing filesystem operation outside ${rootFull}: $full"
    }
    return $full
}

function Normalize-PathForStamp {
    param(
        [Parameter(Mandatory)] [string]$Path
    )

    return [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Get-LowercaseSha256 {
    param(
        [Parameter(Mandatory)] [string]$Path
    )

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).
        Hash.ToLowerInvariant()
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$Sha256,
        [Parameter(Mandatory)] [string]$AllowedRoot
    )

    $containedDestination = Resolve-ContainedChildPath `
        -Path $Destination `
        -Root $AllowedRoot
    $temporary = Resolve-ContainedChildPath `
        -Path ($containedDestination + '.download') `
        -Root $AllowedRoot

    if (Test-Path -LiteralPath $containedDestination -PathType Leaf) {
        if ((Get-LowercaseSha256 -Path $containedDestination) -eq $Sha256) {
            return $containedDestination
        }
        Remove-Item -LiteralPath $containedDestination -Force
    }
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }

    try {
        Invoke-WebRequest `
            -Uri $Url `
            -OutFile $temporary `
            -UseBasicParsing
        $actual = Get-LowercaseSha256 -Path $temporary
        if ($actual -ne $Sha256) {
            throw (
                "Downloaded dependency hash mismatch. " +
                "Expected $Sha256, got $actual.")
        }
        Move-Item `
            -LiteralPath $temporary `
            -Destination $containedDestination
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }

    return $containedDestination
}

function Invoke-WithNativeRootSubstDrive {
    param(
        [Parameter(Mandatory)] [scriptblock]$Action
    )

    $usedDrives = [IO.DriveInfo]::GetDrives().Name |
        ForEach-Object { $_.TrimEnd('\') }
    $drive = @('W:', 'X:', 'Y:', 'Z:') |
        Where-Object { $_ -notin $usedDrives } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($drive)) {
        throw 'No free subst drive letter is available for Qt dependency bootstrap.'
    }

    try {
        subst $drive $nativeRoot
        if ($LASTEXITCODE -ne 0) {
            throw "Could not map $nativeRoot to $drive."
        }
        & $Action $drive
    }
    finally {
        subst $drive /d | Out-Null
    }
}

function Remove-ContainedDirectoryIfPresent {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Root
    )

    $containedPath = Resolve-ContainedChildPath -Path $Path -Root $Root
    if (Test-Path -LiteralPath $containedPath) {
        Remove-Item -LiteralPath $containedPath -Recurse -Force
    }
}

function Invoke-GitCommand {
    param(
        [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string[]]$Arguments
    )

    $output = if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        & $git @Arguments 2>&1
    }
    else {
        & $git -C $WorkingDirectory @Arguments 2>&1
    }
    if ($LASTEXITCODE -ne 0) {
        $joinedArguments = $Arguments -join ' '
        $joinedOutput = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        if ([string]::IsNullOrWhiteSpace($joinedOutput)) {
            throw "Git command failed: git $joinedArguments"
        }
        throw "Git command failed: git $joinedArguments`n$joinedOutput"
    }

    return @($output)
}

function Get-DetachedRepositoryRecloneReason {
    param(
        [Parameter(Mandatory)] [string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Destination)) {
        return $null
    }

    if (-not (Test-Path -LiteralPath (Join-Path $Destination '.git'))) {
        return 'existing destination is not a Git repository'
    }

    try {
        $isWorkTree = (Invoke-GitCommand `
                -WorkingDirectory $Destination `
                -Arguments @('rev-parse', '--is-inside-work-tree')) -join ''
    }
    catch {
        return 'existing destination is not a Git repository'
    }

    if ($isWorkTree.Trim() -ne 'true') {
        return 'existing destination is not a Git repository'
    }

    $statusOutput = @(Invoke-GitCommand `
        -WorkingDirectory $Destination `
        -Arguments @('status', '--porcelain=v1', '--ignored', '--untracked-files=all'))
    if ($statusOutput.Count -gt 0) {
        return 'existing checkout is not pristine'
    }

    return $null
}

function Sync-DetachedRepository {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Commit,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$AllowedRoot
    )

    $containedDestination = Resolve-ContainedChildPath -Path $Destination -Root $AllowedRoot
    $recloneReason = Get-DetachedRepositoryRecloneReason -Destination $containedDestination
    if ($null -ne $recloneReason) {
        Write-Host "Recreating dependency checkout at $containedDestination because $recloneReason."
        Remove-ContainedDirectoryIfPresent -Path $containedDestination -Root $AllowedRoot
    }

    if (-not (Test-Path -LiteralPath $containedDestination)) {
        Invoke-GitCommand `
            -Arguments @('clone', '--quiet', '--no-checkout', $Url, $containedDestination) |
            Out-Null
    }

    Invoke-GitCommand `
        -WorkingDirectory $containedDestination `
        -Arguments @('fetch', '--quiet', '--depth', '1', 'origin', $Commit) |
        Out-Null
    Invoke-GitCommand `
        -WorkingDirectory $containedDestination `
        -Arguments @('checkout', '--quiet', '--detach', $Commit) |
        Out-Null

    $actual = ((Invoke-GitCommand `
                -WorkingDirectory $containedDestination `
                -Arguments @('rev-parse', 'HEAD')) -join '').Trim()
    if ($actual -ne $Commit) {
        throw "Dependency checkout mismatch. Expected $Commit, got $actual."
    }

    $postCheckoutStatus = @(Invoke-GitCommand `
        -WorkingDirectory $containedDestination `
        -Arguments @('status', '--porcelain=v1', '--ignored', '--untracked-files=all'))
    if ($postCheckoutStatus.Count -gt 0) {
        $statusSummary = ($postCheckoutStatus | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        throw "Dependency checkout is not pristine after sync: $containedDestination`n$statusSummary"
    }
}

function Get-WebSocketsOverlayRefreshReason {
    param(
        [Parameter(Mandatory)] [string]$StampPath,
        [Parameter(Mandatory)] [hashtable]$ExpectedStamp,
        [Parameter(Mandatory)] [string[]]$RequiredArtifacts,
        [Parameter(Mandatory)] [string]$OverlayRoot
    )

    if (-not (Test-Path -LiteralPath $StampPath -PathType Leaf)) {
        return 'overlay stamp is missing'
    }

    try {
        $actualStamp = Get-Content -Raw -LiteralPath $StampPath | ConvertFrom-Json
    }
    catch {
        return 'overlay stamp could not be parsed'
    }

    if ($actualStamp.qtVersion -ne $ExpectedStamp.qtVersion) {
        return 'overlay stamp Qt version does not match'
    }
    if ($actualStamp.webSocketsCommit -ne $ExpectedStamp.webSocketsCommit) {
        return 'overlay stamp WebSockets commit does not match'
    }
    if ([string]::IsNullOrWhiteSpace($actualStamp.qtRoot) -or
        $actualStamp.qtRoot -ine $ExpectedStamp.qtRoot) {
        return 'overlay stamp QT_ROOT does not match'
    }
    $actualBuildStrategyProperty = $actualStamp.PSObject.Properties['buildStrategy']
    if ($null -eq $actualBuildStrategyProperty -or
        $actualBuildStrategyProperty.Value -ne $ExpectedStamp.buildStrategy) {
        return 'overlay stamp build strategy does not match'
    }
    $actualSourceStatePolicyProperty = $actualStamp.PSObject.Properties['sourceStatePolicy']
    if ($null -eq $actualSourceStatePolicyProperty -or
        $actualSourceStatePolicyProperty.Value -ne $ExpectedStamp.sourceStatePolicy) {
        return 'overlay stamp source state policy does not match'
    }

    $actualConfigurations = @($actualStamp.requiredConfigurations)
    if ($actualConfigurations.Count -ne $ExpectedStamp.requiredConfigurations.Count) {
        return 'overlay stamp configurations do not match'
    }
    for ($i = 0; $i -lt $ExpectedStamp.requiredConfigurations.Count; $i++) {
        if ($actualConfigurations[$i] -ne $ExpectedStamp.requiredConfigurations[$i]) {
            return 'overlay stamp configurations do not match'
        }
    }

    foreach ($relativePath in $RequiredArtifacts) {
        $artifactPath = Join-Path $OverlayRoot $relativePath
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            return "required overlay artifact is missing: $relativePath"
        }
    }

    if (Get-ChildItem -LiteralPath $OverlayRoot -Recurse -File -Filter '*relwithdebinfo*' |
        Select-Object -First 1) {
        return 'overlay still contains RelWithDebInfo metadata'
    }

    return $null
}

function Write-WebSocketsOverlayStamp {
    param(
        [Parameter(Mandatory)] [string]$StampPath,
        [Parameter(Mandatory)] [hashtable]$Stamp
    )

    $Stamp | ConvertTo-Json | Set-Content -LiteralPath $StampPath -Encoding ascii
}

function Get-ImageFormatsOverlayRefreshReason {
    param(
        [Parameter(Mandatory)] [string]$StampPath,
        [Parameter(Mandatory)] [hashtable]$ExpectedStamp,
        [Parameter(Mandatory)]
        [System.Collections.IDictionary]$PluginHashes,
        [Parameter(Mandatory)] [string]$OverlayRoot
    )

    if (-not (Test-Path -LiteralPath $StampPath -PathType Leaf)) {
        return 'overlay stamp is missing'
    }

    try {
        $actualStamp = Get-Content -Raw -LiteralPath $StampPath |
            ConvertFrom-Json
    }
    catch {
        return 'overlay stamp could not be parsed'
    }

    foreach ($property in @(
        'qtVersion',
        'archiveName',
        'archiveSha256',
        'archiveUrl'
    )) {
        if ($actualStamp.$property -ne $ExpectedStamp[$property]) {
            return "overlay stamp $property does not match"
        }
    }

    foreach ($entry in $PluginHashes.GetEnumerator()) {
        $pluginPath = Join-Path $OverlayRoot $entry.Key
        if (-not (Test-Path -LiteralPath $pluginPath -PathType Leaf)) {
            return "required image-format plugin is missing: $($entry.Key)"
        }
        if ((Get-LowercaseSha256 -Path $pluginPath) -ne $entry.Value) {
            return "image-format plugin hash does not match: $($entry.Key)"
        }
    }

    return $null
}

$webSocketsSource = Join-Path $sourceRoot 'qtwebsockets'
Sync-DetachedRepository `
    -Url 'https://code.qt.io/qt/qtwebsockets.git' `
    -Commit $webSocketsCommit `
    -Destination $webSocketsSource `
    -AllowedRoot $sourceRoot

$qtOverlayPath = Resolve-ContainedChildPath -Path $qtOverlay -Root $depsRoot
$normalizedQtRoot = Normalize-PathForStamp -Path $QtRoot
$webSocketsConfigurations = @(
    @{
        Name = 'Release'
        ImportLibrary = 'Qt6WebSockets.lib'
        Runtime = 'Qt6WebSockets.dll'
        TargetMetadata = 'lib\cmake\Qt6WebSockets\Qt6WebSocketsTargets-release.cmake'
        BuildDir = Resolve-ContainedChildPath -Path (Join-Path $buildRoot 'qtwebsockets-release') -Root $buildRoot
    },
    @{
        Name = 'Debug'
        ImportLibrary = 'Qt6WebSocketsd.lib'
        Runtime = 'Qt6WebSocketsd.dll'
        TargetMetadata = 'lib\cmake\Qt6WebSockets\Qt6WebSocketsTargets-debug.cmake'
        BuildDir = Resolve-ContainedChildPath -Path (Join-Path $buildRoot 'qtwebsockets-debug') -Root $buildRoot
    }
)
$requiredWebSocketsArtifacts = @(
    'lib\cmake\Qt6WebSockets\Qt6WebSocketsConfig.cmake',
    'lib\cmake\Qt6WebSockets\Qt6WebSocketsTargets.cmake'
) + ($webSocketsConfigurations | ForEach-Object {
        @(
            $_.TargetMetadata
            ('lib\' + $_.ImportLibrary)
            ('bin\' + $_.Runtime)
        )
    })
$expectedOverlayStamp = [ordered]@{
    buildStrategy = $webSocketsBuildStrategy
    sourceStatePolicy = $sourceStatePolicy
    qtVersion = $qtVersion
    qtRoot = $normalizedQtRoot
    webSocketsCommit = $webSocketsCommit
    requiredConfigurations = @('Release', 'Debug')
}

$webSocketsOverlayRefreshReason = Get-WebSocketsOverlayRefreshReason `
    -StampPath $webSocketsStampPath `
    -ExpectedStamp $expectedOverlayStamp `
    -RequiredArtifacts $requiredWebSocketsArtifacts `
    -OverlayRoot $qtOverlayPath

if ($null -ne $webSocketsOverlayRefreshReason) {
    Write-Host "Refreshing Qt WebSockets overlay because $webSocketsOverlayRefreshReason."
    foreach ($configuration in $webSocketsConfigurations) {
        Remove-ContainedDirectoryIfPresent -Path $configuration.BuildDir -Root $buildRoot
    }
    Remove-ContainedDirectoryIfPresent -Path $qtOverlayPath -Root $depsRoot
    New-Item -ItemType Directory -Force -Path $qtOverlayPath | Out-Null

    Invoke-WithNativeRootSubstDrive {
        param($drive)

        $driveRoot = $drive + '\'
        $webSocketsSourceAlias = Join-Path $driveRoot '.deps\source\qtwebsockets'
        $qtOverlayAlias = Join-Path $driveRoot '.deps\qt-6.11.1'

        foreach ($configuration in $webSocketsConfigurations) {
            $buildAlias = Join-Path $driveRoot ('.deps\build\qtwebsockets-' + $configuration.Name.ToLowerInvariant())

            & $qtCmake `
                -S $webSocketsSourceAlias `
                -B $buildAlias `
                -G Ninja `
                "-DCMAKE_BUILD_TYPE=$($configuration.Name)" `
                "-DCMAKE_INSTALL_PREFIX=$qtOverlayAlias" `
                '-DQT_BUILD_TESTS=OFF' `
                '-DQT_BUILD_EXAMPLES=OFF'
            if ($LASTEXITCODE -ne 0) {
                throw "Qt WebSockets $($configuration.Name) configure failed."
            }

            & $cmake --build $buildAlias
            if ($LASTEXITCODE -ne 0) {
                throw "Qt WebSockets $($configuration.Name) build failed."
            }

            & $cmake --install $buildAlias
            if ($LASTEXITCODE -ne 0) {
                throw "Qt WebSockets $($configuration.Name) install failed."
            }
        }
    }

    Write-WebSocketsOverlayStamp `
        -StampPath $webSocketsStampPath `
        -Stamp $expectedOverlayStamp
}
else {
    Write-Host "Reusing verified Qt WebSockets overlay at $qtOverlayPath."
}

foreach ($relativePath in $requiredWebSocketsArtifacts) {
    $artifactPath = Join-Path $qtOverlayPath $relativePath
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
        throw "Qt WebSockets overlay artifact was not installed: $relativePath"
    }
}
if (Get-ChildItem -LiteralPath $qtOverlayPath -Recurse -File -Filter '*relwithdebinfo*' |
    Select-Object -First 1) {
    throw 'Qt WebSockets overlay still contains RelWithDebInfo metadata.'
}

$expectedImageFormatsStamp = [ordered]@{
    qtVersion = $qtVersion
    archiveName = $imageFormatsArchiveName
    archiveSha256 = $imageFormatsArchiveSha256
    archiveUrl = $imageFormatsArchiveUrl
}
$imageFormatsRefreshReason = Get-ImageFormatsOverlayRefreshReason `
    -StampPath $imageFormatsStampPath `
    -ExpectedStamp $expectedImageFormatsStamp `
    -PluginHashes $imageFormatsPluginHashes `
    -OverlayRoot $qtOverlayPath

if ($null -ne $imageFormatsRefreshReason) {
    Write-Host (
        'Refreshing Qt Image Formats overlay because ' +
        "$imageFormatsRefreshReason.")
    $imageFormatsArchive = Get-VerifiedDownload `
        -Url $imageFormatsArchiveUrl `
        -Destination (Join-Path $downloadRoot $imageFormatsArchiveName) `
        -Sha256 $imageFormatsArchiveSha256 `
        -AllowedRoot $downloadRoot
    $imageFormatsExtractRoot = Resolve-ContainedChildPath `
        -Path (Join-Path $buildRoot 'qtimageformats-package') `
        -Root $buildRoot
    Remove-ContainedDirectoryIfPresent `
        -Path $imageFormatsExtractRoot `
        -Root $buildRoot
    New-Item `
        -ItemType Directory `
        -Force `
        -Path $imageFormatsExtractRoot | Out-Null

    try {
        Push-Location $imageFormatsExtractRoot
        try {
            & $cmake -E tar xvf $imageFormatsArchive | Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw 'Qt Image Formats archive extraction failed.'
            }
        }
        finally {
            Pop-Location
        }

        foreach ($entry in $imageFormatsPluginHashes.GetEnumerator()) {
            $extractedPlugin = Join-Path $imageFormatsExtractRoot $entry.Key
            if (-not (Test-Path -LiteralPath $extractedPlugin -PathType Leaf)) {
                throw (
                    'Qt Image Formats archive is missing ' +
                    "$($entry.Key).")
            }
            $actualHash = Get-LowercaseSha256 -Path $extractedPlugin
            if ($actualHash -ne $entry.Value) {
                throw (
                    "Qt Image Formats plugin hash mismatch for " +
                    "$($entry.Key).")
            }

            $overlayPlugin = Join-Path $qtOverlayPath $entry.Key
            $overlayPluginDirectory = Split-Path -Parent $overlayPlugin
            New-Item `
                -ItemType Directory `
                -Force `
                -Path $overlayPluginDirectory | Out-Null
            Copy-Item `
                -LiteralPath $extractedPlugin `
                -Destination $overlayPlugin `
                -Force
        }

        $expectedImageFormatsStamp |
            ConvertTo-Json |
            Set-Content `
                -LiteralPath $imageFormatsStampPath `
                -Encoding ascii
    }
    finally {
        Remove-ContainedDirectoryIfPresent `
            -Path $imageFormatsExtractRoot `
            -Root $buildRoot
    }
}
else {
    Write-Host (
        'Reusing verified Qt Image Formats overlay at ' +
        "$qtOverlayPath.")
}

foreach ($entry in $imageFormatsPluginHashes.GetEnumerator()) {
    $pluginPath = Join-Path $qtOverlayPath $entry.Key
    if (-not (Test-Path -LiteralPath $pluginPath -PathType Leaf)) {
        throw (
            'Qt Image Formats overlay artifact was not installed: ' +
            "$($entry.Key)")
    }
    if ((Get-LowercaseSha256 -Path $pluginPath) -ne $entry.Value) {
        throw (
            'Qt Image Formats overlay artifact hash changed: ' +
            "$($entry.Key)")
    }
}

Sync-DetachedRepository `
    -Url 'https://github.com/LoupVaillant/Monocypher.git' `
    -Commit 'ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f' `
    -Destination (Join-Path $sourceRoot 'monocypher') `
    -AllowedRoot $sourceRoot

$foundryBootstrap = Join-Path $PSScriptRoot 'bootstrap-foundry-local.ps1'
& $foundryBootstrap
if ($LASTEXITCODE -ne 0) {
    throw 'Foundry Local dependency bootstrap failed.'
}

Write-Host "Dependencies ready under $depsRoot"
