param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [string]$CxxPrefix = 'E:/SyncForder/CodeRepos/install/q2/c',
    [string]$ToolsetPrefix = 'E:/SyncForder/CodeRepos/install/q2/toolset',
    [string]$PlatformPrefix = 'E:/SyncForder/CodeRepos/install/RelWithDebInfo',
    [string]$VcpkgRoot = 'D:/Development/vcpkg',
    [string]$DevShell = 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1'
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputRoot) { throw "Fresh evidence output required: $OutputRoot" }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
& $DevShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path.Replace('\','/')
$OutputRoot = (Resolve-Path -LiteralPath $OutputRoot).Path.Replace('\','/')
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir).Replace('\','/')
$commandLog = Join-Path $OutputRoot 'commands.jsonl'
function Invoke-Logged([string]$Name, [string]$Program, [string[]]$Arguments) {
    $log = Join-Path $OutputRoot ($Name + '.log')
    & $Program @Arguments *> $log
    $result = $LASTEXITCODE
    [ordered]@{name=$Name;program=$Program;arguments=$Arguments;exit=$result;utc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Compress -Depth 5 | Add-Content -LiteralPath $commandLog -Encoding utf8
    if ($result -ne 0) { throw "$Name failed ($result); see $log" }
}
function Save-Identity([string]$Name) {
    $binaries = Get-ChildItem -LiteralPath "$BuildDir/bin" -File | Where-Object {
        $_.Name -match 'editor_edit|lux_engine_(editor|core_object|function_ui)' -and $_.Extension -in '.exe','.dll'
    }
    $binaries | ForEach-Object {
        [ordered]@{name=$_.Name;bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$OutputRoot/$Name-binaries.json" -Encoding utf8
    Copy-Item -LiteralPath "$BuildDir/CMakeCache.txt" -Destination "$OutputRoot/$Name-CMakeCache.txt"
    Copy-Item -LiteralPath "$BuildDir/compile_commands.json" -Destination "$OutputRoot/$Name-compile_commands.json"
}
$dependencyBins = "$CxxPrefix/bin;$PlatformPrefix/bin;$VcpkgRoot/installed/x64-windows/bin"
$toolPath = ($env:PATH.Split(';') | Where-Object {
    $_ -notmatch '(?i)CodeRepos[/\\](install|build)|lux-ed1-9905|vcpkg[/\\]installed'
}) -join ';'
$env:PATH = "$dependencyBins;$toolPath"
Invoke-Logged 'tracked-snapshot' 'cmake' @("-DLUX_SOURCE_DIR=$SourceDir",'-P',"$SourceDir/cmake/ValidateTrackedSnapshot.cmake")
$sourceHead = (& git -C $SourceDir rev-parse HEAD).Trim()
$prefixA = "$OutputRoot/sdk-a"
$prefixB = "$OutputRoot/sdk-b"
$common = @('-S',$SourceDir,'-B',$BuildDir,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
    '-DLUX_BUILD_PROFILE=EDITOR','-DBUILD_TESTING=ON',"-DCMAKE_INSTALL_PREFIX=$prefixA",
    "-DCMAKE_PREFIX_PATH=$ToolsetPrefix;$CxxPrefix;$PlatformPrefix",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",'-DVCPKG_MANIFEST_INSTALL=OFF')
$tests = '^(editor_|node_graph_editor$|simulation_object_boundary_test$)'
Invoke-Logged 'diagnostic-configure' 'cmake' ($common + '-DLUX_EDITOR_EDITING_TEST_DIAGNOSTICS=ON')
Invoke-Logged 'diagnostic-build' 'cmake' @('--build',$BuildDir,'--target','all','-j','4','--','-k','0')
Invoke-Logged 'diagnostic-tests' 'ctest' @('--test-dir',$BuildDir,'-R',$tests,'-j','1','-V','--output-on-failure')
Save-Identity 'diagnostic'
Invoke-Logged 'normal-configure' 'cmake' ($common + '-DLUX_EDITOR_EDITING_TEST_DIAGNOSTICS=OFF')
Invoke-Logged 'normal-build' 'cmake' @('--build',$BuildDir,'--target','all','-j','4','--','-k','0')
Invoke-Logged 'normal-tests' 'ctest' @('--test-dir',$BuildDir,'-R',$tests,'-j','1','-V','--output-on-failure')
Invoke-Logged 'second-build' 'cmake' @('--build',$BuildDir,'--target','all','-j','4','--','-k','0')
if (!(Select-String -LiteralPath "$OutputRoot/second-build.log" -SimpleMatch 'no work to do' -Quiet)) {
    throw 'Second all build was not a no-op'
}
Save-Identity 'normal'
Invoke-Logged 'install' 'cmake' @('--install',$BuildDir,'--prefix',$prefixA)
Copy-Item -LiteralPath $prefixA -Destination $prefixB -Recurse
Copy-Item -LiteralPath "$SourceDir/cmake/installed-consumers/editor-editing" -Destination "$OutputRoot/consumer-source" -Recurse
foreach ($variant in @('core-a','core-b','context-b')) {
    $prefix = if ($variant -eq 'core-a') { $prefixA } else { $prefixB }
    $context = $variant -eq 'context-b'
    $consumerBuild = "$OutputRoot/$variant"
    $prefixes = "$prefix;$ToolsetPrefix;$CxxPrefix"
    $bins = "$prefix/bin;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin"
    if ($context) { $prefixes += ";$PlatformPrefix"; $bins += ";$PlatformPrefix/bin" }
    $env:PATH = "$bins;$toolPath"
    $ignored = "$SourceDir;$BuildDir"
    if ($variant -ne 'core-a') { $ignored += ";$prefixA" }
    $arguments = @('-S',"$OutputRoot/consumer-source",'-B',$consumerBuild,'-G','Ninja',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',"-DCMAKE_PREFIX_PATH=$prefixes",
        "-DCMAKE_IGNORE_PREFIX_PATH=$ignored",'-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
        '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF',
        "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake",'-DVCPKG_MANIFEST_INSTALL=OFF')
    if ($context) { $arguments += '-DED1_WITH_CONTEXT=ON' }
    Invoke-Logged "$variant-configure" 'cmake' $arguments
    Invoke-Logged "$variant-build" 'cmake' @('--build',$consumerBuild,'--target','all','-j','4','--','-k','0')
    Invoke-Logged "$variant-run" "$consumerBuild/lux_editor_editing_consumer.exe" @("$prefix/bin")
    if ($context) { Invoke-Logged 'context-signal-run' "$consumerBuild/lux_editor_editing_context_consumer.exe" @() }
    Invoke-Logged "$variant-second-build" 'cmake' @('--build',$consumerBuild,'--target','all','-j','4','--','-k','0')
    if (!(Select-String -LiteralPath "$OutputRoot/$variant-second-build.log" -SimpleMatch 'no work to do' -Quiet)) {
        throw "Consumer second build was not a no-op: $variant"
    }
    if (!$context) {
        $commands = [System.IO.File]::ReadAllText("$consumerBuild/compile_commands.json").Replace('\\','/')
        foreach ($forbidden in @($SourceDir,$BuildDir) + $(if ($variant -eq 'core-b') { @($prefixA) } else { @() })) {
            if ($commands.IndexOf($forbidden,[StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "Consumer compile command contains a forbidden path: $forbidden"
            }
        }
        foreach ($binary in @('lux_editor_editing_consumer.exe','editing_consumer_Text.dll','editing_consumer_Records.dll')) {
            Invoke-Logged "$variant-$binary-imports" 'dumpbin' @('/DEPENDENTS',"$consumerBuild/$binary")
        }
        Invoke-Logged "$variant-core-imports" 'dumpbin' @('/DEPENDENTS',"$prefix/bin/lux_engine_editor_editing.dll")
        $imports = Get-Content "$OutputRoot/$variant-*-imports.log" | Where-Object { $_ -match '^\s+\S+\.dll\s*$' }
        if ($imports -match '(?i)(scene|world|simulation|vulkan|imgui|render|graph|core_object|function_ui)') {
            throw "Forbidden dependency in core consumer: $imports"
        }
    }
}
$env:PATH = "$BuildDir/bin;$dependencyBins;$toolPath"
foreach ($count in @(1000,10000)) {
    foreach ($run in 1..5) {
        Invoke-Logged "cost-$count-$run" "$BuildDir/bin/editor_editing_benchmark.exe" @("$count")
    }
}
[ordered]@{source_head=$sourceHead;source=$SourceDir;build=$BuildDir;cxx=$CxxPrefix;toolset=$ToolsetPrefix;
    platform=$PlatformPrefix;compiler=(Get-Command cl.exe).Source;configuration='RelWithDebInfo';profile='EDITOR';
    diagnostic_tests='PASS';normal_tests='PASS';install='PASS';relocation='PASS';cost_processes=10;
    gui='NOT_TESTED';business_migration='NOT_IMPLEMENTED_ED1_BOUNDARY'} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$OutputRoot/qualification.json" -Encoding utf8
Write-Output "ED-1 qualification completed: $OutputRoot"
