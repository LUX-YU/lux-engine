param(
    [string]$OutputRoot = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/hook-closure-performance',
    [int]$Pairs = 5,
    [int]$MeasuredFrames = 1000
)
$ErrorActionPreference = 'Stop'
$vcvars = 'D:\Development\Mircosoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat'
cmd /d /s /c ('"' + $vcvars + '" >nul && set') | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$base = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo'
$old_artifacts = "$base/s6-migration-toolchain/engine/toolchain"
$new_artifacts = "$base/hook-q-toolchain/engine/toolchain"
$points = @{
    B0 = @{ runtime="$base/s6-migration-developer/bin"; artifacts=$old_artifacts; sha='8354ae10e5d247cbc69746ae1f79c97ddfdd5ab9' };
    B1 = @{ runtime="$base/hook-closure-b1/bin"; artifacts=$old_artifacts; sha='da5e29b65f293f68c6c42231ce576cfa85618d2f' };
    B2 = @{ runtime="$base/hook-q-developer/bin"; artifacts=$new_artifacts; sha='5f03e9b156421583ae81857025ec6156ad0e0f05' }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$machine = [ordered]@{
    utc=[DateTime]::UtcNow.ToString('o'); processors=@(Get-CimInstance Win32_Processor |
        Select-Object Name, NumberOfCores, NumberOfLogicalProcessors);
    os=(Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber);
    seed=1592598566; pairs=$Pairs; measured_frames=$MeasuredFrames; effective_warmup_frames=100;
    runtime_raw_rows_discarded=95; runtime_natural_warmups=5; points=$points;
    note='Timing only, no VM allocator tracing; 5 natural warmups + 95 discarded rows normalize old/new binaries to 100 warmups.'
}
$machine | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$OutputRoot/machine-and-protocol.json" -Encoding utf8
function Measure-Process([string]$Executable, [string[]]$NativeArgs, [string]$LogPath) {
    Write-Output ("[paired] " + $Executable + ' ' + ($NativeArgs -join ' '))
    & $Executable @NativeArgs *> $LogPath
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed ($LASTEXITCODE): $LogPath" }
}
$orders = @(@('B0','B1','B2'), @('B1','B2','B0'), @('B2','B0','B1'), @('B0','B2','B1'), @('B2','B1','B0'))
for ($round = 0; $round -lt $Pairs; ++$round) {
    foreach ($group in @('scene-lua-update-heavy','scene-lua-ability','scene-lua-coroutine','scene-lua-event')) {
        foreach ($point in $orders[$round % $orders.Count]) {
            $configuration = $points[$point]
            $stem = "$point-$group-10000-r$($round+1)"
            Measure-Process "$($configuration.runtime)/script_runtime_benchmark.exe" @(
                '--group', $group, '--mode', 'diagnostic', '--size', '10000',
                '--frames', [string]($MeasuredFrames+95), '--seed', '1592598566', '--resume-budget', '2000',
                '--lua-artifact', "$($configuration.artifacts)/lua/lua_runtime_benchmark_fixture.lxsa",
                '--output', "$OutputRoot/$stem.csv") "$OutputRoot/$stem.log"
        }
    }
    foreach ($population in @(10000,20000)) {
        foreach ($point in $orders[$round % $orders.Count]) {
            $configuration = $points[$point]
            $stem = "$point-scene-physics-mixed-$population-r$($round+1)"
            Measure-Process "$($configuration.runtime)/physics2d_script_benchmark.exe" @(
                '--group', 'scene-physics-mixed', '--mode', 'diagnostic', '--size', [string]$population,
                '--warmups', '100', '--frames', [string]$MeasuredFrames,
                '--lua-artifact', "$($configuration.artifacts)/physics2d/physics2d_lua_fixture.lxsa",
                '--flowforge-artifact', "$($configuration.artifacts)/physics2d/physics2d_flowforge_fixture.lxsa",
                '--output', "$OutputRoot/$stem.csv") "$OutputRoot/$stem.log"
        }
    }
}
