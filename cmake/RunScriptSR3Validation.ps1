param(
    [Parameter(Mandatory=$true)][string]$BaselineSource,
    [Parameter(Mandatory=$true)][string]$CandidateSource,
    [Parameter(Mandatory=$true)][string]$BaselineInstall,
    [Parameter(Mandatory=$true)][string]$CandidateInstall,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$Python,
    [string]$Dependencies = 'E:/SyncForder/CodeRepos/install/q2/toolset;E:/SyncForder/CodeRepos/install/q2/c;E:/SyncForder/CodeRepos/install/RelWithDebInfo',
    [switch]$ScaleOnly
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputRoot) { throw 'A fresh evidence directory is required.' }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
& 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:LUX_FLOWFORGE_LINKER = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/link.exe'
$env:PYTHONDONTWRITEBYTECODE = '1'
$common = @('--baseline-source', $BaselineSource, '--candidate-source', $CandidateSource,
    '--baseline-prefix', "$BaselineInstall/d", '--candidate-prefix', "$CandidateInstall/d",
    '--dependencies', $Dependencies)
& $Python -B "$PSScriptRoot/RunScriptSR3Scale.py" @common --output "$OutputRoot/scale" *> "$OutputRoot/scale-driver.log"
if ($LASTEXITCODE -ne 0) { throw 'Scale replay failed; retain the original logs.' }
if ($ScaleOnly) { return }
& "$PSScriptRoot/RunScriptV3Consumers.ps1" -SourceDir $CandidateSource -OutputRoot "$OutputRoot/consumers" `
    -PrefixPath "$CandidateInstall/d;$CandidateInstall/t;$Dependencies" -DependencyRoot 'D:/Development/vcpkg/installed' `
    -Names cpp-generated-script,physics2d-script,system-event-await-runtime,flowforge-compiler,lua-script-packager,scene-script-runtime,script-ability-codegen,system-hook-script-binding,cpp-coroutine-script,script-static-ability-specialization,script-ability-ipo,script-authoring,script-runtime-input,script-description `
    *> "$OutputRoot/consumers-driver.log"
$consumers = @(Get-Content -LiteralPath "$OutputRoot/consumers/consumers.json" -Raw | ConvertFrom-Json)
if ($consumers.Count -ne 14 -or @($consumers | Where-Object status -ne 'PASS').Count -ne 0) {
    throw 'Installed consumers did not all pass.'
}
& $Python -B "$PSScriptRoot/RunScriptSR2Probes.py" @common --output "$OutputRoot/probes" *> "$OutputRoot/probes-driver.log"
if ($LASTEXITCODE -ne 0) { throw 'Lifecycle/Event/wire replay failed.' }
& $Python -B "$PSScriptRoot/RunScriptSR2Measurements.py" --baseline $BaselineInstall --candidate $CandidateInstall `
    --output "$OutputRoot/measurements" *> "$OutputRoot/measurements-driver.log"
if ($LASTEXITCODE -ne 0) { throw 'Paired runtime measurements failed.' }
$flow = @('--baseline-source', $BaselineSource, '--candidate-source', $CandidateSource,
    '--baseline-prefix', "$BaselineInstall/t", '--candidate-prefix', "$CandidateInstall/t",
    '--dependencies', $Dependencies)
& $Python -B "$PSScriptRoot/RunScriptSR2FlowAllocations.py" @flow --output "$OutputRoot/flow-allocations" `
    *> "$OutputRoot/flow-allocations-driver.log"
if ($LASTEXITCODE -ne 0) { throw 'FlowForge allocation diagnostics failed.' }
Write-Output 'SR-3 installed validation and paired measurements completed.'
