param(
    [Parameter(Mandatory = $true)]
    [string]$QmlRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$resolvedRoot = (Resolve-Path -LiteralPath $QmlRoot).Path
$interactiveTypes = @(
    "Button",
    "ComboBox",
    "CompactSwitch",
    "GoalActionButton",
    "MenuItem",
    "MouseArea",
    "PetContextMenuItem",
    "PetControlButton",
    "ProcessActionButton",
    "Slider",
    "Switch",
    "TabButton",
    "TapHandler",
    "ToolButton"
)
$alwaysInteractiveTypes = @(
    "Button",
    "ComboBox",
    "CompactSwitch",
    "GoalActionButton",
    "MenuItem",
    "PetContextMenuItem",
    "PetControlButton",
    "ProcessActionButton",
    "Slider",
    "Switch",
    "TabButton",
    "ToolButton"
)
$handlerPattern =
    '^\s*on(?:Activated|Clicked|DoubleClicked|Moved|Pressed|Released|Tapped|Toggled|Triggered)\s*:'
$interactionPattern =
    '^\s*(?:property\s+string\s+)?interactionId\s*:'
$declarationPattern =
    '^\s*(?<type>[A-Z][A-Za-z0-9_]*)\s*\{'

$missing = [System.Collections.Generic.List[object]]::new()
$auditedCount = 0

foreach ($file in Get-ChildItem -LiteralPath $resolvedRoot -Recurse -Filter "*.qml" | Sort-Object FullName) {
    $depth = 0
    $inBlockComment = $false
    $stack = [System.Collections.Generic.List[object]]::new()
    $lines = Get-Content -LiteralPath $file.FullName

    for ($lineIndex = 0; $lineIndex -lt $lines.Count; ++$lineIndex) {
        $line = [string]$lines[$lineIndex]
        $builder = [System.Text.StringBuilder]::new()
        $quote = [char]0
        $escaped = $false

        for ($column = 0; $column -lt $line.Length; ++$column) {
            $current = $line[$column]
            $next = if ($column + 1 -lt $line.Length) {
                $line[$column + 1]
            } else {
                [char]0
            }

            if ($inBlockComment) {
                if ($current -eq '*' -and $next -eq '/') {
                    $inBlockComment = $false
                    ++$column
                }
                continue
            }

            if ($quote -ne [char]0) {
                if ($escaped) {
                    $escaped = $false
                } elseif ($current -eq '\') {
                    $escaped = $true
                } elseif ($current -eq $quote) {
                    $quote = [char]0
                }
                [void]$builder.Append(' ')
                continue
            }

            if ($current -eq '/' -and $next -eq '/') {
                break
            }
            if ($current -eq '/' -and $next -eq '*') {
                $inBlockComment = $true
                ++$column
                continue
            }
            if ($current -eq '"' -or $current -eq "'") {
                $quote = $current
                [void]$builder.Append(' ')
                continue
            }
            [void]$builder.Append($current)
        }

        $structuralLine = $builder.ToString()
        foreach ($entry in $stack) {
            if ($depth -ne $entry.BodyDepth) {
                continue
            }
            if ($structuralLine -match $interactionPattern) {
                $entry.HasInteractionId = $true
            }
            if ($structuralLine -match $handlerPattern) {
                $entry.HasActionHandler = $true
            }
        }

        $declaration = [regex]::Match(
            $structuralLine,
            $declarationPattern)
        if ($declaration.Success) {
            $type = $declaration.Groups["type"].Value
            if ($interactiveTypes -contains $type) {
                $stack.Add([pscustomobject]@{
                    Type = $type
                    File = $file.FullName
                    Line = $lineIndex + 1
                    BodyDepth = $depth + 1
                    HasInteractionId = $false
                    HasActionHandler = $false
                })
            }
        }

        $openCount =
            ([regex]::Matches($structuralLine, '\{')).Count
        $closeCount =
            ([regex]::Matches($structuralLine, '\}')).Count
        $depth += $openCount - $closeCount

        while ($stack.Count -gt 0) {
            $entry = $stack[$stack.Count - 1]
            if ($depth -ge $entry.BodyDepth) {
                break
            }
            $stack.RemoveAt($stack.Count - 1)

            $requiresId =
                ($alwaysInteractiveTypes -contains $entry.Type) -or
                $entry.HasActionHandler
            if (-not $requiresId) {
                continue
            }

            ++$auditedCount
            if (-not $entry.HasInteractionId) {
                $missing.Add($entry)
            }
        }
    }
}

if ($missing.Count -gt 0) {
    foreach ($entry in $missing) {
        $relativePath = [System.IO.Path]::GetRelativePath(
            $resolvedRoot,
            $entry.File)
        Write-Error (
            "{0}:{1}: interactive {2} has no direct interactionId" -f
            $relativePath,
            $entry.Line,
            $entry.Type)
    }
    exit 1
}

Write-Host (
    "QML interaction audit passed: {0} controls have stable interaction IDs." -f
    $auditedCount)
