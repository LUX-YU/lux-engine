param([Parameter(Mandatory=$true)][string]$Root,
    [Parameter(Mandatory=$true)][string]$Commit,
    [Parameter(Mandatory=$true)][string]$Label)
$ErrorActionPreference = 'Stop'
if ($Commit -notmatch '^[0-9a-f]{40}$') { throw 'Use a complete immutable source SHA, not HEAD or a branch' }
$development = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/s5/source'
$source = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source'
$scripts = "$development/.internal/diagnostics/lua55-s0-s1-2026-09-09"
$python = 'C:/Users/ChenHui/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
if ((& git -C $source status --porcelain)) { throw 'Qualification source is not clean' }
& git -C $source fetch $development codex/s6-deep-optimization
if ($LASTEXITCODE) { throw 'fetch failed' }
& git -C $source checkout --detach $Commit
if ($LASTEXITCODE) { throw 'checkout failed' }
& cmake "-DLUX_SOURCE_DIR=$source" -P "$source/cmake/ValidateTrackedSnapshot.cmake"
if ($LASTEXITCODE) { throw 'tracked snapshot failed' }
. 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
foreach ($slot in @('t','d','l')) {
    $log = "$Root/$Label-configure-$slot.log"
    if (Test-Path -LiteralPath $log) { throw 'Refusing overwrite' }
    & cmake -S $source -B "E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/$slot" *> $log
    if ($LASTEXITCODE) { throw "configure $slot failed" }
    & $python -B "$scripts/run.py" $Root "$Label-build" $slot build
    if ($LASTEXITCODE) { throw "build $slot failed" }
}
foreach ($slot in @('t','d','l')) {
    & $python -B "$scripts/run.py" $Root "$Label-correctness" $slot tests
    if ($LASTEXITCODE) { throw "correctness $slot failed" }
    if ($slot -ne 't') {
        & $python -B "$scripts/run.py" $Root "$Label-correctness" $slot smoke
        if ($LASTEXITCODE) { throw "oracle $slot failed" }
    }
}
