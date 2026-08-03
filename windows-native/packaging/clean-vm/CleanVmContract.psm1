Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-CompanionHttpsUrl {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [string]$Value,

        [switch]$AllowEmpty
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        if ($AllowEmpty) {
            return ''
        }
        throw "$Name is required."
    }

    $uri = $null
    try {
        $uri = [Uri]$Value
    } catch {
        throw "$Name must be an absolute HTTPS URL."
    }
    if (
        -not $uri.IsAbsoluteUri -or
        $uri.Scheme -ne 'https' -or
        [string]::IsNullOrWhiteSpace($uri.Host) -or
        -not [string]::IsNullOrWhiteSpace($uri.UserInfo) -or
        -not [string]::IsNullOrWhiteSpace($uri.Fragment)
    ) {
        throw "$Name must be an absolute HTTPS URL without credentials or a fragment."
    }
    return $uri.AbsoluteUri
}

function Set-CompanionUpdateFeedConfiguration {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Configuration,

        [bool]$DriveUpdateUi,

        [string]$UpdateFeedUrl
    )

    $normalized = ConvertTo-CompanionHttpsUrl `
        -Name 'UpdateFeedUrl' `
        -Value $UpdateFeedUrl `
        -AllowEmpty
    if (
        $DriveUpdateUi -and
        [string]::IsNullOrWhiteSpace($normalized)
    ) {
        throw 'UpdateFeedUrl is required when the update UI is driven.'
    }
    $Configuration['updateFeedUrl'] = $normalized
    return $Configuration
}

function Get-CompanionLaunchArguments {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Installed', 'Update', 'Uninstalled')]
        [string]$Mode,

        [string]$UpdateFeedUrl
    )

    if ($Mode -ne 'Update') {
        return @()
    }
    $normalized = ConvertTo-CompanionHttpsUrl `
        -Name 'UpdateFeedUrl' `
        -Value $UpdateFeedUrl
    return @(
        '--update-manifest-url'
        $normalized
    )
}

function ConvertTo-CompanionFullPath {
    param([string]$Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return ''
    }
    try {
        return [System.IO.Path]::GetFullPath($Value)
    } catch {
        return ''
    }
}

function Test-CompanionPathEqual {
    param(
        [string]$Left,
        [string]$Right
    )

    $leftFull = ConvertTo-CompanionFullPath -Value $Left
    $rightFull = ConvertTo-CompanionFullPath -Value $Right
    return (
        -not [string]::IsNullOrWhiteSpace($leftFull) -and
        -not [string]::IsNullOrWhiteSpace($rightFull) -and
        $leftFull.Equals(
            $rightFull,
            [System.StringComparison]::OrdinalIgnoreCase
        )
    )
}

function Test-CompanionShortcutIdentity {
    param(
        [string]$TargetPath,
        [string]$IconLocation,
        [string]$WorkingDirectory,
        [string]$ExecutablePath
    )

    $executableFull =
        ConvertTo-CompanionFullPath -Value $ExecutablePath
    if (
        [string]::IsNullOrWhiteSpace($executableFull) -or
        -not (
            Test-CompanionPathEqual `
                -Left $TargetPath `
                -Right $executableFull
        ) -or
        -not (
            Test-CompanionPathEqual `
                -Left $WorkingDirectory `
                -Right (
                    Split-Path -Parent $executableFull
                )
        )
    ) {
        return $false
    }

    $iconValue = ([string]$IconLocation).Trim()
    $separatorIndex = $iconValue.LastIndexOf(',')
    if ($separatorIndex -lt 1) {
        return $false
    }
    $iconIndex = 0
    if (
        -not (
            [int]::TryParse(
                $iconValue.Substring(
                    $separatorIndex + 1
                ).Trim(),
                [ref]$iconIndex
            )
        ) -or
        $iconIndex -ne 0
    ) {
        return $false
    }

    $iconPath =
        $iconValue.Substring(
            0,
            $separatorIndex
        ).Trim()
    $startsQuoted =
        $iconPath.StartsWith(
            '"',
            [System.StringComparison]::Ordinal
        )
    $endsQuoted =
        $iconPath.EndsWith(
            '"',
            [System.StringComparison]::Ordinal
        )
    if ($startsQuoted -ne $endsQuoted) {
        return $false
    }
    if ($startsQuoted) {
        if ($iconPath.Length -lt 3) {
            return $false
        }
        $iconPath =
            $iconPath.Substring(
                1,
                $iconPath.Length - 2
            )
    }

    return (
        Test-CompanionPathEqual `
            -Left $iconPath `
            -Right $executableFull
    )
}

function Get-CompanionObjectPropertyNames {
    param([object]$Value)

    if ($null -eq $Value) {
        return @()
    }
    if ($Value -is [System.Collections.IDictionary]) {
        return @(
            $Value.Keys |
                ForEach-Object { [string]$_ }
        )
    }
    return @(
        $Value.PSObject.Properties |
            ForEach-Object { [string]$_.Name }
    )
}

function Get-CompanionObjectPropertyValue {
    param(
        [object]$Value,
        [string]$Name
    )

    if ($Value -is [System.Collections.IDictionary]) {
        return $Value[$Name]
    }
    $property = $Value.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Test-CompanionObjectHasExactProperties {
    param(
        [object]$Value,
        [string[]]$Expected
    )

    $actual = @(Get-CompanionObjectPropertyNames -Value $Value)
    if ($actual.Count -ne $Expected.Count) {
        return $false
    }
    foreach ($name in $Expected) {
        if ($name -notin $actual) {
            return $false
        }
    }
    return $true
}

function Test-CompanionVersionIdentity {
    param(
        [string]$ProductVersion,
        [string]$FileVersion,
        [string]$ExpectedVersion,
        [int]$ExpectedBuild
    )

    if (
        [string]::IsNullOrWhiteSpace($ExpectedVersion) -or
        $ExpectedBuild -lt 0
    ) {
        return $false
    }
    try {
        $base = [Version]$ExpectedVersion
        if ($base.Build -lt 0) {
            return $false
        }
        $expected =
            [Version](
                "$($base.Major).$($base.Minor)." +
                "$($base.Build).$ExpectedBuild"
            )
        return (
            ([Version]$ProductVersion).Equals($expected) -and
            ([Version]$FileVersion).Equals($expected)
        )
    } catch {
        return $false
    }
}

function ConvertTo-CompanionReleaseVersion {
    param(
        [string]$Version,
        [int]$Build
    )

    if (
        $Version -cnotmatch '^\d+\.\d+\.\d+$' -or
        $Build -lt 1 -or
        $Build -gt 65535
    ) {
        return $null
    }
    try {
        $base = [Version]$Version
        if (
            $base.Major -gt 65535 -or
            $base.Minor -gt 65535 -or
            $base.Build -gt 65535
        ) {
            return $null
        }
        return [Version](
            "$($base.Major).$($base.Minor)." +
            "$($base.Build).$Build"
        )
    } catch {
        return $null
    }
}

function Test-CompanionInstallerIdentity {
    param(
        [string]$ProductVersion,
        [string]$FileVersion,
        [string]$OriginalFilename,
        [string]$ProductName,
        [string]$ExpectedVersion,
        [int]$ExpectedBuild
    )

    $expectedNumeric =
        ConvertTo-CompanionReleaseVersion `
            -Version $ExpectedVersion `
            -Build $ExpectedBuild
    if ($null -eq $expectedNumeric) {
        return $false
    }
    $productVersionValue = ([string]$ProductVersion).Trim()
    $fileVersionValue = ([string]$FileVersion).Trim()
    $originalFilenameValue = ([string]$OriginalFilename).Trim()
    $productNameValue = ([string]$ProductName).Trim()
    try {
        $fileNumeric = [Version]$fileVersionValue
    } catch {
        return $false
    }
    $expectedProductVersion =
        "cc-update/1|$ExpectedVersion|" +
        "$ExpectedBuild|w|x64|10.0.22000"
    $expectedFilename =
        "Codex-Companion-$ExpectedVersion-" +
        "$ExpectedBuild-windows-x64.exe"
    return (
        $productVersionValue -ceq $expectedProductVersion -and
        $fileNumeric.Equals($expectedNumeric) -and
        $originalFilenameValue.Equals(
            $expectedFilename,
            [System.StringComparison]::OrdinalIgnoreCase
        ) -and
        $productNameValue -ceq 'Codex Companion'
    )
}

function Test-CompanionReleasePredecessor {
    param(
        [string]$PreviousVersion,
        [int]$PreviousBuild,
        [string]$CurrentVersion,
        [int]$CurrentBuild
    )

    $previous =
        ConvertTo-CompanionReleaseVersion `
            -Version $PreviousVersion `
            -Build $PreviousBuild
    $current =
        ConvertTo-CompanionReleaseVersion `
            -Version $CurrentVersion `
            -Build $CurrentBuild
    return (
        $null -ne $previous -and
        $null -ne $current -and
        $previous.CompareTo($current) -lt 0
    )
}

function Get-CompanionPeMachine {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $fullPath = ConvertTo-CompanionFullPath -Value $Path
    if (
        [string]::IsNullOrWhiteSpace($fullPath) -or
        -not (Test-Path -LiteralPath $fullPath -PathType Leaf)
    ) {
        return 0
    }

    $stream = $null
    $reader = $null
    try {
        $stream =
            [System.IO.File]::Open(
                $fullPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read `
                    -bor `
                    [System.IO.FileShare]::Delete
            )
        if ($stream.Length -lt 136) {
            return 0
        }
        $reader =
            New-Object System.IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d) {
            return 0
        }
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        if (
            $peOffset -lt 0 -or
            $peOffset -gt ($stream.Length - 6)
        ) {
            return 0
        }
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            return 0
        }
        return [int]$reader.ReadUInt16()
    } catch {
        return 0
    } finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        } elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Test-CompanionRollbackResult {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,

        [Parameter(Mandatory = $true)]
        [ValidateSet(
            'installer-failure',
            'acknowledgement-timeout'
        )]
        [string]$Scenario,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion,

        [Parameter(Mandatory = $true)]
        [int]$ExpectedBuild
    )

    $resultFields = @(
        'schemaVersion'
        'scenario'
        'passed'
        'requestId'
        'injectedFailureCode'
        'rollbackSucceeded'
        'completedAtUtc'
        'evidence'
    )
    if (
        -not (
            Test-CompanionObjectHasExactProperties `
                -Value $Result `
                -Expected $resultFields
        )
    ) {
        return $false
    }

    $evidence =
        Get-CompanionObjectPropertyValue `
            -Value $Result `
            -Name 'evidence'
    $evidenceFields = @(
        'restoredVersion'
        'restoredBuild'
        'relaunchProcessId'
        'helperExited'
        'rollbackRemoved'
        'userDataPreserved'
    )
    if (
        -not (
            Test-CompanionObjectHasExactProperties `
                -Value $evidence `
                -Expected $evidenceFields
        )
    ) {
        return $false
    }

    $expectedFailureCode =
        switch ($Scenario) {
        'installer-failure' {
            'update.installer_exit_failed'
        }
        'acknowledgement-timeout' {
            'update.acknowledgement_timeout'
        }
        }
    try {
        $schemaVersion =
            [int](
                Get-CompanionObjectPropertyValue `
                    -Value $Result `
                    -Name 'schemaVersion'
            )
        $restoredBuild =
            [int](
                Get-CompanionObjectPropertyValue `
                    -Value $evidence `
                    -Name 'restoredBuild'
            )
        $relaunchProcessId =
            [long](
                Get-CompanionObjectPropertyValue `
                    -Value $evidence `
                    -Name 'relaunchProcessId'
            )
    } catch {
        return $false
    }

    $requestId =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'requestId'
        )
    $parsedRequestId = [Guid]::Empty
    $requestIdValid =
        [Guid]::TryParseExact(
            $requestId,
            'D',
            [ref]$parsedRequestId
        )
    $completedAtUtc =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'completedAtUtc'
        )
    $completed = [DateTimeOffset]::MinValue
    $completedValid =
        [DateTimeOffset]::TryParse(
            $completedAtUtc,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal `
                -bor `
                [Globalization.DateTimeStyles]::AdjustToUniversal,
            [ref]$completed
        )
    $passed =
        Get-CompanionObjectPropertyValue `
            -Value $Result `
            -Name 'passed'
    $rollbackSucceeded =
        Get-CompanionObjectPropertyValue `
            -Value $Result `
            -Name 'rollbackSucceeded'
    $helperExited =
        Get-CompanionObjectPropertyValue `
            -Value $evidence `
            -Name 'helperExited'
    $rollbackRemoved =
        Get-CompanionObjectPropertyValue `
            -Value $evidence `
            -Name 'rollbackRemoved'
    $userDataPreserved =
        Get-CompanionObjectPropertyValue `
            -Value $evidence `
            -Name 'userDataPreserved'
    return (
        $schemaVersion -eq 1 -and
        (
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $Result `
                    -Name 'scenario'
            )
        ) -ceq $Scenario -and
        $passed -is [bool] -and
        $passed -and
        $requestIdValid -and
        (
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $Result `
                    -Name 'injectedFailureCode'
            )
        ) -ceq $expectedFailureCode -and
        $rollbackSucceeded -is [bool] -and
        $rollbackSucceeded -and
        $completedValid -and
        (
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $evidence `
                    -Name 'restoredVersion'
            )
        ) -ceq $ExpectedVersion -and
        $restoredBuild -eq $ExpectedBuild -and
        $relaunchProcessId -gt 0 -and
        $helperExited -is [bool] -and
        $helperExited -and
        $rollbackRemoved -is [bool] -and
        $rollbackRemoved -and
        $userDataPreserved -is [bool] -and
        $userDataPreserved
    )
}

function Get-CompanionUpdaterTransactionRoots {
    param(
        [string]$UpdaterRoot = (
            Join-Path `
                ([System.IO.Path]::GetTempPath()) `
                'CodexCompanionUpdater'
        )
    )

    $root = ConvertTo-CompanionFullPath -Value $UpdaterRoot
    if (
        [string]::IsNullOrWhiteSpace($root) -or
        -not (Test-Path -LiteralPath $root -PathType Container)
    ) {
        return @()
    }
    return @(
        Get-ChildItem `
            -LiteralPath $root `
            -Directory `
            -Force |
            Sort-Object `
                -Property LastWriteTimeUtc `
                -Descending |
            ForEach-Object {
                [System.IO.Path]::GetFullPath($_.FullName)
            }
    )
}

function Find-CompanionUpdateTransaction {
    param(
        [string]$UpdaterRoot = (
            Join-Path `
                ([System.IO.Path]::GetTempPath()) `
                'CodexCompanionUpdater'
        ),

        [string[]]$ExcludedRoots = @(),

        [Parameter(Mandatory = $true)]
        [string]$ExpectedVersion,

        [Parameter(Mandatory = $true)]
        [int]$ExpectedBuild,

        [Parameter(Mandatory = $true)]
        [string]$InstallRoot
    )

    $expectedInstallRoot =
        ConvertTo-CompanionFullPath -Value $InstallRoot
    if (
        [string]::IsNullOrWhiteSpace($expectedInstallRoot) -or
        [string]::IsNullOrWhiteSpace($ExpectedVersion) -or
        $ExpectedBuild -lt 0
    ) {
        return $null
    }

    $excluded =
        New-Object 'System.Collections.Generic.HashSet[string]' (
            [System.StringComparer]::OrdinalIgnoreCase
        )
    foreach ($path in $ExcludedRoots) {
        $full = ConvertTo-CompanionFullPath -Value $path
        if (-not [string]::IsNullOrWhiteSpace($full)) {
            [void]$excluded.Add($full)
        }
    }

    $requestFields = @(
        'requestId'
        'installerPath'
        'expectedSha256'
        'expectedSize'
        'expectedVersion'
        'expectedBuild'
        'installRoot'
        'rollbackRoot'
        'uninstallRegistryKey'
        'startMenuShortcut'
        'acknowledgementEvent'
        'parentProcessId'
    )
    foreach (
        $transactionRoot in @(
            Get-CompanionUpdaterTransactionRoots `
                -UpdaterRoot $UpdaterRoot
        )
    ) {
        if ($excluded.Contains($transactionRoot)) {
            continue
        }
        $requestPath =
            Join-Path $transactionRoot 'request.json'
        if (-not (Test-Path -LiteralPath $requestPath -PathType Leaf)) {
            continue
        }

        try {
            $request =
                Get-Content -LiteralPath $requestPath -Raw |
                ConvertFrom-Json
        } catch {
            continue
        }
        if (
            -not (
                Test-CompanionObjectHasExactProperties `
                    -Value $request `
                    -Expected $requestFields
            )
        ) {
            continue
        }

        $requestId =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'requestId'
            )
        $parsedRequestId = [Guid]::Empty
        if (
            -not [Guid]::TryParseExact(
                $requestId,
                'D',
                [ref]$parsedRequestId
            ) -or
            -not (
                [System.IO.Path]::GetFileName(
                    $transactionRoot
                ).Equals(
                    $requestId,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            )
        ) {
            continue
        }

        $requestInstallRoot =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'installRoot'
            )
        $expectedRollbackRoot =
            "$expectedInstallRoot.rollback.$requestId"
        $requestRollbackRoot =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'rollbackRoot'
            )
        $requestVersion =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'expectedVersion'
            )
        try {
            $requestBuild =
                [int](
                    Get-CompanionObjectPropertyValue `
                        -Value $request `
                        -Name 'expectedBuild'
                )
            $expectedSize =
                [long](
                    Get-CompanionObjectPropertyValue `
                        -Value $request `
                        -Name 'expectedSize'
                )
            $parentProcessId =
                [long](
                    Get-CompanionObjectPropertyValue `
                        -Value $request `
                        -Name 'parentProcessId'
                )
        } catch {
            continue
        }
        $expectedSha256 =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'expectedSha256'
            )
        $installerPath =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'installerPath'
            )
        $acknowledgementEvent =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $request `
                    -Name 'acknowledgementEvent'
            )
        $normalizedInstallerPath =
            ConvertTo-CompanionFullPath `
                -Value $installerPath
        if (
            $requestVersion -cne $ExpectedVersion -or
            $requestBuild -ne $ExpectedBuild -or
            -not (
                Test-CompanionPathEqual `
                    -Left $requestInstallRoot `
                    -Right $expectedInstallRoot
            ) -or
            -not (
                Test-CompanionPathEqual `
                    -Left $requestRollbackRoot `
                    -Right $expectedRollbackRoot
            ) -or
            $acknowledgementEvent -cne (
                "Local\CodexCompanion.UpdateAck.$requestId"
            ) -or
            $expectedSha256 -cnotmatch '^[0-9a-f]{64}$' -or
            $expectedSize -le 0 -or
            $parentProcessId -le 0 -or
            [string]::IsNullOrWhiteSpace(
                $normalizedInstallerPath
            )
        ) {
            continue
        }

        return [pscustomobject]@{
            root = $transactionRoot
            requestPath = $requestPath
            resultPath =
                Join-Path $transactionRoot 'result.json'
            helperPath =
                Join-Path `
                    $transactionRoot `
                    'CodexCompanionUpdater.exe'
            rollbackRoot = $expectedRollbackRoot
            request = $request
        }
    }
    return $null
}

function Test-CompanionUpdateResult {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Result,

        [Parameter(Mandatory = $true)]
        [string]$RequestId,

        [Parameter(Mandatory = $true)]
        [string]$TransactionRoot
    )

    $resultFields = @(
        'schema'
        'requestId'
        'success'
        'errorCode'
        'message'
        'completedAtUtc'
        'installerLogPath'
        'context'
    )
    if (
        -not (
            Test-CompanionObjectHasExactProperties `
                -Value $Result `
                -Expected $resultFields
        )
    ) {
        return $false
    }

    try {
        $schema =
            [int](
                Get-CompanionObjectPropertyValue `
                    -Value $Result `
                    -Name 'schema'
            )
    } catch {
        return $false
    }
    $actualRequestId =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'requestId'
        )
    $success =
        Get-CompanionObjectPropertyValue `
            -Value $Result `
            -Name 'success'
    $errorCode =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'errorCode'
        )
    $message =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'message'
        )
    $completedAtUtc =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'completedAtUtc'
        )
    $installerLogPath =
        [string](
            Get-CompanionObjectPropertyValue `
                -Value $Result `
                -Name 'installerLogPath'
        )
    $completed = [DateTimeOffset]::MinValue
    $completedValid =
        [DateTimeOffset]::TryParse(
            $completedAtUtc,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal `
                -bor `
                [Globalization.DateTimeStyles]::AdjustToUniversal,
            [ref]$completed
        )
    return (
        $schema -eq 1 -and
        $actualRequestId -ceq $RequestId -and
        $success -is [bool] -and
        $success -and
        [string]::IsNullOrWhiteSpace($errorCode) -and
        -not [string]::IsNullOrWhiteSpace($message) -and
        $completedValid -and
        (
            Test-CompanionPathEqual `
                -Left $installerLogPath `
                -Right (
                    Join-Path $TransactionRoot 'installer.log'
                )
        )
    )
}

function Get-CompanionDurableStateSnapshot {
    param(
        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Roots
    )

    $entries = @()
    foreach ($rootNameValue in @($Roots.Keys | Sort-Object)) {
        $rootName = ([string]$rootNameValue).Trim()
        $rootPath =
            ConvertTo-CompanionFullPath `
                -Value ([string]$Roots[$rootNameValue])
        if (
            [string]::IsNullOrWhiteSpace($rootName) -or
            [string]::IsNullOrWhiteSpace($rootPath)
        ) {
            throw 'Durable-state roots require non-empty names and paths.'
        }

        $rootItem =
            Get-Item `
                -LiteralPath $rootPath `
                -Force `
                -ErrorAction SilentlyContinue
        if ($null -eq $rootItem) {
            continue
        }
        $files = @()
        if ($rootItem.PSIsContainer) {
            $files = @(
                Get-ChildItem `
                    -LiteralPath $rootPath `
                    -File `
                    -Recurse `
                    -Force `
                    -ErrorAction Stop
            )
        } else {
            $files = @($rootItem)
        }

        foreach ($file in $files) {
            $relativePath = if ($rootItem.PSIsContainer) {
                (
                    $file.FullName.Substring(
                        $rootPath.Length
                    )
                ).TrimStart('\', '/')
            } else {
                $file.Name
            }
            if ([string]::IsNullOrWhiteSpace($relativePath)) {
                throw (
                    "Could not determine the durable-state path for " +
                    "$rootName."
                )
            }
            $entries += [ordered]@{
                root = $rootName
                path = $relativePath
                size = [int64]$file.Length
                sha256 = (
                    Get-FileHash `
                        -LiteralPath $file.FullName `
                        -Algorithm SHA256 `
                        -ErrorAction Stop
                ).Hash.ToLowerInvariant()
            }
        }
    }
    return @(
        $entries |
            Sort-Object root, path
    )
}

function Test-CompanionDurableStatePreserved {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Baseline,

        [Parameter(Mandatory = $true)]
        [object[]]$Current,

        [string[]]$RequiredRoots = @()
    )

    if ($Baseline.Count -eq 0) {
        return $false
    }

    $baselineRoots =
        New-Object `
            'System.Collections.Generic.HashSet[string]' `
            ([System.StringComparer]::OrdinalIgnoreCase)
    $currentByIdentity =
        New-Object `
            'System.Collections.Generic.Dictionary[string,object]' `
            ([System.StringComparer]::OrdinalIgnoreCase)

    foreach ($entry in $Current) {
        $root =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $entry `
                    -Name 'root'
            )
        $path =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $entry `
                    -Name 'path'
            )
        $sha256 =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $entry `
                    -Name 'sha256'
            )
        if (
            [string]::IsNullOrWhiteSpace($root) -or
            [string]::IsNullOrWhiteSpace($path) -or
            $sha256 -cnotmatch '^[0-9a-f]{64}$'
        ) {
            return $false
        }
        $identity = "$root`0$path"
        if ($currentByIdentity.ContainsKey($identity)) {
            return $false
        }
        $currentByIdentity.Add($identity, $entry)
    }

    $baselineIdentities =
        New-Object `
            'System.Collections.Generic.HashSet[string]' `
            ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($expected in $Baseline) {
        $root =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $expected `
                    -Name 'root'
            )
        $path =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $expected `
                    -Name 'path'
            )
        $sha256 =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $expected `
                    -Name 'sha256'
            )
        try {
            $size =
                [int64](
                    Get-CompanionObjectPropertyValue `
                        -Value $expected `
                        -Name 'size'
                )
        } catch {
            return $false
        }
        if (
            [string]::IsNullOrWhiteSpace($root) -or
            [string]::IsNullOrWhiteSpace($path) -or
            $size -lt 0 -or
            $sha256 -cnotmatch '^[0-9a-f]{64}$'
        ) {
            return $false
        }

        [void]$baselineRoots.Add($root)
        $identity = "$root`0$path"
        if (-not $baselineIdentities.Add($identity)) {
            return $false
        }
        if (-not $currentByIdentity.ContainsKey($identity)) {
            return $false
        }
        $actual = $currentByIdentity[$identity]
        $actualSha256 =
            [string](
                Get-CompanionObjectPropertyValue `
                    -Value $actual `
                    -Name 'sha256'
            )
        try {
            $actualSize =
                [int64](
                    Get-CompanionObjectPropertyValue `
                        -Value $actual `
                        -Name 'size'
                )
        } catch {
            return $false
        }
        if (
            $actualSha256 -cne $sha256 -or
            $actualSize -ne $size
        ) {
            return $false
        }
    }

    foreach ($requiredRoot in $RequiredRoots) {
        if (
            [string]::IsNullOrWhiteSpace($requiredRoot) -or
            -not $baselineRoots.Contains($requiredRoot)
        ) {
            return $false
        }
    }
    return $true
}

function Initialize-CompanionByteSearch {
    if ('CompanionCleanVmByteSearch' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;

public static class CompanionCleanVmByteSearch
{
    public static bool Contains(
        byte[] bytes,
        byte[] needle)
    {
        if (
            bytes == null ||
            needle == null ||
            needle.Length == 0 ||
            needle.Length > bytes.Length)
        {
            return false;
        }

        int lastStart =
            bytes.Length - needle.Length;
        byte first = needle[0];
        for (int start = 0;
             start <= lastStart;
             ++start)
        {
            if (bytes[start] != first)
            {
                continue;
            }
            int offset = 1;
            while (
                offset < needle.Length &&
                bytes[start + offset]
                    == needle[offset])
            {
                ++offset;
            }
            if (offset == needle.Length)
            {
                return true;
            }
        }
        return false;
    }
}
'@
}

function Test-CompanionByteSequence {
    param(
        [byte[]]$Bytes,
        [byte[]]$Needle
    )

    Initialize-CompanionByteSearch
    return [CompanionCleanVmByteSearch]::Contains(
        $Bytes,
        $Needle
    )
}

function Get-CompanionSensitiveLogEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Paths,

        [Parameter(Mandatory = $true)]
        [System.Collections.IDictionary]$Markers
    )

    $matchedCategories =
        New-Object `
            'System.Collections.Generic.HashSet[string]' `
            ([System.StringComparer]::Ordinal)
    $missingFiles = @()
    $unreadableFiles = @()
    $scannedFileCount = 0

    foreach ($path in $Paths) {
        if (
            [string]::IsNullOrWhiteSpace($path) -or
            -not (
                Test-Path `
                    -LiteralPath $path `
                    -PathType Leaf
            )
        ) {
            $missingFiles += if (
                [string]::IsNullOrWhiteSpace($path)
            ) {
                '<empty>'
            } else {
                [System.IO.Path]::GetFileName($path)
            }
            continue
        }

        $bytes = $null
        try {
            $bytes =
                [System.IO.File]::ReadAllBytes(
                    [System.IO.Path]::GetFullPath($path)
                )
            ++$scannedFileCount
        } catch {
            $unreadableFiles +=
                [System.IO.Path]::GetFileName($path)
            continue
        }

        foreach ($categoryValue in @($Markers.Keys | Sort-Object)) {
            $category =
                ([string]$categoryValue).Trim()
            $markerValue = $Markers[$categoryValue]
            if (
                [string]::IsNullOrWhiteSpace($category) -or
                $null -eq $markerValue
            ) {
                continue
            }

            $needles =
                New-Object `
                    'System.Collections.Generic.List[byte[]]'
            if ($markerValue -is [byte[]]) {
                [void]$needles.Add(
                    [byte[]]$markerValue
                )
            } else {
                $marker = [string]$markerValue
                if ([string]::IsNullOrEmpty($marker)) {
                    continue
                }
                [void]$needles.Add(
                    [System.Text.Encoding]::UTF8.GetBytes(
                        $marker
                    )
                )
                [void]$needles.Add(
                    [System.Text.Encoding]::Unicode.GetBytes(
                        $marker
                    )
                )
                [void]$needles.Add(
                    [System.Text.Encoding]::BigEndianUnicode.GetBytes(
                        $marker
                    )
                )
            }
            foreach ($needle in $needles) {
                if (
                    Test-CompanionByteSequence `
                        -Bytes $bytes `
                        -Needle $needle
                ) {
                    [void]$matchedCategories.Add(
                        $category
                    )
                    break
                }
            }
        }
    }

    $matched = @(
        $matchedCategories |
            Sort-Object
    )
    $missing = @(
        $missingFiles |
            Sort-Object -Unique
    )
    $unreadable = @(
        $unreadableFiles |
            Sort-Object -Unique
    )
    return [pscustomobject][ordered]@{
        clean =
            $Paths.Count -gt 0 -and
            $missing.Count -eq 0 -and
            $unreadable.Count -eq 0 -and
            $matched.Count -eq 0
        matchedCategories = $matched
        scannedFileCount = $scannedFileCount
        missingFiles = $missing
        unreadableFiles = $unreadable
    }
}

Export-ModuleMember `
    -Function `
        Set-CompanionUpdateFeedConfiguration, `
        Get-CompanionLaunchArguments, `
        Test-CompanionVersionIdentity, `
        Test-CompanionShortcutIdentity, `
        Test-CompanionInstallerIdentity, `
        Test-CompanionReleasePredecessor, `
        Get-CompanionPeMachine, `
        Test-CompanionRollbackResult, `
        Get-CompanionUpdaterTransactionRoots, `
        Find-CompanionUpdateTransaction, `
        Test-CompanionUpdateResult, `
        Get-CompanionDurableStateSnapshot, `
        Test-CompanionDurableStatePreserved, `
        Get-CompanionSensitiveLogEvidence
