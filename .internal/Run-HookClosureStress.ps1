param([int]$Repeats=100)
$ErrorActionPreference='Stop'
$vcvars='D:\Development\Mircosoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat'
cmd /d /s /c ('"'+$vcvars+'" >nul && set') | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$'){
        [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
    }
}
$root='E:/SyncForder/CodeRepos/build'
$cases=@(
    @{
        path="$root/RelWithDebInfo/hook-q-developer";
        pattern='^(simulation_script_(lua_closure_(provenance|interpreter)_test|lua_coroutine(_interpreter)?_integration_test|lua_test|ingress_frontier_test|continuation_test|event_wait_test|lifecycle_test)|simulation_hook_(execution|regions)_test|scene_script_runtime_test|scene_script_lua_runtime_(interpreter_)?workers_4)$';
        log='stress-100.log'
    },
    @{
        path="$root/RelWithDebInfo/hook-q-toolchain";
        pattern='^(flowforge_script_artifact_test|flowforge_script_runtime_integration_test|physics2d_flowforge_integration_test)$';
        log='stress-100.log'
    },
    @{
        path="$root/Debug/hook-q-developer";
        pattern='^(simulation_script_(lua_closure_provenance_test|ingress_frontier_test|continuation_test|event_wait_test|lifecycle_test)|simulation_hook_(execution|regions)_test|scene_script_lua_runtime_workers_4)$';
        log='stress-100.log'
    }
)
foreach($case in $cases){
    Write-Output "[stress] $($case.path) x$Repeats"
    & ctest --test-dir $case.path --output-on-failure --repeat "until-fail:$Repeats" -R $case.pattern `
        *> "$($case.path)/$($case.log)"
    if($LASTEXITCODE -ne 0){throw "Stress failed: $($case.path)/$($case.log)"}
}
