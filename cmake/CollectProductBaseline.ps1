<#
.SYNOPSIS
  Collect a reproducible build-product baseline without launching a scene.

.DESCRIPTION
  Reads CMake's orthogonal build facts and classified target report, measures
  the exact Player runtime inventory and binary closure size, then records the
  median process startup cost of `lux_player --help`. Frame/asset performance
  remains a separate attended fixed-scene benchmark; this script deliberately
  does not pretend a CLI startup is frame performance.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,
    [ValidateRange(1, 25)]
    [int]$Runs = 5,
    [string]$Output = ""
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (-not $Output) {
    $Output = Join-Path $build 'product-baseline.json'
}

$facts = @{}
$factsPath = Join-Path $build 'lux-build-facts.txt'
foreach ($line in Get-Content -LiteralPath $factsPath) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $parts = $line.Split('=', 2)
    if ($parts.Count -eq 2) { $facts[$parts[0]] = $parts[1] }
}

$classifiedPath = Join-Path $build 'target-architecture-classified.txt'
$classified = @(Get-Content -LiteralPath $classifiedPath |
    Where-Object { $_ -and -not $_.StartsWith('#') })
$byLayer = @{}
$byProduct = @{}
foreach ($line in $classified) {
    $fields = $line.Split('|')
    if ($fields.Count -lt 6) { throw "invalid classified target row: $line" }
    $layer = $fields[2]
    $product = $fields[3]
    $byLayer[$layer] = 1 + [int]($byLayer[$layer] ?? 0)
    $byProduct[$product] = 1 + [int]($byProduct[$product] ?? 0)
}

$bin = Join-Path $build 'bin'
$player = @(
    (Join-Path $bin 'lux_player.exe'),
    (Join-Path $bin 'lux_player')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$inventoryPath = Join-Path $bin 'lux_player.runtime-files'
$runtimeFiles = @()
if (Test-Path -LiteralPath $inventoryPath) {
    $runtimeFiles = @(Get-Content -LiteralPath $inventoryPath |
        Where-Object { $_ -and -not $_.StartsWith('#') } |
        Sort-Object -Unique)
}

$closureBytes = [uint64]0
$missingRuntimeFiles = @()
foreach ($name in $runtimeFiles) {
    $path = Join-Path $bin $name
    if (Test-Path -LiteralPath $path) {
        $closureBytes += (Get-Item -LiteralPath $path).Length
    } else {
        $missingRuntimeFiles += $name
    }
}

$startupMs = @()
if ($player) {
    for ($index = 0; $index -lt $Runs; ++$index) {
        $elapsed = Measure-Command { & $player --help *> $null }
        if ($LASTEXITCODE -ne 0) {
            throw "lux_player --help failed with exit code $LASTEXITCODE"
        }
        $startupMs += [Math]::Round($elapsed.TotalMilliseconds, 3)
    }
}
$sortedStartup = @($startupMs | Sort-Object)
$medianStartup = if ($sortedStartup.Count) {
    $sortedStartup[[int][Math]::Floor($sortedStartup.Count / 2)]
} else { $null }

$report = [ordered]@{
    schema = 1
    collected_utc = [DateTime]::UtcNow.ToString('o')
    build_facts = $facts
    classified_target_count = $classified.Count
    targets_by_layer = $byLayer
    targets_by_product = $byProduct
    player = if ($player) { Split-Path -Leaf $player } else { $null }
    player_binary_bytes = if ($player) {
        (Get-Item -LiteralPath $player).Length
    } else { 0 }
    runtime_file_count = $runtimeFiles.Count
    runtime_closure_bytes = $closureBytes
    missing_runtime_files = $missingRuntimeFiles
    startup_runs_ms = $startupMs
    startup_median_ms = $medianStartup
    note = 'CLI startup only; fixed-scene frame/IO benchmarks remain separate.'
}

$json = $report | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText(
    [System.IO.Path]::GetFullPath($Output),
    $json + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false)
)
Write-Host "wrote $Output"
