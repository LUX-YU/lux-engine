param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$InstallPrefix,
    [ValidateSet('developer','toolchain','debug','physics-off')][string]$BuildMode = 'developer',
    [string]$ArtifactBuildDir,
    [switch]$Install
)
$ErrorActionPreference = 'Stop'
$vcvars = 'D:\Development\Mircosoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat'
cmd /d /s /c ('"' + $vcvars + '" >nul && set') | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if (!(Test-Path "$env:VCToolsInstallDir\include\vector")) { throw 'MSVC environment unavailable' }

function Invoke-Checked([string]$Program, [string[]]$NativeArgs, [string]$LogPath) {
    Write-Output ("[hook-closure] " + $Program + ' ' + ($NativeArgs -join ' '))
    & $Program @NativeArgs *> $LogPath
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $LogPath -Tail 60
        throw "$Program failed ($LASTEXITCODE); see $LogPath"
    }
}

$source_path = (Resolve-Path -LiteralPath $SourceDir).Path
$build_path = [IO.Path]::GetFullPath($BuildDir)
New-Item -ItemType Directory -Force -Path $build_path | Out-Null
$production_sha = (& git -C $source_path rev-parse HEAD).Trim()
Invoke-Checked cmake @("-DLUX_SOURCE_DIR=$source_path", '-P', "$source_path/cmake/ValidateTrackedSnapshot.cmake") `
    "$build_path/tracked.log"
$configuration = if ($BuildMode -eq 'debug') { 'Debug' } else { 'RelWithDebInfo' }
$build_profile = if ($BuildMode -eq 'toolchain') { 'TOOLCHAIN' } else { 'DEVELOPER' }
$physics = if ($BuildMode -eq 'physics-off') { 'OFF' } else { 'ON' }
$dependency_prefix = if ($BuildMode -eq 'debug') {
    'E:/SyncForder/CodeRepos/install/Debug;E:/SyncForder/CodeRepos/install/RelWithDebInfo'
} else { 'E:/SyncForder/CodeRepos/install/RelWithDebInfo' }
$configure_args = @('-S', $source_path, '-B', $build_path, '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$configuration", "-DLUX_BUILD_PROFILE=$build_profile", '-DLUX_LUA_VM=LUAJIT',
    "-DLUX_BUILD_PHYSICS2D=$physics", '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
    "-DCMAKE_PREFIX_PATH=$dependency_prefix", "-DCMAKE_INSTALL_PREFIX=$InstallPrefix")
if ($ArtifactBuildDir -and $physics -eq 'ON' -and $BuildMode -ne 'toolchain') {
    $artifact_path = [IO.Path]::GetFullPath($ArtifactBuildDir).Replace('\','/')
    $configure_args += "-DLUX_PHYSICS2D_FLOWFORGE_ARTIFACT=$artifact_path/engine/toolchain/physics2d/physics2d_flowforge_fixture.lxsa"
    $configure_args += "-DLUX_PHYSICS2D_LUA_ARTIFACT=$artifact_path/engine/toolchain/physics2d/physics2d_lua_fixture.lxsa"
}
Invoke-Checked cmake $configure_args "$build_path/configure.log"
$targets = @('all')
if ($BuildMode -eq 'toolchain') {
    $targets += @('physics2d_flowforge_fixture', 'physics2d_lua_fixture', 'lua_runtime_benchmark_fixture')
}
Invoke-Checked cmake (@('--build', $build_path, '--target') + $targets + @('-j', '4', '--', '-k', '0')) `
    "$build_path/all.log"
$test_args = @('--test-dir', $build_path, '--output-on-failure')
if ($BuildMode -eq 'debug') {
    $test_args += @('-R', '(simulation_hook_|simulation_unscoped_hook|simulation_script_(lua_|event_wait|continuation|lifecycle)|scene_script_)')
}
Invoke-Checked ctest $test_args "$build_path/ctest.log"
Invoke-Checked cmake @('--build', $build_path, '--target', 'all', '-j', '4', '--', '-k', '0') "$build_path/second-build.log"
if (!(Select-String -LiteralPath "$build_path/second-build.log" -SimpleMatch 'no work to do' -Quiet)) {
    throw "Second build was not a no-op: $build_path"
}
if ($Install) {
    Invoke-Checked cmake @('--install', $build_path) "$build_path/install.log"
}
[ordered]@{
    production_sha=$production_sha; source=$source_path; build=$build_path; profile=$build_profile;
    configuration=$configuration; lua_vm='LUAJIT'; physics=$physics; tests='passed'; second_build='no-work';
    player='NOT RUN'; editor='NOT RUN'; lua54='NOT RUN'; android='NOT RUN'
} | ConvertTo-Json | Set-Content -LiteralPath "$build_path/qualification.json" -Encoding utf8
Write-Output "[hook-closure] $BuildMode passed at $production_sha"
