<#
.SYNOPSIS
  External-feature authoring probe — proves a third-party RenderFeature compiles
  against the INSTALLED Vulkan extension surface alone
  (render/vulkan/include + external SDKs),
  with the module's internal dirs (sinclude / pinclude) STRIPPED.

  Complements check_install_surface.ps1: that one proves every public HEADER
  self-compiles; this one proves a real feature AUTHORED on top of them compiles.
  Every .cpp under <ModuleSrc>/samples is compiled syntax-only (cl /Zs).

.DESCRIPTION
  Reuses the same flag-derivation as the install-surface gate: take one
  representative module TU's compile flags from compile_commands.json, strip the
  internal -I dirs (suffix match on \sinclude, \pinclude, \builtin_shaders) and
  the per-file /Fo /Fd /c tail, then compile the sample TUs in one cl /Zs call.

  Requires a VS Developer environment (cl.exe on PATH, amd64).

.PARAMETER BuildDir
  CMake build dir containing compile_commands.json.
.PARAMETER ModuleSrc
  Module source dir; its samples/ tree is compiled.
.PARAMETER RepresentativeTU
  A source file (basename) in compile_commands.json whose flags are reused.
#>
param(
    [string]$BuildDir         = 'E:/SyncForder/CodeRepos/build/Debug/lux-engine',
    [string]$ModuleSrc        = 'E:/SyncForder/CodeRepos/lux-engine/modules/function/render/vulkan',
    [string]$RepresentativeTU = 'KernelRegistry.cpp'
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Error "cl.exe not on PATH — run from a VS Developer Shell (amd64)."
    exit 3
}

$ccPath = Join-Path $BuildDir 'compile_commands.json'
if (-not (Test-Path $ccPath)) { Write-Error "compile_commands.json not found: $ccPath"; exit 3 }
$cc = Get-Content $ccPath -Raw | ConvertFrom-Json

$entry = $cc | Where-Object { $_.file -match ([regex]::Escape($RepresentativeTU) + '$') } | Select-Object -First 1
if (-not $entry) { Write-Error "Representative TU '$RepresentativeTU' not in compile_commands.json"; exit 3 }

# --- Stripped flag string (same algorithm as check_install_surface.ps1) ---
$flags = $entry.command
$flags = $flags.Substring(0, $flags.IndexOf('/Fo'))

$internalSuffixes = @('\sinclude', '\pinclude', '\builtin_shaders')
$tokens   = @($flags -split '\s+' | Where-Object { $_.Trim().Length -gt 0 })
$compiler = $tokens[0]
$kept = @()
foreach ($tok in ($tokens | Select-Object -Skip 1)) {
    $isInternal = $false
    if ($tok -match '^-(external:)?I(.+)$') {
        $dir = $Matches[2]
        foreach ($suf in $internalSuffixes) { if ($dir.TrimEnd('\') -like "*$suf") { $isInternal = $true; break } }
    }
    if (-not $isInternal) { $kept += $tok }
}

# --- Collect sample TUs ---
$sampleDir = Join-Path $ModuleSrc 'samples'
$samples = @(Get-ChildItem -Path $sampleDir -Filter '*.cpp' -ErrorAction SilentlyContinue | Sort-Object FullName)
if ($samples.Count -eq 0) { Write-Output "No external-feature samples under $sampleDir — nothing to probe."; exit 0 }

Write-Output ("Compiling {0} external-feature sample(s) with PUBLIC headers only (sinclude/pinclude stripped) ..." -f $samples.Count)

$probeDir = Join-Path $BuildDir '_external_feature_probe'
if (Test-Path $probeDir) { Remove-Item $probeDir -Recurse -Force }
New-Item -ItemType Directory -Path $probeDir -Force | Out-Null

$rsp = Join-Path $probeDir '_args.rsp'
$rspLines = @('/Zs')
$rspLines += $kept
$rspLines += ($samples | ForEach-Object { '"' + $_.FullName + '"' })
Set-Content -Path $rsp -Value $rspLines -Encoding ascii

$out  = & $compiler "@$rsp" 2>&1 | Out-String
$code = $LASTEXITCODE

if ($code -eq 0) {
    Write-Output "EXTERNAL-FEATURE PROBE: PASS — sample feature(s) compile against the public surface alone."
    Remove-Item $probeDir -Recurse -Force
    exit 0
} else {
    Write-Output "EXTERNAL-FEATURE PROBE: FAIL — a sample feature needs a non-public (internal) header.`n"
    Write-Output $out
    Write-Output "Probe args kept for inspection at: $rsp"
    exit 1
}
