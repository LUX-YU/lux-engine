param(
    [Parameter(Mandatory=$true)][string]$BaselineBin,
    [Parameter(Mandatory=$true)][string]$FinalBin,
    [Parameter(Mandatory=$true)][string]$BaselineToolBin,
    [Parameter(Mandatory=$true)][string]$FinalToolBin,
    [Parameter(Mandatory=$true)][string]$LuaArtifact,
    [Parameter(Mandatory=$true)][string]$PhysicsLuaArtifact,
    [Parameter(Mandatory=$true)][string]$PhysicsFlowArtifact,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [string[]]$Only = @(),
    [int]$Pairs = 5
)
$ErrorActionPreference = 'Stop'
& 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:LUX_FLOWFORGE_LINKER = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/link.exe'
$base_path = $env:PATH
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$cases = @(
    @{name='cpp-update-10k'; exe='script_runtime_benchmark.exe'; group='scene-cpp-update-heavy'; size=10000},
    @{name='lua-update-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-update-heavy'; size=10000; lua=$true},
    @{name='lua-update-interpreter-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-update-heavy'; size=10000; lua=$true; interpreter=$true},
    @{name='lua-ability-10k'; exe='script_runtime_benchmark.exe'; group='micro-lua-ability-query'; size=10000; lua=$true; micro=$true},
    @{name='lua-coroutine-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-coroutine'; size=10000; lua=$true},
    @{name='lua-event-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-event'; size=10000; lua=$true},
    @{name='lua-event-interpreter-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-event'; size=10000; lua=$true; interpreter=$true},
    @{name='flow-update-10k'; exe='flowforge_script_runtime_benchmark.exe'; group='scene-flowforge-update-heavy'; size=10000; tool=$true},
    @{name='flow-ability-10k'; exe='flowforge_script_runtime_benchmark.exe'; group='micro-flowforge-ability-query'; size=10000; tool=$true; micro=$true},
    @{name='flow-coroutine-10k'; exe='flowforge_script_runtime_benchmark.exe'; group='scene-flowforge-gameplay-mixed'; size=10000; tool=$true},
    @{name='flow-event-10k'; exe='flowforge_script_runtime_benchmark.exe'; group='scene-flowforge-event'; size=10000; tool=$true},
    @{name='cpp-sequence-10k'; exe='script_runtime_benchmark.exe'; group='scene-cpp-sequence'; size=10000},
    @{name='lua-sequence-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-sequence'; size=10000; lua=$true},
    @{name='lua-sequence-interpreter-10k'; exe='script_runtime_benchmark.exe'; group='scene-lua-sequence'; size=10000; lua=$true; interpreter=$true},
    @{name='flow-sequence-10k'; exe='flowforge_script_runtime_benchmark.exe'; group='scene-flowforge-sequence'; size=10000; tool=$true},
    @{name='event-register-1'; exe='script_runtime_benchmark.exe'; group='micro-event-wait'; size=10000; micro=$true; requirements=1},
    @{name='event-register-4'; exe='script_runtime_benchmark.exe'; group='micro-event-wait'; size=10000; micro=$true; requirements=4},
    @{name='event-register-16'; exe='script_runtime_benchmark.exe'; group='micro-event-wait'; size=10000; micro=$true; requirements=16},
    @{name='event-register-64'; exe='script_runtime_benchmark.exe'; group='micro-event-wait'; size=10000; micro=$true; requirements=64},
    @{name='event-targeted-10k'; exe='script_runtime_benchmark.exe'; group='micro-event-wait'; size=10000; micro=$true; targeted=$true},
    @{name='event-fanout-10k'; exe='script_runtime_benchmark.exe'; group='scene-event-fanout'; size=10000},
    @{name='cpp-coroutine-100k'; exe='script_runtime_benchmark.exe'; group='micro-cpp-coroutine'; size=100000; micro=$true; pairs=3},
    @{name='physics-mixed-10k'; exe='physics2d_script_benchmark.exe'; group='scene-physics-mixed'; size=10000; physics=$true},
    @{name='physics-mixed-20k'; exe='physics2d_script_benchmark.exe'; group='scene-physics-mixed'; size=20000; physics=$true}
)
foreach ($count in @(1,4,16,64)) {
    $cases += @{name="cpp-event-requirements-$count";exe='script_runtime_benchmark.exe';
        group='micro-cpp-event-wait';size=10000;micro=$true;requirements=$count}
}
foreach ($count in @(1,64,1000,10000)) {
    foreach ($route in @('broadcast','targeted')) {
        $cases += @{name="cpp-event-$route-$count";exe='script_runtime_benchmark.exe';
            group='micro-cpp-event-wait';size=$count;micro=$true;targeted=($route -eq 'targeted')}
    }
}
foreach ($size in @(10000,50000,100000)) {
    foreach ($backend in @('cpp','native','lua')) {
        $cases += @{name="$backend-population-$size";exe='script_runtime_benchmark.exe';
            group="scene-$backend-population";size=$size;micro=$true;pairs=3;lua=($backend -eq 'lua')}
    }
}
$manifest = @()
$manifest_path = Join-Path $OutputRoot 'runs.json'
if (Test-Path -LiteralPath $manifest_path) { $manifest = @(Get-Content -Raw $manifest_path | ConvertFrom-Json) }
foreach ($case in $cases) {
    if ($Only.Count -ne 0 -and $case.name -notin $Only) { continue }
    $pair_count = if ($case.pairs) { [Math]::Min($Pairs,$case.pairs) } else { $Pairs }
    for ($pair=0; $pair -lt $pair_count; ++$pair) {
        $order = if ($pair % 2 -eq 0) { @('baseline','final') } else { @('final','baseline') }
        foreach ($variant in $order) {
            $bin = if ($variant -eq 'baseline') { $BaselineBin } else { $FinalBin }
            $tool = if ($variant -eq 'baseline') { $BaselineToolBin } else { $FinalToolBin }
            $exe_root = if ($case.tool) { $tool } else { $bin }
            $executable = Join-Path $exe_root $case.exe
            $name = "$($case.name)-p$pair-$variant"
            $csv = Join-Path $OutputRoot "$name.csv"
            if (Test-Path -LiteralPath $csv) { throw "Refusing to overwrite existing run $csv" }
            $warmups = if ($case.micro) { 5 } else { 1000 }
            $frames = if ($case.micro) { 30 } else { 5000 }
            $arguments = @('--group',$case.group,'--mode','performance','--size',"$($case.size)",
                '--warmups',"$warmups",'--frames',"$frames",'--output',$csv)
            if (!$case.physics) { $arguments += @('--seed','1592598566','--resume-budget','2000') }
            if ($case.lua) { $arguments += @('--lua-artifact',$LuaArtifact) }
            if ($case.interpreter) { $arguments += @('--lua-policy','interpreter-only') }
            if ($case.requirements) { $arguments += @('--event-requirements',"$($case.requirements)") }
            if ($case.targeted) { $arguments += @('--event-route','targeted') }
            if ($case.physics) {
                $arguments += @('--lua-artifact',$PhysicsLuaArtifact,'--flowforge-artifact',$PhysicsFlowArtifact,'--workers','0')
            }
            $env:PATH = "$exe_root;$bin;$tool;D:/Development/vcpkg/installed/x64-windows/bin;$base_path"
            $entry = [ordered]@{case=$case.name;pair=$pair;variant=$variant;executable=$executable;
                sha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash;
                arguments=$arguments;start_utc=[DateTime]::UtcNow.ToString('o');output=$csv}
            Write-Output "[measure] $name"
            $watch = [Diagnostics.Stopwatch]::StartNew()
            & $executable @arguments *> (Join-Path $OutputRoot "$name.log")
            $entry.exit_code = $LASTEXITCODE
            $entry.process_ms = $watch.Elapsed.TotalMilliseconds
            $entry.valid = $LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $csv)
            $manifest += $entry
            $manifest | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifest_path -Encoding utf8
            if (!$entry.valid) { Write-Output "[measure] INVALID $name (exit $($entry.exit_code))" }
        }
    }
}
