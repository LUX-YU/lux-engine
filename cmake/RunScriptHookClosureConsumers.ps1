param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$InstallPrefix,
    [string[]]$Names = @('physics2d-script','system-event-await-runtime','flowforge-compiler','lua-script-packager',
        'scene-script-runtime','script-ability-codegen','system-hook-script-binding','cpp-coroutine-script',
        'script-static-ability-specialization','script-ability-ipo'),
    [ValidateSet('RelWithDebInfo','Release')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$vcvars = 'D:\Development\Mircosoft\VisualStudio\VC\Auxiliary\Build\vcvars64.bat'
cmd /d /s /c ('"' + $vcvars + '" >nul && set') | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
if (!(Test-Path "$env:VCToolsInstallDir\include\vector")) { throw 'MSVC environment unavailable' }
$targets = @{
    'physics2d-script'='lux_physics2d_script_consumer';
    'system-event-await-runtime'='lux_system_event_await_runtime_consumer';
    'flowforge-compiler'='lux_flowforge_compiler_consumer';
    'lua-script-packager'='lua_script_packager_consumer';
    'scene-script-runtime'='lux_scene_script_runtime_consumer';
    'script-ability-codegen'='lux_script_ability_codegen_consumer';
    'system-hook-script-binding'='lux_system_hook_script_consumer';
    'cpp-coroutine-script'='lux_cpp_coroutine_script_consumer';
    'script-static-ability-specialization'='lux_script_static_ability_consumer';
    'script-ability-ipo'='lux_script_ability_ipo_consumer'
}
$prefix = (Resolve-Path -LiteralPath $InstallPrefix).Path
$env:PATH = "$prefix/bin;E:/SyncForder/CodeRepos/install/RelWithDebInfo/bin;D:/Development/vcpkg/installed/x64-windows/bin;$env:PATH"
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
foreach ($name in $Names) {
    if (!$targets.ContainsKey($name)) { throw "Unknown qualification consumer: $name" }
    $consumer_root = Join-Path $OutputRoot $name
    if (Test-Path -LiteralPath $consumer_root) { throw "Fresh consumer root required: $consumer_root" }
    New-Item -ItemType Directory -Path $consumer_root | Out-Null
    Copy-Item -LiteralPath "$SourceDir/cmake/installed-consumers/$name" -Destination "$consumer_root/source" -Recurse
    $build_path = "$consumer_root/build"
    Write-Output "[hook-closure] configure/build/run $name ($Configuration)"
    & cmake -S "$consumer_root/source" -B $build_path -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" `
        '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake' `
        "-DCMAKE_PREFIX_PATH=$prefix;E:/SyncForder/CodeRepos/install/RelWithDebInfo" `
        *> "$consumer_root/configure.log"
    if ($LASTEXITCODE -ne 0) { throw "$name configure failed; see $consumer_root/configure.log" }
    & cmake --build $build_path --target all -j 4 -- -k 0 *> "$consumer_root/build.log"
    if ($LASTEXITCODE -ne 0) { throw "$name build failed; see $consumer_root/build.log" }
    $executable = Join-Path $build_path ($targets[$name] + '.exe')
    if (!(Test-Path -LiteralPath $executable)) {
        $executable = Join-Path "$build_path/bin" ($targets[$name] + '.exe')
    }
    & $executable *> "$consumer_root/run.log"
    if ($LASTEXITCODE -ne 0) { throw "$name execution failed ($LASTEXITCODE); see $consumer_root/run.log" }
    & cmake --build $build_path --target all -j 4 -- -k 0 *> "$consumer_root/second-build.log"
    if ($LASTEXITCODE -ne 0 -or
        !(Select-String -LiteralPath "$consumer_root/second-build.log" -SimpleMatch 'no work to do' -Quiet)) {
        throw "$name second build was not a no-op"
    }
    Write-Output "[hook-closure] $name passed"
}
