param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [Parameter(Mandatory=$true)][string]$InstallRoot,
    [Parameter(Mandatory=$true)][string]$CxxPrefix,
    [Parameter(Mandatory=$true)][string]$ToolsetPrefix,
    [Parameter(Mandatory=$true)][string]$FoundationPrefix,
    [string[]]$Profiles = @('developer', 'toolchain', 'physics-off', 'lua-off', 'lua54')
)
$ErrorActionPreference = 'Stop'
& 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$source_path = (Resolve-Path -LiteralPath $SourceDir).Path
$sha = (& git -C $source_path rev-parse HEAD).Trim()
$results = @()
foreach ($profile in $Profiles) {
    if ($profile -notin @('developer','toolchain','physics-off','lua-off','lua54')) { throw "Unsupported profile $profile" }
    $short = @{developer='d'; toolchain='t'; 'physics-off'='p'; 'lua-off'='n'; lua54='l'}[$profile]
    $build = Join-Path $BuildRoot $short
    $prefix = Join-Path $InstallRoot $short
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $record = [ordered]@{source_sha=$sha; profile=$profile; build_type='RelWithDebInfo'; build=$build; prefix=$prefix}
    Write-Output "[v3] $profile start"
    try {
        & cmake "-DLUX_SOURCE_DIR=$source_path" -P "$source_path/cmake/ValidateTrackedSnapshot.cmake" *> "$build/tracked.log"
        if ($LASTEXITCODE -ne 0) { throw 'tracked snapshot failed' }
        $engine_profile = if ($profile -eq 'toolchain') { 'TOOLCHAIN' } else { 'DEVELOPER' }
        $vm = if ($profile -eq 'lua54') { 'LUA54' } else { 'LUAJIT' }
        $physics = if ($profile -eq 'physics-off') { 'OFF' } else { 'ON' }
        $lua = if ($profile -in @('toolchain','lua-off')) { 'OFF' } else { 'ON' }
        $arguments = @('-S',$source_path,'-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
            "-DLUX_BUILD_PROFILE=$engine_profile", "-DLUX_LUA_VM=$vm", "-DLUX_BUILD_PHYSICS2D=$physics",
            "-DLUX_SCRIPT_HAS_LUA=$lua", '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            '-DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake',
            "-DCMAKE_PREFIX_PATH=$ToolsetPrefix;$CxxPrefix;$FoundationPrefix", "-DCMAKE_INSTALL_PREFIX=$prefix",
            '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF','-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF')
        & cmake @arguments *> "$build/configure.log"
        if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
        & cmake --build $build --target all -j 4 -- -k 0 *> "$build/all.log"
        if ($LASTEXITCODE -ne 0) { throw 'all build failed; no old executable was run' }
        $test_args = @('--test-dir',$build,'-C','RelWithDebInfo','--output-on-failure')
        if ($profile -notin @('developer','toolchain')) {
            $test_args += @('-R','(script|scene|simulation|physics|artifact|semantic|architecture)')
        }
        & ctest @test_args *> "$build/ctest.log"
        $record.ctest_exit = $LASTEXITCODE
        & cmake --build $build --target all -j 4 -- -k 0 *> "$build/second-build.log"
        $record.second_build_noop = $LASTEXITCODE -eq 0 -and
            (Select-String -LiteralPath "$build/second-build.log" -SimpleMatch 'no work to do' -Quiet)
        & cmake --install $build *> "$build/install.log"
        $record.install_exit = $LASTEXITCODE
        $record.status = if ($record.ctest_exit -eq 0 -and $record.second_build_noop -and $record.install_exit -eq 0) {
            'PASS'
        } else { 'FAILED' }
        $record.ctest_summary = @(Select-String -LiteralPath "$build/ctest.log" -Pattern 'tests passed|tests failed|Total Test' |
            ForEach-Object Line)
    } catch {
        $record.status = 'BLOCKED'
        $record.error = $_.Exception.Message
    }
    $results += $record
    $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $BuildRoot 'qualification.json') -Encoding utf8
    Write-Output "[v3] $profile $($record.status)"
}
