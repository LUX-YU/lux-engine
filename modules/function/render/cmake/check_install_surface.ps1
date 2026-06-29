<#
.SYNOPSIS
  Install-surface probe — proves every PUBLIC header (include/) of a module
  self-compiles with ONLY the public + external include paths, i.e. with the
  module's internal dirs (sinclude / pinclude / generated-private) STRIPPED.

  This reproduces what an EXTERNAL consumer of the INSTALLED library sees, and
  fails the build the moment a public header leaks a dependency on an internal
  one. It is the encapsulation regression gate for the RenderFeature SDK work.

.DESCRIPTION
  Algorithm (see .internal/renderfeature-sdk-design-2026-06-26.md §8):
    1. Glob <ModuleSrc>/include/**/*.hpp.
    2. For each header, emit a one-line TU `#include <rel/path.hpp>`.
    3. Take the compile flags of ONE representative module TU from
       compile_commands.json, strip the internal -I dirs (suffix match on
       \sinclude, \pinclude, \builtin_shaders) and the per-file /Fo /Fd /c flags.
    4. Compile all probe TUs in a single `cl /Zs` (syntax-only) invocation.
    5. Non-zero exit on any failure; print the offending headers.

  Requires a VS Developer environment (cl.exe on PATH). Run from a VS Dev Shell
  (amd64), or let CI wrap it.

.PARAMETER BuildDir
  The CMake build dir containing compile_commands.json.
.PARAMETER ModuleSrc
  The module source dir whose include/ tree is probed.
.PARAMETER RepresentativeTU
  A source file (basename) present in compile_commands.json whose flags are reused.
#>
param(
    [string]$BuildDir         = 'E:/SyncForder/CodeRepos/build/Debug/lux-engine',
    [string]$ModuleSrc        = 'E:/SyncForder/CodeRepos/lux-engine/modules/function/render',
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

# --- Build the stripped flag string ---
# Cut the per-file output/source tail (everything from the first /Fo).
$flags = $entry.command
$flags = $flags.Substring(0, $flags.IndexOf('/Fo'))

# Strip internal include dirs by path SUFFIX (so we never strip an external dep
# whose path merely contains 'include'). Matches both -I and -external:I forms.
# The FIRST token is the compiler path (cl.exe) — peel it off; the rest are args.
$internalSuffixes = @('\sinclude', '\pinclude', '\builtin_shaders')
$tokens = @($flags -split '\s+' | Where-Object { $_.Trim().Length -gt 0 })
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
$keptFlags = ($kept -join ' ')

# --- Generate one probe TU per public header ---
$incRoot   = Join-Path $ModuleSrc 'include'
$probeDir  = Join-Path $BuildDir '_install_surface_probe'
if (Test-Path $probeDir) { Remove-Item $probeDir -Recurse -Force }
New-Item -ItemType Directory -Path $probeDir -Force | Out-Null

$headers = Get-ChildItem -Path $incRoot -Recurse -Filter '*.hpp' | Sort-Object FullName
$tuFiles = @()
foreach ($h in $headers) {
    $rel = $h.FullName.Substring($incRoot.Length).TrimStart('\','/') -replace '\\','/'
    $safe = ($rel -replace '[\\/]', '__')
    $tu = Join-Path $probeDir ($safe + '.cpp')
    "#include <$rel>" | Set-Content -Path $tu -Encoding utf8
    $tuFiles += $tu
}

Write-Output ("Probing {0} public headers under {1}/include ..." -f $headers.Count, (Split-Path $ModuleSrc -Leaf))

# --- Compile all probe TUs in one cl /Zs invocation (via response file to dodge
#     the command-line length limit; cl accepts `cl @file.rsp`) ---
$rsp = Join-Path $probeDir '_probe_args.rsp'
$rspLines = @('/Zs')
$rspLines += $kept                                   # all kept flags, one per line
$rspLines += ($tuFiles | ForEach-Object { '"' + $_ + '"' })
Set-Content -Path $rsp -Value $rspLines -Encoding ascii
$out = & $compiler "@$rsp" 2>&1 | Out-String
$code = $LASTEXITCODE

if ($code -eq 0) {
    Write-Output "INSTALL-SURFACE PROBE: PASS — all $($headers.Count) public headers self-compile (sinclude/pinclude stripped)."
    Remove-Item $probeDir -Recurse -Force
    exit 0
} else {
    Write-Output "INSTALL-SURFACE PROBE: FAIL — at least one public header leaks an internal dependency.`n"
    Write-Output $out
    Write-Output "Probe TUs kept for inspection at: $probeDir"
    exit 1
}
