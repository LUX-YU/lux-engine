param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$PrefixPath,
    [Parameter(Mandatory=$true)][string]$DependencyRoot,
    [string]$IgnorePrefixes = '',
    [string[]]$Names = @('cpp-generated-script','physics2d-script','system-event-await-runtime',
        'flowforge-compiler','lua-script-packager','scene-script-runtime','script-ability-codegen',
        'system-hook-script-binding','cpp-coroutine-script','script-static-ability-specialization',
        'script-ability-ipo','script-authoring')
)
$ErrorActionPreference = 'Stop'
& 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:LUX_FLOWFORGE_LINKER = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/link.exe'
$target_names = @{
    'cpp-generated-script'='lux_cpp_generated_script_consumer';
    'physics2d-script'='lux_physics2d_script_consumer';
    'system-event-await-runtime'='lux_system_event_await_runtime_consumer';
    'flowforge-compiler'='lux_flowforge_compiler_consumer';
    'lua-script-packager'='lua_script_packager_consumer';
    'scene-script-runtime'='lux_scene_script_runtime_consumer';
    'script-ability-codegen'='lux_script_ability_codegen_consumer';
    'system-hook-script-binding'='lux_system_hook_script_consumer';
    'cpp-coroutine-script'='lux_cpp_coroutine_script_consumer';
    'script-static-ability-specialization'='lux_script_static_ability_consumer';
    'script-ability-ipo'='lux_script_ability_ipo_consumer';
    'script-authoring'='lux_script_authoring_consumer';
    'script-runtime-input'='lux_script_runtime_input_consumer';
    'script-description'='lux_script_description_consumer'
}
$bins = @($PrefixPath.Split(';') | ForEach-Object { Join-Path $_ 'bin' })
$clean_path = @($env:PATH.Split(';') | Where-Object {
    $_ -notmatch 'CodeRepos[/\\](install|build)' -and $_ -notmatch 'vcpkg[/\\]installed'
})
$env:PATH = (@($bins) + @("$DependencyRoot/x64-windows/bin") + $clean_path) -join ';'
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$results = @()
foreach ($name in $Names) {
    if (!$target_names.ContainsKey($name)) { throw "Unknown consumer $name" }
    $root = Join-Path $OutputRoot $name
    if (Test-Path -LiteralPath $root) { throw "Fresh root required: $root" }
    New-Item -ItemType Directory -Path $root | Out-Null
    Copy-Item -LiteralPath "$SourceDir/cmake/installed-consumers/$name" -Destination "$root/source" -Recurse
    $record = [ordered]@{consumer=$name;source_sha=(& git -C $SourceDir rev-parse HEAD).Trim();prefixes=$PrefixPath}
    Write-Output "[consumer] $name"
    try {
        $arguments = @('-S',"$root/source",'-B',"$root/build",'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
            '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF',"-DCMAKE_PREFIX_PATH=$PrefixPath",
            "-DCMAKE_IGNORE_PREFIX_PATH=$IgnorePrefixes", "-DVCPKG_INSTALLED_DIR=$DependencyRoot",
            '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake')
        & cmake @arguments *> "$root/configure.log"
        if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
        & cmake --build "$root/build" --target all -j 4 -- -k 0 *> "$root/build.log"
        if ($LASTEXITCODE -ne 0) { throw 'build failed; old binary not executed' }
        $executable = "$root/build/$($target_names[$name]).exe"
        if (!(Test-Path -LiteralPath $executable)) { $executable = "$root/build/bin/$($target_names[$name]).exe" }
        $arguments = if ($name -eq 'script-authoring') { @('save',"$root/bindings.bin") } else { @() }
        & $executable @arguments *> "$root/run.log"
        if ($LASTEXITCODE -ne 0) { throw "run failed ($LASTEXITCODE)" }
        if ($name -eq 'script-authoring') {
            & $executable load "$root/bindings.bin" *> "$root/reload.log"
            if ($LASTEXITCODE -ne 0) { throw 'independent reload failed' }
        }
        & cmake --build "$root/build" --target all -j 4 -- -k 0 *> "$root/second-build.log"
        if ($LASTEXITCODE -ne 0 -or !(Select-String -LiteralPath "$root/second-build.log" -SimpleMatch 'no work to do' -Quiet)) {
            throw 'second build not a no-op'
        }
        $record.executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
        $record.status = 'PASS'
    } catch {
        $record.status = 'FAILED'
        $record.error = $_.Exception.Message
    }
    $results += $record
    $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$OutputRoot/consumers.json" -Encoding utf8
    Write-Output "[consumer] $name $($record.status)"
}
