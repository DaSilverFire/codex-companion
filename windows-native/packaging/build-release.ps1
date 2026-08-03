[CmdletBinding()]
param(
    [string]$Version,
    [string]$Build,
    [string]$StageDir,
    [string]$OutputDir,
    [string]$IconPath,
    [string]$IsccPath,
    [string]$SourceRoot,
    [string]$CMakeCachePath,
    [string]$RecordingPath,
    [string]$GitPath,
    [string]$SignToolPath,
    [string]$ManifestSignerPath,
    [string]$ArtifactBaseUrl,
    [string]$ReleaseVerifierPath,
    [string]$PublishedAt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$requiredValues = [ordered]@{
    Version = $Version
    Build = $Build
    StageDir = $StageDir
    OutputDir = $OutputDir
    IconPath = $IconPath
    IsccPath = $IsccPath
}
foreach ($entry in $requiredValues.GetEnumerator()) {
    if ([string]::IsNullOrWhiteSpace([string]$entry.Value)) {
        throw "-$($entry.Key) is required."
    }
}

$semVerPattern = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'
$semVerMatch = [regex]::Match(
    $Version,
    $semVerPattern,
    [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
)
if (-not $semVerMatch.Success) {
    throw '-Version must be a valid SemVer 2.0 value.'
}

function Convert-VersionComponent {
    param(
        [string]$Name,
        [string]$Value
    )

    if ($Value.Length -gt 5) {
        throw "Version $Name must be between 0 and 65535."
    }
    $number = [int]$Value
    if ($number -lt 0 -or $number -gt 65535) {
        throw "Version $Name must be between 0 and 65535."
    }
    return $number
}

$versionMajor = Convert-VersionComponent `
    -Name 'major' `
    -Value $semVerMatch.Groups[1].Value
$versionMinor = Convert-VersionComponent `
    -Name 'minor' `
    -Value $semVerMatch.Groups[2].Value
$versionPatch = Convert-VersionComponent `
    -Name 'patch' `
    -Value $semVerMatch.Groups[3].Value

if ($Build -notmatch '^\d+$' -or $Build.Length -gt 5) {
    throw '-Build must be a positive integer between 1 and 65535.'
}
$numericBuild = [int]$Build
if ($numericBuild -lt 1 -or $numericBuild -gt 65535) {
    throw '-Build must be a positive integer between 1 and 65535.'
}

$productVersion =
    "cc-update/1|$Version|$Build|w|x64|10.0.22000"
if ($productVersion.Length -gt 50) {
    throw (
        'Installer ProductVersion metadata must not ' +
        'exceed 50 characters.'
    )
}

if (-not (Test-Path -LiteralPath $StageDir -PathType Container)) {
    throw "-StageDir directory was not found: $StageDir"
}
$resolvedStageDir = (Resolve-Path -LiteralPath $StageDir).ProviderPath
$requiredStageFiles = @(
    'bin\CodexCompanion.exe'
    'bin\CodexCompanionUpdater.exe'
    'bin\Qt6Core.dll'
    'bin\Qt6Gui.dll'
    'bin\Qt6Network.dll'
    'bin\Qt6Qml.dll'
    'bin\Qt6Quick.dll'
    'bin\Qt6QuickControls2.dll'
    'bin\Qt6Sql.dll'
    'bin\Qt6WebSockets.dll'
    'bin\msvcp140.dll'
    'bin\msvcp140_1.dll'
    'bin\vcruntime140.dll'
    'bin\vcruntime140_1.dll'
    'plugins\platforms\qwindows.dll'
    'plugins\tls\qschannelbackend.dll'
    'plugins\sqldrivers\qsqlite.dll'
    'plugins\imageformats\qwebp.dll'
    'resources\skills\companion-pet\SKILL.md'
    'resources\skills\companion-pet\agents\openai.yaml'
    'resources\skills\companion-pet\references\codex-pet-schema-2026-07-13.json'
    'resources\skills\companion-pet\references\companion-contract.md'
    'resources\skills\companion-pet\scripts\companion_pet_assets.py'
)
foreach ($relativePath in $requiredStageFiles) {
    $stagedFile = Join-Path $resolvedStageDir $relativePath
    if (-not (Test-Path -LiteralPath $stagedFile -PathType Leaf)) {
        throw (
            '-StageDir is not a complete Companion stage; missing ' +
            "${relativePath}: $resolvedStageDir"
        )
    }
}
$stageQml = Join-Path $resolvedStageDir 'qml'
if (
    -not (Test-Path -LiteralPath $stageQml -PathType Container) -or
    -not (
        Get-ChildItem `
            -LiteralPath $stageQml `
            -Recurse `
            -File `
            -Force `
            -ErrorAction Stop |
        Select-Object -First 1
    )
) {
    throw "-StageDir does not contain a deployed QML tree: $resolvedStageDir"
}

$forbiddenExtensions = @(
    '.pdb'
    '.lib'
    '.exp'
    '.ilk'
    '.obj'
    '.cmake'
    '.py'
    '.ps1'
    '.pfx'
    '.p12'
    '.pem'
    '.key'
)
$stagePathPrefix = $resolvedStageDir.TrimEnd('\') + '\'
$bundledPetRoot = Join-Path $resolvedStageDir 'resources\pets'
if (Test-Path -LiteralPath $bundledPetRoot) {
    throw (
        '-StageDir must not contain bundled pet artwork: ' +
        $bundledPetRoot
    )
}
$approvedStageFiles = @(
    'resources\skills\companion-pet\scripts\companion_pet_assets.py'
)
$forbiddenStageFile = Get-ChildItem `
    -LiteralPath $resolvedStageDir `
    -Recurse `
    -File `
    -Force `
    -ErrorAction Stop |
    Where-Object {
        $relativeStagePath =
            $_.FullName.Substring($stagePathPrefix.Length)
        ($forbiddenExtensions -contains $_.Extension.ToLowerInvariant()) `
            -and ($approvedStageFiles -notcontains $relativeStagePath)
    } |
    Select-Object -First 1
if ($null -ne $forbiddenStageFile) {
    throw (
        '-StageDir contains a forbidden release file: ' +
        $forbiddenStageFile.Name
    )
}

if (-not (Test-Path -LiteralPath $IconPath -PathType Leaf)) {
    throw "-IconPath file was not found: $IconPath"
}
$resolvedIconPath = (Resolve-Path -LiteralPath $IconPath).ProviderPath

if (-not (Test-Path -LiteralPath $IsccPath -PathType Leaf)) {
    throw "-IsccPath file was not found: $IsccPath"
}
$resolvedIsccPath = (Resolve-Path -LiteralPath $IsccPath).ProviderPath

if (Test-Path -LiteralPath $OutputDir) {
    if (-not (Test-Path -LiteralPath $OutputDir -PathType Container)) {
        throw "-OutputDir must identify a directory: $OutputDir"
    }
} else {
    New-Item `
        -ItemType Directory `
        -Path $OutputDir `
        -Force | Out-Null
}
$resolvedOutputDir = (Resolve-Path -LiteralPath $OutputDir).ProviderPath

$portableRoot = Join-Path `
    $resolvedOutputDir `
    'portable\Codex Companion'
$outputPrefix = (
    [System.IO.Path]::GetFullPath($resolvedOutputDir).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    ) + [System.IO.Path]::DirectorySeparatorChar
)
$portableFullPath = [System.IO.Path]::GetFullPath($portableRoot)
if (
    -not $portableFullPath.StartsWith(
        $outputPrefix,
        [System.StringComparison]::OrdinalIgnoreCase
    )
) {
    throw 'The portable release path escaped the release output directory.'
}
if (Test-Path -LiteralPath $portableFullPath) {
    Remove-Item `
        -LiteralPath $portableFullPath `
        -Recurse `
        -Force
}
New-Item `
    -ItemType Directory `
    -Path $portableFullPath `
    -Force | Out-Null
Get-ChildItem `
    -LiteralPath $resolvedStageDir `
    -Force `
    -ErrorAction Stop |
    Copy-Item `
        -Destination $portableFullPath `
        -Recurse `
        -Force

foreach ($relativePath in $requiredStageFiles) {
    $portableFile = Join-Path $portableFullPath $relativePath
    if (-not (Test-Path -LiteralPath $portableFile -PathType Leaf)) {
        throw (
            'The portable Companion tree is incomplete after copying: ' +
            $relativePath
        )
    }
}

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $PSScriptRoot '..\..')
    )
}
if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
    throw "-SourceRoot directory was not found: $SourceRoot"
}
$resolvedSourceRoot = (Resolve-Path -LiteralPath $SourceRoot).ProviderPath

if ([string]::IsNullOrWhiteSpace($CMakeCachePath)) {
    $candidateCache = Join-Path `
        $resolvedSourceRoot `
        'work\build\windows-msvc-release\CMakeCache.txt'
    if (Test-Path -LiteralPath $candidateCache -PathType Leaf) {
        $CMakeCachePath = $candidateCache
    }
}

function Get-GitValue {
    param(
        [string]$WorkingDirectory,
        [string[]]$Arguments
    )

    if (
        [string]::IsNullOrWhiteSpace($GitPath) -or
        -not (Test-Path -LiteralPath $GitPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)
    ) {
        return ''
    }
    $value = & $GitPath -C $WorkingDirectory @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) {
        return ''
    }
    $text = ([string]$value).Trim().ToLowerInvariant()
    if ($text -notmatch '^[0-9a-f]{40,64}$') {
        return ''
    }
    return $text
}

function Test-GitClean {
    param([string]$WorkingDirectory)

    if (
        [string]::IsNullOrWhiteSpace($GitPath) -or
        -not (Test-Path -LiteralPath $GitPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)
    ) {
        return $false
    }
    $status = & $GitPath `
        -C $WorkingDirectory `
        status `
        --porcelain=v1 `
        --untracked-files=normal `
        2>$null
    if ($LASTEXITCODE -ne 0) {
        return $false
    }
    return [string]::IsNullOrWhiteSpace(
        (($status | ForEach-Object { [string]$_ }) -join "`n")
    )
}

function Get-CMakeCacheValue {
    param([string]$Name)

    if (
        [string]::IsNullOrWhiteSpace($CMakeCachePath) -or
        -not (Test-Path -LiteralPath $CMakeCachePath -PathType Leaf)
    ) {
        return ''
    }
    $match = Select-String `
        -LiteralPath $CMakeCachePath `
        -Pattern ("^" + [regex]::Escape($Name) + ':[^=]+=(.*)$') |
        Select-Object -First 1
    if ($null -eq $match) {
        return ''
    }
    return $match.Matches[0].Groups[1].Value
}

function Get-NumericFileVersion {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ''
    }
    $version = (Get-Item -LiteralPath $Path).VersionInfo.ProductVersion
    if ([string]::IsNullOrWhiteSpace($version)) {
        $version = (Get-Item -LiteralPath $Path).VersionInfo.FileVersion
    }
    $match = [regex]::Match([string]$version, '\d+(?:\.\d+){1,3}')
    if (-not $match.Success) {
        return ''
    }
    return $match.Value
}

function Get-InnoVersion {
    param([string]$Path)

    if (
        [System.IO.Path]::GetExtension($Path) -ne '.exe' -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)
    ) {
        return ''
    }
    $installedUninstaller = Join-Path `
        (Split-Path -Parent $Path) `
        'unins000.exe'
    $installedVersion = Get-NumericFileVersion `
        -Path $installedUninstaller
    if (
        -not [string]::IsNullOrWhiteSpace($installedVersion) -and
        $installedVersion -ne '0.0.0.0'
    ) {
        return $installedVersion
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
        return ''
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
        return ''
    }
    return $match.Groups[1].Value
}

$sourceIsClean = Test-GitClean `
    -WorkingDirectory $resolvedSourceRoot
$productionReleaseRequested =
    -not [string]::IsNullOrWhiteSpace(
        [string]$env:CODEX_COMPANION_SIGN_CERT_SHA1
    ) -or
    -not [string]::IsNullOrWhiteSpace(
        [string]$env:CODEX_COMPANION_TIMESTAMP_URL
    ) -or
    -not [string]::IsNullOrWhiteSpace($ReleaseVerifierPath)
if ($productionReleaseRequested -and -not $sourceIsClean) {
    throw (
        'Production release signing and verification require a clean ' +
        'Git worktree and an explicit -GitPath.'
    )
}
$sourceCommit = if ($sourceIsClean) {
    Get-GitValue `
        -WorkingDirectory $resolvedSourceRoot `
        -Arguments @('rev-parse', 'HEAD')
} else {
    ''
}
$sourceTree = if ($sourceIsClean) {
    Get-GitValue `
        -WorkingDirectory $resolvedSourceRoot `
        -Arguments @('rev-parse', 'HEAD^{tree}')
} else {
    ''
}
$webSocketsCommit = Get-GitValue `
    -WorkingDirectory (
        Join-Path $resolvedSourceRoot 'windows-native\.deps\source\qtwebsockets'
    ) `
    -Arguments @('rev-parse', 'HEAD')
$monocypherCommit = Get-GitValue `
    -WorkingDirectory (
        Join-Path $resolvedSourceRoot 'windows-native\.deps\source\monocypher'
    ) `
    -Arguments @('rev-parse', 'HEAD')

$cmakeCacheSha256 = ''
if (
    -not [string]::IsNullOrWhiteSpace($CMakeCachePath) -and
    (Test-Path -LiteralPath $CMakeCachePath -PathType Leaf)
) {
    $cmakeCacheSha256 = (
        Get-FileHash `
            -LiteralPath $CMakeCachePath `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
}

$compilerVersion = ''
$compilerPath = Get-CMakeCacheValue -Name 'CMAKE_CXX_COMPILER'
if (
    -not [string]::IsNullOrWhiteSpace($compilerPath) -and
    (Test-Path -LiteralPath $compilerPath -PathType Leaf)
) {
    $compilerFileVersion = Get-NumericFileVersion -Path $compilerPath
    if (-not [string]::IsNullOrWhiteSpace($compilerFileVersion)) {
        $compilerVersion = "MSVC $compilerFileVersion"
    }
}

$qtVersion = Get-NumericFileVersion `
    -Path (Join-Path $portableFullPath 'bin\Qt6Core.dll')
$innoVersion = Get-InnoVersion -Path $resolvedIsccPath
$windowsSdkVersion = ''
if (
    -not [string]::IsNullOrWhiteSpace($SignToolPath) -and
    (Test-Path -LiteralPath $SignToolPath -PathType Leaf)
) {
    $sdkMatch = [regex]::Match(
        $SignToolPath,
        '[\\/](\d+\.\d+\.\d+\.\d+)[\\/]x64[\\/]signtool\.exe$',
        [System.Text.RegularExpressions.RegexOptions]::IgnoreCase
    )
    if ($sdkMatch.Success) {
        $windowsSdkVersion = $sdkMatch.Groups[1].Value
    }
}

$recordingSha256 = ''
if (-not [string]::IsNullOrWhiteSpace($RecordingPath)) {
    if (-not (Test-Path -LiteralPath $RecordingPath -PathType Leaf)) {
        throw "-RecordingPath file was not found: $RecordingPath"
    }
    $recordingSha256 = (
        Get-FileHash `
            -LiteralPath $RecordingPath `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
}

$releaseMetadata = [ordered]@{
    schemaVersion = 1
    sourceCommit = $sourceCommit
    sourceTree = $sourceTree
    cmakeCacheSha256 = $cmakeCacheSha256
    compilerVersion = $compilerVersion
    qtVersion = $qtVersion
    innoVersion = $innoVersion
    windowsSdkVersion = $windowsSdkVersion
    webSocketsSourceCommit = $webSocketsCommit
    monocypherCommit = $monocypherCommit
    recordingSha256 = $recordingSha256
}
$releaseMetadataPath = Join-Path `
    $resolvedOutputDir `
    'release-metadata.json'
$utf8WithoutBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText(
    $releaseMetadataPath,
    (($releaseMetadata | ConvertTo-Json -Depth 3) + "`n"),
    $utf8WithoutBom
)

$certificateSha1 = (
    [string]$env:CODEX_COMPANION_SIGN_CERT_SHA1
).Replace(' ', '').Trim()
$timestampUrl = (
    [string]$env:CODEX_COMPANION_TIMESTAMP_URL
).Trim()
$authenticodeRequested =
    -not [string]::IsNullOrWhiteSpace($certificateSha1) -or
    -not [string]::IsNullOrWhiteSpace($timestampUrl)
$resolvedSignToolPath = ''
if ($authenticodeRequested) {
    if (
        $certificateSha1 -notmatch '^[0-9A-Fa-f]{40}$' -or
        [string]::IsNullOrWhiteSpace($timestampUrl)
    ) {
        throw (
            'Authenticode signing requires a 40-digit ' +
            'CODEX_COMPANION_SIGN_CERT_SHA1 and ' +
            'CODEX_COMPANION_TIMESTAMP_URL.'
        )
    }
    try {
        $timestampUri = [uri]$timestampUrl
    } catch {
        throw 'CODEX_COMPANION_TIMESTAMP_URL is invalid.'
    }
    # RFC 3161 responses are signed, and major public TSAs publish HTTP URLs.
    if (
        -not $timestampUri.IsAbsoluteUri -or
        $timestampUri.Scheme -notin @('http', 'https') -or
        [string]::IsNullOrWhiteSpace($timestampUri.Host) -or
        -not [string]::IsNullOrWhiteSpace($timestampUri.UserInfo)
    ) {
        throw (
            'CODEX_COMPANION_TIMESTAMP_URL must use HTTP or HTTPS ' +
            'without credentials.'
        )
    }
    if (
        [string]::IsNullOrWhiteSpace($SignToolPath) -or
        -not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)
    ) {
        throw (
            'Authenticode signing requires an existing -SignToolPath.'
        )
    }
    $resolvedSignToolPath = (
        Resolve-Path -LiteralPath $SignToolPath
    ).ProviderPath
}

function Invoke-AuthenticodeSign {
    param([string]$Path)

    & $resolvedSignToolPath sign `
        /sha1 $certificateSha1 `
        /fd SHA256 `
        /tr $timestampUrl `
        /td SHA256 `
        $Path
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool failed to sign an artifact: $LASTEXITCODE"
    }
    & $resolvedSignToolPath verify /pa /all /v $Path
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool rejected a signed artifact: $LASTEXITCODE"
    }
}

if ($authenticodeRequested) {
    Invoke-AuthenticodeSign `
        -Path (
            Join-Path $portableFullPath 'bin\CodexCompanion.exe'
        )
    Invoke-AuthenticodeSign `
        -Path (
            Join-Path $portableFullPath 'bin\CodexCompanionUpdater.exe'
        )
}

$installerScript = Join-Path $PSScriptRoot 'CodexCompanion.iss'
if (-not (Test-Path -LiteralPath $installerScript -PathType Leaf)) {
    throw "Installer source was not found: $installerScript"
}
$installerScript = (Resolve-Path -LiteralPath $installerScript).ProviderPath

$isccArguments = @(
    "/DVersion=$Version"
    "/DVersionMajor=$versionMajor"
    "/DVersionMinor=$versionMinor"
    "/DVersionPatch=$versionPatch"
    "/DBuild=$Build"
    "/DSourceDir=$portableFullPath"
    "/DOutputDir=$resolvedOutputDir"
    "/DIconPath=$resolvedIconPath"
    $installerScript
)
if ($authenticodeRequested) {
    $innoSign = (
        '$q' + $resolvedSignToolPath +
        '$q sign /sha1 ' + $certificateSha1 +
        ' /fd SHA256 /tr $q' + $timestampUrl +
        '$q /td SHA256 $f'
    )
    $isccArguments = @(
        "/Scompanion=$innoSign"
        '/DEnableSigning=1'
    ) + $isccArguments
}

& $resolvedIsccPath @isccArguments
if ($LASTEXITCODE -ne 0) {
    throw "ISCC failed with exit code $LASTEXITCODE."
}

$installerName = "Codex-Companion-$Version-$Build-windows-x64.exe"
$installerPath = Join-Path $resolvedOutputDir $installerName
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "ISCC did not produce the expected installer: $installerPath"
}
if ($authenticodeRequested) {
    & $resolvedSignToolPath verify /pa /all /v $installerPath
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool rejected the signed installer: $LASTEXITCODE"
    }
}

$privateUpdateKey = (
    [string]$env:CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64
).Trim()
$publicUpdateKey = (
    [string]$env:CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY
).Trim()
$manifestRequested =
    -not [string]::IsNullOrWhiteSpace($ManifestSignerPath) -or
    -not [string]::IsNullOrWhiteSpace($ArtifactBaseUrl) -or
    -not [string]::IsNullOrWhiteSpace($privateUpdateKey) -or
    -not [string]::IsNullOrWhiteSpace($publicUpdateKey)
$versionedManifestPath = ''
$stableManifestPath = ''
if ($manifestRequested) {
    if (
        [string]::IsNullOrWhiteSpace($ManifestSignerPath) -or
        -not (
            Test-Path `
                -LiteralPath $ManifestSignerPath `
                -PathType Leaf
        )
    ) {
        throw (
            'Manifest generation requires an existing ' +
            '-ManifestSignerPath.'
        )
    }
    if (
        [string]::IsNullOrWhiteSpace($privateUpdateKey) -or
        [string]::IsNullOrWhiteSpace($publicUpdateKey)
    ) {
        throw (
            'Manifest generation requires ' +
            'CODEX_COMPANION_WINDOWS_UPDATE_PRIVATE_KEY_BASE64 ' +
            'and CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY.'
        )
    }
    try {
        $artifactBaseUri = [uri]$ArtifactBaseUrl
    } catch {
        throw '-ArtifactBaseUrl is invalid.'
    }
    if (
        -not $artifactBaseUri.IsAbsoluteUri -or
        $artifactBaseUri.Scheme -ne 'https' -or
        -not [string]::IsNullOrWhiteSpace($artifactBaseUri.UserInfo)
    ) {
        throw '-ArtifactBaseUrl must be an HTTPS URL without credentials.'
    }
    $resolvedManifestSignerPath = (
        Resolve-Path -LiteralPath $ManifestSignerPath
    ).ProviderPath
    if ([string]::IsNullOrWhiteSpace($PublishedAt)) {
        $PublishedAt = [DateTime]::UtcNow.ToString(
            'yyyy-MM-ddTHH:mm:ssZ',
            [Globalization.CultureInfo]::InvariantCulture
        )
    }

    $installerHash = (
        Get-FileHash `
            -LiteralPath $installerPath `
            -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $installerSize = (Get-Item -LiteralPath $installerPath).Length
    $downloadUrl = (
        $ArtifactBaseUrl.TrimEnd('/') +
        '/' +
        $installerName
    )
    $versionedManifestPath = Join-Path `
        $resolvedOutputDir `
        "update-windows-x64-$Version-$Build.json"
    $stableManifestPath = Join-Path `
        $resolvedOutputDir `
        'update-windows-x64.json'

    $previousPath = $env:PATH
    try {
        $env:PATH = (
            (Join-Path $portableFullPath 'bin') +
            [System.IO.Path]::PathSeparator +
            $previousPath
        )
        & $resolvedManifestSignerPath `
            --version $Version `
            --build $Build `
            --minimum-system-version '10.0.22000' `
            --published-at $PublishedAt `
            --download-url $downloadUrl `
            --sha256 $installerHash `
            --size $installerSize `
            --output $versionedManifestPath `
            --expected-public-key $publicUpdateKey
        if ($LASTEXITCODE -ne 0) {
            throw (
                'The update manifest signer failed with exit code ' +
                $LASTEXITCODE +
                '.'
            )
        }
    } finally {
        $env:PATH = $previousPath
    }
    if (
        -not (
            Test-Path `
                -LiteralPath $versionedManifestPath `
                -PathType Leaf
        )
    ) {
        throw (
            'The update manifest signer did not create the versioned manifest.'
        )
    }
    Copy-Item `
        -LiteralPath $versionedManifestPath `
        -Destination $stableManifestPath `
        -Force
}

if (-not [string]::IsNullOrWhiteSpace($ReleaseVerifierPath)) {
    if ([string]::IsNullOrWhiteSpace($versionedManifestPath)) {
        throw (
            '-ReleaseVerifierPath requires a generated signed manifest.'
        )
    }
    if (
        -not (
            Test-Path `
                -LiteralPath $ReleaseVerifierPath `
                -PathType Leaf
        )
    ) {
        throw "-ReleaseVerifierPath was not found: $ReleaseVerifierPath"
    }
    $resolvedReleaseVerifierPath = (
        Resolve-Path -LiteralPath $ReleaseVerifierPath
    ).ProviderPath
    $evidenceDirectory = Join-Path `
        $resolvedOutputDir `
        'evidence'
    New-Item `
        -ItemType Directory `
        -Path $evidenceDirectory `
        -Force | Out-Null
    $evidencePath = Join-Path `
        $evidenceDirectory `
        'release-verification.json'
    $previousPath = $env:PATH
    try {
        $env:PATH = (
            (Join-Path $portableFullPath 'bin') +
            [System.IO.Path]::PathSeparator +
            $previousPath
        )
        & $resolvedReleaseVerifierPath `
            --installer $installerPath `
            --manifest $versionedManifestPath `
            --public-key $publicUpdateKey `
            --stage $portableFullPath `
            --metadata $releaseMetadataPath `
            --output $evidencePath
        if ($LASTEXITCODE -ne 0) {
            throw (
                'Release verification failed with exit code ' +
                $LASTEXITCODE +
                '.'
            )
        }
    } finally {
        $env:PATH = $previousPath
    }
}

Write-Output $installerPath
