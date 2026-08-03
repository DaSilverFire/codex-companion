param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$nativeRoot = Split-Path -Parent $PSScriptRoot
$depsRoot = Join-Path $nativeRoot '.deps'
$foundryVersion = '1.2.1'
$foundryParent = Join-Path $depsRoot 'foundry-local'
$foundryRoot = Join-Path $foundryParent $foundryVersion
$foundryStamp = Join-Path $foundryRoot 'bootstrap-stamp.json'
$vcpkgBaseline = 'a9f0cd0345fb29cd227d802f1fd1917c28f8e5a3'
$vcpkgParent = Join-Path $depsRoot 'vcpkg'
$vcpkgRoot = Join-Path $vcpkgParent $vcpkgBaseline
$vcpkgSource = Join-Path $vcpkgRoot 'source'
$vcpkgInstalled = Join-Path $vcpkgRoot 'installed'
$vcpkgRegistry = Join-Path $vcpkgRoot 'registry'
$vcpkgStamp = Join-Path $vcpkgRoot 'bootstrap-stamp.json'
$triplet = 'x64-windows-static-md'

$foundrySource = @{
    Url = 'https://github.com/microsoft/Foundry-Local/archive/refs/tags/v1.2.1.tar.gz'
    File = 'foundry-local-v1.2.1.tar.gz'
    Sha256 = '9206e71571bb6b80aff296165fd9dfe7b869dfab1775d4bda8a10ef71db3995b'
}
$packages = @(
    @{
        Name = 'Microsoft.AI.Foundry.Local.Core'
        Version = '1.2.1'
        Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ai.foundry.local.core/1.2.1/microsoft.ai.foundry.local.core.1.2.1.nupkg'
        File = 'Microsoft.AI.Foundry.Local.Core.1.2.1.nupkg'
        Sha256 = '01aac1f3cfaababdcbabad8dd1cd3831fa19119d5b55f9fc3b3c4f6af3a3c5d2'
    },
    @{
        Name = 'Microsoft.ML.OnnxRuntime.Foundry'
        Version = '1.26.0'
        Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.foundry/1.26.0/microsoft.ml.onnxruntime.foundry.1.26.0.nupkg'
        File = 'Microsoft.ML.OnnxRuntime.Foundry.1.26.0.nupkg'
        Sha256 = '62205a727541f1ba149babc4518dd2d6efd84c8f9f4a221f1f8fe6c60e781224'
    },
    @{
        Name = 'Microsoft.ML.OnnxRuntimeGenAI.Foundry'
        Version = '0.14.1'
        Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntimegenai.foundry/0.14.1/microsoft.ml.onnxruntimegenai.foundry.0.14.1.nupkg'
        File = 'Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1.nupkg'
        Sha256 = '073ec41aeaa466b82ec23c890d4e7c7c2f15b8e589b5626c5f3562f14805fe87'
    }
)
$vcpkgArchive = @{
    Url = "https://github.com/microsoft/vcpkg/archive/$vcpkgBaseline.zip"
    File = "vcpkg-$vcpkgBaseline.zip"
    Sha256 = '43058154ec2640211d7da75014c7154b7fda07e8371d639f83f18cdfdb769bcd'
}

function Resolve-ContainedPath {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Root
    )

    $full = [IO.Path]::GetFullPath($Path)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing filesystem operation outside ${rootFull}: $full"
    }
    return $full
}

function Remove-ContainedDirectory {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Root
    )

    $resolved = Resolve-ContainedPath -Path $Path -Root $Root
    if (Test-Path -LiteralPath $resolved) {
        [IO.Directory]::Delete(
            (Convert-ToExtendedPath -Path $resolved),
            $true)
    }
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
        throw 'No free subst drive letter is available for the Foundry Local bootstrap.'
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

function Get-Sha256 {
    param([Parameter(Mandatory)] [string]$Path)

    $stream = [IO.File]::OpenRead(
        (Convert-ToExtendedPath -Path $Path))
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return (
                [BitConverter]::ToString(
                    $sha.ComputeHash($stream))
            ).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $sha.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Test-FileExists {
    param([Parameter(Mandatory)] [string]$Path)

    return [IO.File]::Exists(
        (Convert-ToExtendedPath -Path $Path))
}

function Assert-FileHash {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [string]$Expected
    )

    if (-not (Test-FileExists -Path $Path)) {
        throw "Required dependency file is missing: $Path"
    }
    $actual = Get-Sha256 -Path $Path
    if ($actual -ne $Expected.ToLowerInvariant()) {
        throw "Dependency hash mismatch for $Path. Expected $Expected, got $actual."
    }
}

function Save-VerifiedDownload {
    param(
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Destination,
        [Parameter(Mandatory)] [string]$Sha256
    )

    Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    Assert-FileHash -Path $Destination -Expected $Sha256
}

function Convert-ToExtendedPath {
    param([Parameter(Mandatory)] [string]$Path)

    $full = [IO.Path]::GetFullPath($Path)
    if ($full.StartsWith('\\?\')) {
        return $full
    }
    if ($full.StartsWith('\\')) {
        return '\\?\UNC\' + $full.TrimStart('\')
    }
    return '\\?\' + $full
}

function Expand-SafeZip {
    param(
        [Parameter(Mandatory)] [string]$Archive,
        [Parameter(Mandatory)] [string]$Destination
    )

    $destinationRoot = [IO.Path]::GetFullPath($Destination)
    [IO.Directory]::CreateDirectory(
        (Convert-ToExtendedPath -Path $destinationRoot)) | Out-Null
    $archiveObject = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        foreach ($entry in $archiveObject.Entries) {
            $relative = $entry.FullName.Replace('/', [IO.Path]::DirectorySeparatorChar)
            if ([string]::IsNullOrWhiteSpace($relative)) {
                continue
            }
            $target = Resolve-ContainedPath `
                -Path (Join-Path $destinationRoot $relative) `
                -Root $destinationRoot
            if ($entry.FullName.EndsWith('/')) {
                [IO.Directory]::CreateDirectory(
                    (Convert-ToExtendedPath -Path $target)) | Out-Null
                continue
            }
            [IO.Directory]::CreateDirectory(
                (Convert-ToExtendedPath -Path (Split-Path -Parent $target))) | Out-Null
            $input = $entry.Open()
            try {
                $output = [IO.File]::Open(
                    (Convert-ToExtendedPath -Path $target),
                    [IO.FileMode]::Create,
                    [IO.FileAccess]::Write,
                    [IO.FileShare]::None)
                try {
                    $input.CopyTo($output)
                }
                finally {
                    $output.Dispose()
                }
            }
            finally {
                $input.Dispose()
            }
        }
    }
    finally {
        $archiveObject.Dispose()
    }
}

function Assert-FoundryManifest {
    param([Parameter(Mandatory)] [string]$Root)

    $checks = @(
        @('source\LICENSE', 'dbb4186093080916f718e921b8e3b50be0396faf297b3d16bec5d2f47b8f81cb'),
        @('packages\Microsoft.AI.Foundry.Local.Core.1.2.1\runtimes\win-x64\native\Microsoft.AI.Foundry.Local.Core.dll', '171e368e7e4579e60946bae9a1836d6634beb01b7422d69948094b6e49c355c7'),
        @('packages\Microsoft.AI.Foundry.Local.Core.1.2.1\LICENSE.txt', '833bf9711b2e75bf0b1ab667618ee4f4ea2a6f0fa4fb5f828f7221a0cbfb7702'),
        @('packages\Microsoft.ML.OnnxRuntime.Foundry.1.26.0\runtimes\win-x64\native\onnxruntime.dll', '6a4129504501cbd615efddc897345ec9557390b408887165ab5faf9812a54b31'),
        @('packages\Microsoft.ML.OnnxRuntime.Foundry.1.26.0\runtimes\win-x64\native\onnxruntime_providers_shared.dll', '97fc0ccc43386f8769a0afc43fb1dba3a066f718cd1fe0e8f540e24e0ecb61a7'),
        @('packages\Microsoft.ML.OnnxRuntime.Foundry.1.26.0\LICENSE', 'c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674'),
        @('packages\Microsoft.ML.OnnxRuntime.Foundry.1.26.0\ThirdPartyNotices.txt', 'fb0af774b4d7cffc5b9d046f2aaeade2f37df2f80abf8033c95dfffcc77a8866'),
        @('packages\Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1\runtimes\win-x64\native\onnxruntime-genai.dll', '762a76aa622eb2e7b1ee977752e1a30f763669d1037e9be18d036668e0b1ef27'),
        @('packages\Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1\LICENSE', '9906940f61b1f0b533fa7d99baf55178b2808fbe113ea51dfbfad8572ccd5f2b'),
        @('packages\Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1\ThirdPartyNotices.txt', 'c3adb0951a5964aa6092b88faa07efdb0037074bb0092e92d1fc7ace31e9c542')
    )
    foreach ($check in $checks) {
        Assert-FileHash -Path (Join-Path $Root $check[0]) -Expected $check[1]
    }
}

function Test-FoundryReady {
    if ($Force -or -not (Test-FileExists -Path $foundryStamp)) {
        return $false
    }
    try {
        Assert-FoundryManifest -Root $foundryRoot
        return Test-FileExists -Path (Join-Path $foundryRoot 'source\sdk\cpp\src\model.cpp')
    }
    catch {
        return $false
    }
}

function Install-FoundryPackages {
    if (Test-FoundryReady) {
        Write-Host "Reusing verified Foundry Local payload at $foundryRoot."
        return
    }

    New-Item -ItemType Directory -Force -Path $foundryParent | Out-Null
    $temporaryRoot = Join-Path $foundryParent ('.bootstrap-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    try {
        $downloadRoot = Join-Path $temporaryRoot 'downloads'
        $payloadRoot = Join-Path $temporaryRoot 'payload'
        New-Item -ItemType Directory -Force -Path $downloadRoot,$payloadRoot | Out-Null

        $sourceArchive = Join-Path $downloadRoot $foundrySource.File
        Save-VerifiedDownload -Url $foundrySource.Url -Destination $sourceArchive -Sha256 $foundrySource.Sha256
        $sourceExtract = Join-Path $temporaryRoot 'source-extract'
        New-Item -ItemType Directory -Force -Path $sourceExtract | Out-Null
        $tar = (Get-Command tar.exe -ErrorAction Stop).Source
        & $tar -xzf $sourceArchive -C $sourceExtract
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not extract the verified Foundry Local source archive.'
        }
        $sourceDirectory = Join-Path $sourceExtract 'foundry-local-1.2.1'
        if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'sdk\cpp\src\model.cpp') -PathType Leaf)) {
            throw 'The Foundry Local source archive has an unexpected layout.'
        }
        Move-Item -LiteralPath $sourceDirectory -Destination (Join-Path $payloadRoot 'source')

        $packageRoot = Join-Path $payloadRoot 'packages'
        New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null
        foreach ($package in $packages) {
            $archive = Join-Path $downloadRoot $package.File
            Save-VerifiedDownload -Url $package.Url -Destination $archive -Sha256 $package.Sha256
            $destination = Join-Path $packageRoot ($package.Name + '.' + $package.Version)
            Expand-SafeZip -Archive $archive -Destination $destination
        }

        Assert-FoundryManifest -Root $payloadRoot
        [ordered]@{
            foundryVersion = $foundryVersion
            sourceCommit = 'e3e134c0f56b18523bec5f2b28b8e921080dda23'
            sourceSha256 = $foundrySource.Sha256
            corePackageSha256 = $packages[0].Sha256
            onnxRuntimePackageSha256 = $packages[1].Sha256
            onnxRuntimeGenAiPackageSha256 = $packages[2].Sha256
        } | ConvertTo-Json | Set-Content `
            -LiteralPath (Join-Path $payloadRoot 'bootstrap-stamp.json') `
            -Encoding ascii

        Remove-ContainedDirectory -Path $foundryRoot -Root $foundryParent
        Move-Item -LiteralPath $payloadRoot -Destination $foundryRoot
        Write-Host "Installed verified Foundry Local payload at $foundryRoot."
    }
    finally {
        Remove-ContainedDirectory -Path $temporaryRoot -Root $foundryParent
    }
}

function Test-VcpkgReady {
    if ($Force -or -not (Test-FileExists -Path $vcpkgStamp)) {
        return $false
    }
    $required = @(
        (Join-Path $vcpkgSource 'vcpkg.exe'),
        (Join-Path $vcpkgInstalled 'vcpkg\status'),
        (Join-Path $vcpkgInstalled "$triplet\include\nlohmann\json.hpp"),
        (Join-Path $vcpkgInstalled "$triplet\include\gsl\gsl"),
        (Join-Path $vcpkgRegistry 'versions\baseline.json')
    )
    return @($required | Where-Object { -not (Test-FileExists -Path $_) }).Count -eq 0
}

function New-PinnedFilesystemRegistry {
    $definitions = @(
        @{
            Name = 'ms-gsl'
            VersionField = 'version'
            Version = '4.2.1'
            PortVersion = 0
            Bucket = 'm-'
        },
        @{
            Name = 'nlohmann-json'
            VersionField = 'version-semver'
            Version = '3.12.0'
            PortVersion = 2
            Bucket = 'n-'
        },
        @{
            Name = 'vcpkg-cmake'
            VersionField = 'version-date'
            Version = '2024-04-23'
            PortVersion = 0
            Bucket = 'v-'
        },
        @{
            Name = 'vcpkg-cmake-config'
            VersionField = 'version-date'
            Version = '2024-05-23'
            PortVersion = 0
            Bucket = 'v-'
        }
    )

    Remove-ContainedDirectory -Path $vcpkgRegistry -Root $vcpkgRoot
    $portsRoot = Join-Path $vcpkgRegistry 'ports'
    $versionsRoot = Join-Path $vcpkgRegistry 'versions'
    New-Item -ItemType Directory -Force -Path $portsRoot,$versionsRoot | Out-Null

    $baselineEntries = [ordered]@{}
    foreach ($definition in $definitions) {
        $sourcePort = Join-Path (Join-Path $vcpkgSource 'ports') $definition.Name
        $destinationPort = Join-Path $portsRoot $definition.Name
        if (-not (Test-Path -LiteralPath (Join-Path $sourcePort 'vcpkg.json') -PathType Leaf)) {
            throw "Pinned vcpkg port is missing: $($definition.Name)"
        }
        Copy-Item -LiteralPath $sourcePort -Destination $destinationPort -Recurse

        $baselineEntries[$definition.Name] = [ordered]@{
            baseline = $definition.Version
            'port-version' = $definition.PortVersion
        }
        $versionEntry = [ordered]@{
            $definition.VersionField = $definition.Version
            'port-version' = $definition.PortVersion
            path = '$/ports/' + $definition.Name
        }
        $bucketRoot = Join-Path $versionsRoot $definition.Bucket
        New-Item -ItemType Directory -Force -Path $bucketRoot | Out-Null
        [ordered]@{
            versions = @($versionEntry)
        } | ConvertTo-Json -Depth 6 | Set-Content `
            -LiteralPath (Join-Path $bucketRoot ($definition.Name + '.json')) `
            -Encoding ascii
    }

    [ordered]@{
        $vcpkgBaseline = $baselineEntries
    } | ConvertTo-Json -Depth 6 | Set-Content `
        -LiteralPath (Join-Path $versionsRoot 'baseline.json') `
        -Encoding ascii
}

function Install-PinnedVcpkg {
    if (Test-VcpkgReady) {
        Write-Host "Reusing verified pinned vcpkg tree at $vcpkgRoot."
        return
    }

    New-Item -ItemType Directory -Force -Path $vcpkgParent | Out-Null
    $temporaryRoot = Join-Path $vcpkgParent ('.bootstrap-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temporaryRoot | Out-Null
    try {
        $archive = Join-Path $temporaryRoot $vcpkgArchive.File
        Save-VerifiedDownload -Url $vcpkgArchive.Url -Destination $archive -Sha256 $vcpkgArchive.Sha256
        $expanded = Join-Path $temporaryRoot 'expanded'
        Expand-SafeZip -Archive $archive -Destination $expanded
        $sourceDirectory = Join-Path $expanded ("vcpkg-" + $vcpkgBaseline)
        if (-not (Test-Path -LiteralPath (Join-Path $sourceDirectory 'bootstrap-vcpkg.bat') -PathType Leaf)) {
            throw 'The pinned vcpkg archive has an unexpected layout.'
        }

        Remove-ContainedDirectory -Path $vcpkgRoot -Root $vcpkgParent
        New-Item -ItemType Directory -Force -Path $vcpkgRoot | Out-Null
        Move-Item -LiteralPath $sourceDirectory -Destination $vcpkgSource
        New-PinnedFilesystemRegistry

        Invoke-WithNativeRootSubstDrive {
            param($drive)

            $driveRoot = $drive + '\'
            $sourceAlias = Join-Path $driveRoot ('.deps\vcpkg\' + $vcpkgBaseline + '\source')
            $installedAlias = Join-Path $driveRoot ('.deps\vcpkg\' + $vcpkgBaseline + '\installed')
            $previousVcpkgRoot = $env:VCPKG_ROOT
            $env:VCPKG_ROOT = $sourceAlias
            Push-Location $sourceAlias
            try {
                & (Join-Path $sourceAlias 'bootstrap-vcpkg.bat') -disableMetrics
                if ($LASTEXITCODE -ne 0) {
                    throw 'Pinned vcpkg bootstrap failed.'
                }
                & (Join-Path $sourceAlias 'vcpkg.exe') install `
                    "--x-manifest-root=$driveRoot" `
                    "--triplet=$triplet" `
                    "--x-install-root=$installedAlias" `
                    "--vcpkg-root=$sourceAlias"
                if ($LASTEXITCODE -ne 0) {
                    throw 'Pinned vcpkg manifest install failed.'
                }
            }
            finally {
                Pop-Location
                $env:VCPKG_ROOT = $previousVcpkgRoot
            }
        }

        [ordered]@{
            baseline = $vcpkgBaseline
            archiveSha256 = $vcpkgArchive.Sha256
            triplet = $triplet
            manifestSha256 = Get-Sha256 -Path (Join-Path $nativeRoot 'vcpkg.json')
            configurationSha256 = Get-Sha256 -Path (Join-Path $nativeRoot 'vcpkg-configuration.json')
        } | ConvertTo-Json | Set-Content -LiteralPath $vcpkgStamp -Encoding ascii
        if (-not (Test-VcpkgReady)) {
            throw 'Pinned vcpkg installation did not produce the required files.'
        }
        Write-Host "Installed pinned vcpkg dependencies at $vcpkgInstalled."
    }
    finally {
        Remove-ContainedDirectory -Path $temporaryRoot -Root $vcpkgParent
    }
}

New-Item -ItemType Directory -Force -Path $depsRoot | Out-Null
Install-FoundryPackages
Install-PinnedVcpkg
Write-Host 'Foundry Local dependencies are ready.'
