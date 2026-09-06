param(
    [Parameter(Mandatory=$true)][string]$FinalBin,
    [Parameter(Mandatory=$true)][string]$Lua54Bin,
    [Parameter(Mandatory=$true)][string]$LuaArtifact,
    [Parameter(Mandatory=$true)][string]$OutputRoot
)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputRoot){throw 'Fresh supplemental output root required'}
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$base_path=$env:PATH
$script:runs=[Collections.Generic.List[object]]::new()
function runCase([string]$name,[string]$bin,[string[]]$arguments) {
    $executable=Join-Path $bin 'script_runtime_benchmark.exe'
    $env:PATH="$bin;E:/SyncForder/CodeRepos/build/deps/lua54-vcpkg/x64-windows/bin;D:/Development/vcpkg/installed/x64-windows/bin;$base_path"
    $csv=Join-Path $OutputRoot "$name.csv"
    $arguments += @('--output',$csv,'--seed','1592598566')
    Write-Output "[supplement] $name"
    & $executable @arguments *> (Join-Path $OutputRoot "$name.log")
    $record=[ordered]@{name=$name;executable=$executable;sha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash;
        arguments=$arguments;exit_code=$LASTEXITCODE;output=$csv;valid=($LASTEXITCODE -eq 0)}
    $script:runs.Add($record)
    $script:runs | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$OutputRoot/runs.json" -Encoding utf8
}
foreach($size in @(10000,20000)) {
    for($pair=0;$pair -lt 5;++$pair) {
        $order=if($pair % 2 -eq 0){@(0,4)}else{@(4,0)}
        foreach($workers in $order) {
            runCase "region-$size-p$pair-w$workers" $FinalBin @('--group','scene-region-numeric','--mode','performance',
                '--size',"$size",'--workers',"$workers",'--warmups','1000','--frames','5000')
        }
    }
}
foreach($shape in @(@(32,2),@(128,2),@(512,4),@(1024,8))) {
    foreach($density in @('sparse','dense')) {
        runCase "graph-$($shape[0])-$($shape[1])-$density" $FinalBin @('--group','graph-build','--mode','performance',
            '--size',"$($shape[0])",'--hooks',"$($shape[1])",'--density',$density,'--warmups','5','--frames','30')
    }
}
foreach($workers in @(1,2,4)) {
    runCase "region-diagnostic-w$workers" $FinalBin @('--group','scene-region-numeric','--mode','diagnostic',
        '--size','20000','--workers',"$workers",'--trace','on','--warmups','2','--frames','10')
}
foreach($size in @(10000,50000,100000)) {
    runCase "idle-structure-$size" $FinalBin @('--group','scene-event-idle','--mode','diagnostic',
        '--size',"$size",'--warmups','5','--frames','30')
}
runCase 'sparse-structure-100k' $FinalBin @('--group','scene-event-sparse','--mode','diagnostic',
    '--size','100000','--ready','10000','--resume-budget','2000','--warmups','5','--frames','30')
foreach($group in @('scene-lua-sequence','scene-lua-event','scene-lua-object-churn')) {
    for($run=0;$run -lt 5;++$run) {
        runCase "lua54-$group-r$run" $Lua54Bin @('--group',$group,'--mode','performance','--size','10000',
            '--warmups','1000','--frames','5000','--resume-budget','2000','--lua-artifact',$LuaArtifact)
    }
}
foreach($group in @('micro-cpp-event-wait','scene-lua-sequence')) {
    $arguments=@('--group',$group,'--mode','diagnostic','--size','10000','--warmups','2','--frames','5',
        '--resume-budget','2000','--lua-artifact',$LuaArtifact)
    if($group -eq 'scene-lua-sequence'){$arguments+=@('--vm-accounting','on')}
    runCase "allocation-$group" $FinalBin $arguments
}
