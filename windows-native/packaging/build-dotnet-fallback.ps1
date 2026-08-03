[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputDir,
    [string]$PublishScriptPath,
    [string]$GitPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile {
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

function Assert-BoundedChildPath {
    param(
        [string]$Root,
        [string]$Candidate,
        [string]$Name
    )

    $rootPath = [System.IO.Path]::GetFullPath($Root).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    )
    $candidatePath = [System.IO.Path]::GetFullPath($Candidate)
    $prefix =
        $rootPath +
        [System.IO.Path]::DirectorySeparatorChar
    if (
        -not $candidatePath.StartsWith(
            $prefix,
            [System.StringComparison]::OrdinalIgnoreCase
        )
    ) {
        throw "$Name must stay below $rootPath."
    }
    return $candidatePath
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
        (($Value | ConvertTo-Json -Depth 8) + "`n"),
        $utf8WithoutBom
    )
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot '..\..')
    )
}
if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "-SourceRoot directory was not found: $SourceRoot"
}
$resolvedSourceRoot =
    (Resolve-Path -LiteralPath $SourceRoot).ProviderPath

$fallbackRoot = Join-Path `
    $resolvedSourceRoot `
    'work\release\fallback'
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = $fallbackRoot
}
$resolvedOutputDir = Assert-BoundedChildPath `
    -Root (Join-Path $resolvedSourceRoot 'work\release') `
    -Candidate $OutputDir `
    -Name 'OutputDir'

if ([string]::IsNullOrWhiteSpace($PublishScriptPath)) {
    $PublishScriptPath = Join-Path `
        $resolvedSourceRoot `
        'scripts\publish-windows.ps1'
}
$resolvedPublishScript = Resolve-ExistingFile `
    -Name 'The existing .NET release publisher' `
    -Path $PublishScriptPath

$projectPath = Resolve-ExistingFile `
    -Name 'The .NET fallback project' `
    -Path (
        Join-Path `
            $resolvedSourceRoot `
            'windows\src\CodexCompanion.App\CodexCompanion.App.csproj'
    )

[xml]$projectDocument =
    Get-Content -LiteralPath $projectPath -Raw
$versionValues = @(
    $projectDocument.Project.PropertyGroup |
        ForEach-Object { [string]$_.Version } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)
if ($versionValues.Count -ne 1) {
    throw 'The .NET fallback project must define exactly one Version.'
}
$version = $versionValues[0].Trim()
if (
    $version -notmatch
        '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$'
) {
    throw "The .NET fallback Version is not valid SemVer: $version"
}

if ([string]::IsNullOrWhiteSpace($GitPath)) {
    $gitCommand = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -ne $gitCommand) {
        $GitPath = $gitCommand.Source
    }
}
if (
    -not [string]::IsNullOrWhiteSpace($GitPath) -and
    (Test-Path -LiteralPath $GitPath -PathType Leaf)
) {
    $GitPath = (Resolve-Path -LiteralPath $GitPath).ProviderPath
} else {
    throw (
        'Git is required to prove the .NET fallback source identity and ' +
        'cleanliness.'
    )
}

$sourceScope = @(
    'windows'
    'scripts/publish-windows.ps1'
    'scripts/dotnet.ps1'
    'scripts/install-dotnet.ps1'
    'global.json'
)
function Get-ScopedGitStatus {
    if ([string]::IsNullOrWhiteSpace($GitPath)) {
        return ''
    }
    $status = & $GitPath `
        -C $resolvedSourceRoot `
        status `
        --porcelain=v1 `
        --untracked-files=normal `
        -- `
        @sourceScope
    if ($LASTEXITCODE -ne 0) {
        throw 'Git could not inspect the .NET fallback source scope.'
    }
    return (
        ($status | ForEach-Object { [string]$_ }) -join "`n"
    ).Trim()
}

function Get-GitHash {
    param([string[]]$Arguments)

    if ([string]::IsNullOrWhiteSpace($GitPath)) {
        return ''
    }
    $value = & $GitPath `
        -C $resolvedSourceRoot `
        @Arguments `
        2>$null
    if ($LASTEXITCODE -ne 0) {
        return ''
    }
    $text = ([string]$value).Trim().ToLowerInvariant()
    if ($text -notmatch '^[0-9a-f]{40,64}$') {
        return ''
    }
    return $text
}

$sourceStatusBefore = Get-ScopedGitStatus
if (-not [string]::IsNullOrWhiteSpace($sourceStatusBefore)) {
    throw (
        'The .NET fallback source scope must be clean before building. ' +
        'Build the recoverable fallback from a clean worktree.'
    )
}
$sourceCommit =
    Get-GitHash -Arguments @('rev-parse', 'HEAD')
$windowsTree =
    Get-GitHash -Arguments @(
        'rev-parse'
        'HEAD:windows'
    )
if (
    [string]::IsNullOrWhiteSpace($sourceCommit) -or
    [string]::IsNullOrWhiteSpace($windowsTree)
) {
    throw (
        'Git could not identify the .NET fallback source commit and ' +
        'windows tree.'
    )
}
$publishOutputRoot = Assert-BoundedChildPath `
    -Root (Join-Path $resolvedSourceRoot 'artifacts\windows') `
    -Candidate (
        Join-Path `
            $resolvedSourceRoot `
            "artifacts\windows\v$version-native-cutover-fallback"
    ) `
    -Name 'The existing publisher output'

& $resolvedPublishScript `
    -Version $version `
    -OutputRoot $publishOutputRoot
if ($LASTEXITCODE -ne 0) {
    throw (
        'The existing .NET release publisher failed with exit code ' +
        $LASTEXITCODE +
        '.'
    )
}

$sourceStatusAfter = Get-ScopedGitStatus
if ($sourceStatusAfter -ne $sourceStatusBefore) {
    throw (
        'The .NET fallback source scope changed while building the ' +
        'fallback artifact.'
    )
}

$fallbackReleaseRoot =
    Join-Path $resolvedSourceRoot 'work\release'
$transactionId = (
    [DateTime]::UtcNow.ToString(
        'yyyyMMddTHHmmssfffffffZ',
        [Globalization.CultureInfo]::InvariantCulture
    ) +
    '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 8)
)
$outputLeaf =
    Split-Path -Leaf $resolvedOutputDir
$transactionRoot = Assert-BoundedChildPath `
    -Root $fallbackReleaseRoot `
    -Candidate (
        Join-Path `
            (Split-Path -Parent $resolvedOutputDir) `
            ($outputLeaf + '.staging-' + $transactionId)
    ) `
    -Name 'The fallback transaction directory'
$previousOutputDir = ''
if (Test-Path -LiteralPath $resolvedOutputDir -PathType Container) {
    $previousOutputDir = Assert-BoundedChildPath `
        -Root $fallbackReleaseRoot `
        -Candidate (
            Join-Path `
                (Split-Path -Parent $resolvedOutputDir) `
                ($outputLeaf + '.previous-' + $transactionId)
        ) `
        -Name 'The previous fallback directory'
}
New-Item `
    -ItemType Directory `
    -Path $transactionRoot `
    -Force | Out-Null

Add-Type -AssemblyName System.IO.Compression.FileSystem
$artifactEvidence = @()
$verificationChecks = @(
    [ordered]@{
        id = 'fallback.source.clean'
        passed = $true
    }
    [ordered]@{
        id = 'fallback.source.identified'
        passed = $true
    }
)
foreach ($runtime in @('win-x64', 'win-arm64')) {
    $publishedDirectory = Join-Path $publishOutputRoot $runtime
    if (
        -not (
            Test-Path `
                -LiteralPath $publishedDirectory `
                -PathType Container
        )
    ) {
        throw (
            "The existing publisher did not produce the $runtime directory."
        )
    }

    $sanitizedDirectory = Join-Path `
        $transactionRoot `
        ('.staging\' + $runtime)
    New-Item `
        -ItemType Directory `
        -Path $sanitizedDirectory `
        -Force | Out-Null
    Get-ChildItem `
        -LiteralPath $publishedDirectory `
        -Force |
        Copy-Item `
            -Destination $sanitizedDirectory `
            -Recurse `
            -Force
    Get-ChildItem `
        -LiteralPath $sanitizedDirectory `
        -File `
        -Recurse `
        -Force |
        Where-Object {
            $_.Extension -in @('.pdb', '.dbg')
        } |
        Remove-Item `
            -Force

    foreach ($required in @(
        'CodexCompanion.exe'
        'coreclr.dll'
        'hostfxr.dll'
    )) {
        if (
            -not (
                Test-Path `
                    -LiteralPath (
                        Join-Path $sanitizedDirectory $required
                    ) `
                    -PathType Leaf
            )
        ) {
            throw (
                "The $runtime fallback is not self-contained; " +
                "missing $required."
            )
        }
    }
    $debugFile = Get-ChildItem `
        -LiteralPath $sanitizedDirectory `
        -File `
        -Recurse `
        -Force |
        Where-Object {
            $_.Extension -in @('.pdb', '.dbg')
        } |
        Select-Object -First 1
    if ($null -ne $debugFile) {
        throw (
            "The sanitized $runtime fallback still contains a debug file: " +
            $debugFile.Name
        )
    }

    $archiveName =
        "Codex-Companion-$version-$runtime.zip"
    $destination =
        Join-Path $transactionRoot $archiveName
    Compress-Archive `
        -Path (Join-Path $sanitizedDirectory '*') `
        -DestinationPath $destination `
        -CompressionLevel Optimal

    $zip =
        [System.IO.Compression.ZipFile]::OpenRead($destination)
    try {
        $entries = @(
            $zip.Entries |
                Where-Object {
                    -not [string]::IsNullOrWhiteSpace($_.Name)
                } |
                ForEach-Object {
                    $_.FullName.Replace('/', '\')
                }
        )
        foreach ($required in @(
            'CodexCompanion.exe'
            'coreclr.dll'
            'hostfxr.dll'
        )) {
            if (-not ($entries -contains $required)) {
                throw (
                    "The $runtime fallback archive is not self-contained; " +
                    "missing $required."
                )
            }
        }
        $debugFile = $entries |
            Where-Object { $_ -match '\.(?:pdb|dbg)$' } |
            Select-Object -First 1
        if ($null -ne $debugFile) {
            throw (
                "The $runtime fallback archive contains a debug file: " +
                $debugFile
            )
        }
    } finally {
        $zip.Dispose()
    }

    $copied = Get-Item -LiteralPath $destination
    $sha256 = (
        Get-FileHash `
            -LiteralPath $destination `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $artifactEvidence += [ordered]@{
        runtime = $runtime
        fileName = $copied.Name
        size = $copied.Length
        sha256 = $sha256
        launchCommand = 'CodexCompanion.exe'
    }
    $verificationChecks += [ordered]@{
        id = "fallback.$runtime.self_contained"
        passed = $true
    }
}
Remove-Item `
    -LiteralPath (
        Join-Path $transactionRoot '.staging'
    ) `
    -Recurse `
    -Force

$checksumLines = @(
    $artifactEvidence |
        ForEach-Object {
            "$($_.sha256)  $($_.fileName)"
        }
)
[System.IO.File]::WriteAllLines(
    (Join-Path $transactionRoot 'SHA256SUMS'),
    $checksumLines,
    [System.Text.Encoding]::ASCII
)

$dotnetVersion = ''
$localDotnet = Join-Path $resolvedSourceRoot '.dotnet\dotnet.exe'
if (Test-Path -LiteralPath $localDotnet -PathType Leaf) {
    $dotnetVersion = (
        & $localDotnet --version
    ).Trim()
}

$metadata = [ordered]@{
    schemaVersion = 1
    implementation = 'dotnet'
    version = $version
    sourceScopeClean = $true
    sourceCommit = $sourceCommit
    windowsTree = $windowsTree
    previousFallbackDirectory =
        if ([string]::IsNullOrWhiteSpace($previousOutputDir)) {
            ''
        } else {
            Split-Path -Leaf $previousOutputDir
        }
    projectSha256 = (
        Get-FileHash `
            -LiteralPath $projectPath `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    publishScriptSha256 = (
        Get-FileHash `
            -LiteralPath $resolvedPublishScript `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    dotnetVersion = $dotnetVersion
    artifacts = $artifactEvidence
}
Write-JsonFile `
    -Path (
        Join-Path $transactionRoot 'fallback-metadata.json'
    ) `
    -Value $metadata

$verification = [ordered]@{
    schemaVersion = 1
    passed = (
        @(
            $verificationChecks |
                Where-Object { -not $_.passed }
        ).Count -eq 0
    )
    version = $version
    sourceStatusBefore = $sourceStatusBefore
    sourceStatusAfter = $sourceStatusAfter
    checks = $verificationChecks
}
Write-JsonFile `
    -Path (
        Join-Path $transactionRoot 'fallback-verification.json'
    ) `
    -Value $verification

if (-not $verification.passed) {
    throw 'The .NET fallback verification did not pass.'
}

if (-not [string]::IsNullOrWhiteSpace($previousOutputDir)) {
    Move-Item `
        -LiteralPath $resolvedOutputDir `
        -Destination $previousOutputDir
}
try {
    Move-Item `
        -LiteralPath $transactionRoot `
        -Destination $resolvedOutputDir
} catch {
    if (
        -not [string]::IsNullOrWhiteSpace($previousOutputDir) -and
        (Test-Path -LiteralPath $previousOutputDir -PathType Container) -and
        -not (Test-Path -LiteralPath $resolvedOutputDir)
    ) {
        Move-Item `
            -LiteralPath $previousOutputDir `
            -Destination $resolvedOutputDir
    }
    throw
}

Write-Output $resolvedOutputDir
