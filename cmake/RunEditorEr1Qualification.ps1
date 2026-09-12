param(
    [Parameter(Mandatory = $true)][string]$SourceDirectory,
    [Parameter(Mandatory = $true)][string]$QualificationRoot,
    [Parameter(Mandatory = $true)][string]$Commit,
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Ninja,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot,
    [Parameter(Mandatory = $true)][string]$ToolsetPrefix,
    [Parameter(Mandatory = $true)][string]$CxxPrefix,
    [Parameter(Mandatory = $true)][string]$BuildDependencyPrefix,
    [Parameter(Mandatory = $true)][string]$SeedPak,
    [Parameter(Mandatory = $true)][string]$DeveloperShell,
    [string]$TestFont = ''
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $QualificationRoot) { throw 'Qualification requires a new directory' }
$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
$revision = (& git -C $source rev-parse $Commit).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve candidate' }
if ((& git -C $source rev-parse HEAD).Trim() -ne $revision) { throw 'Validate the actual candidate checkout' }
& $CMake "-DLUX_SOURCE_DIR=$source" -P (Join-Path $source 'cmake/ValidateTrackedSnapshot.cmake')
if ($LASTEXITCODE -ne 0) { throw 'Candidate snapshot failed validation' }
New-Item -ItemType Directory -Path $QualificationRoot | Out-Null
$qroot = (Resolve-Path -LiteralPath $QualificationRoot).Path
$clone = Join-Path $qroot 'src'
$build = Join-Path $qroot 'build'
$logs = Join-Path $qroot 'raw'
New-Item -ItemType Directory -Path $logs | Out-Null
function Invoke-Logged([string]$Name, [string]$Program, [string[]]$Arguments) {
    $path = Join-Path $logs ($Name + '.log')
    & $Program @Arguments *> $path
    $result = $LASTEXITCODE
    if ($result -ne 0) { throw "$Name failed ($result). See $path" }
    Write-Output "$Name PASS"
}
Invoke-Logged 'clone' 'git' @('clone', '--no-hardlinks', '--no-checkout', $source, $clone)
Invoke-Logged 'checkout' 'git' @('-C', $clone, 'checkout', '--detach', $revision)
Invoke-Logged 'tracked-snapshot' $CMake @("-DLUX_SOURCE_DIR=$clone", '-P', "$clone/cmake/ValidateTrackedSnapshot.cmake")
@(& git -C $clone ls-files | ForEach-Object {
    [pscustomobject]@{ path = $_; sha256 = (Get-FileHash -LiteralPath (Join-Path $clone $_) -Algorithm SHA256).Hash }
}) | Export-Csv -LiteralPath "$qroot/source-files.sha256.csv" -NoTypeInformation
& $DeveloperShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$parserRuntime = Join-Path $env:VCINSTALLDIR 'Tools/Llvm/x64/bin'
if (!(Test-Path -LiteralPath "$parserRuntime/libclang.dll")) { throw 'Configured MSVC parser runtime missing' }
# The existing generator dynamically loads libclang. Match the working MSVC toolchain;
# vcpkg also supplies an older libclang with different alias-template deduction support.
$env:PATH = "$parserRuntime;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;" + $env:PATH
Get-FileHash -LiteralPath "$parserRuntime/libclang.dll" -Algorithm SHA256 |
    ConvertTo-Json | Set-Content -LiteralPath "$qroot/parser-runtime.json" -Encoding utf8
$toolchain = "$VcpkgRoot/scripts/buildsystems/vcpkg.cmake"
Invoke-Logged 'configure' $CMake @('-S', $clone, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DCMAKE_CXX_COMPILER=$compiler",
    '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DCMAKE_INSTALL_PREFIX=$qroot/sdk",
    "-DCMAKE_PREFIX_PATH=$ToolsetPrefix;$CxxPrefix;$BuildDependencyPrefix", '-DLUX_BUILD_PROFILE=EDITOR',
    '-DBUILD_TESTING=ON', '-DLUX_EDITOR_DIAGNOSTICS=OFF', '-DLUX_EDITOR_EDITING_TEST_DIAGNOSTICS=OFF',
    '-DLUX_BUILD_PACKED_RENDER_CONTENT=ON', "-DLUX_EDITOR_SEED_PAK=$SeedPak")
Invoke-Logged 'build-all' $CMake @('--build', $build, '--target', 'all', '-j', '4', '--', '-k', '0')
Invoke-Logged 'build-noop' $CMake @('--build', $build, '--target', 'all', '-j', '4', '--', '-k', '0')
if ((Get-Content -LiteralPath "$logs/build-noop.log" -Raw) -notmatch 'ninja: no work to do') {
    throw 'Second all build was not a no-op'
}
# Content, not timestamps, binds this build. A restored mtime must not hide an edited source.
foreach ($file in Import-Csv -LiteralPath "$qroot/source-files.sha256.csv") {
    if ((Get-FileHash -LiteralPath (Join-Path $clone $file.path)).Hash -ne $file.sha256) {
        throw "Source content changed during build; tests prohibited: $($file.path)"
    }
}
if ($TestFont) {
    Get-FileHash -LiteralPath $TestFont | ConvertTo-Json |
        Set-Content -LiteralPath "$qroot/external-font-identity.json" -Encoding utf8
}
$ctest = Join-Path (Split-Path $CMake) 'ctest.exe'
$cleanPath = ($env:PATH -split ';' | Where-Object {
    $_ -and $_ -notmatch '(?i)lux-er1|lux-sv1|install[/\\]RelWithDebInfo|build[/\\]RelWithDebInfo'
}) -join ';'
$env:PATH = "$build/bin;$parserRuntime;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;$cleanPath"
Invoke-Logged 'ctest' $ctest @('--test-dir', $build, '--output-on-failure', '-j', '1')
Copy-Item -LiteralPath "$build/Testing/Temporary/LastTest.log" -Destination "$logs/ctest-details.log"
foreach ($variant in @('base', 'alternate', 'multiple_equal', 'multiple_reverse', 'multiple_lifecycle',
                      'multiple_shared_equal', 'multiple_shared_reverse', 'multiple_shared_lifecycle',
                      'late_close', 'late_selection', 'reentrant_close', 'image_lifetime',
                      'view_failure', 'coordinate_1024', 'coordinate_256', 'resolved_source', 'viewport_input')) {
    Invoke-Logged "gpu-$variant" "$build/bin/editor_scene_gpu_test.exe" @($SeedPak, "$qroot/images", $variant)
}
Invoke-Logged 'gpu-foreign' "$build/bin/editor_foreign_renderer_test.exe" @()
Invoke-Logged 'gpu-application-lifecycle' "$build/bin/editor_application_lifecycle_test.exe" @()
Invoke-Logged 'ui-cold-default' "$build/bin/ui_cold_input_test.exe" @()
if ($TestFont) {
    Invoke-Logged 'ui-cold-font' "$build/bin/ui_cold_input_test.exe" @($TestFont)
    Invoke-Logged 'gpu-cold-font' "$build/bin/lux_editor_er1.exe" @('--assets', $SeedPak, '--font', $TestFont,
        '--hidden', '--frames', '100', '--validation')
}
$normalRules = Get-Content -LiteralPath "$build/build.ninja" -Raw
if ($normalRules -match 'LUX_SV1_DIAGNOSTICS=1|EditorAllocationDiagnostics\.cpp\.obj:|ClientAllocationDiagnostics\.cpp\.obj:|fsanitize=address') {
    throw 'Normal SDK qualification rejected: diagnostic instrumentation appears in build rules'
}
$sdk = Join-Path $qroot 'sdk'
Invoke-Logged 'install' $CMake @('--install', $build, '--prefix', $sdk, '--config', 'RelWithDebInfo')
$relocated = Join-Path $qroot 'relocated-sdk'
Copy-Item -LiteralPath $sdk -Destination $relocated -Recurse
foreach ($location in @('sdk', 'relocated-sdk')) {
    $prefix = Join-Path $qroot $location
    $env:PATH = "$prefix/bin;$parserRuntime;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;$cleanPath"
    foreach ($consumer in @('editor-ui', 'editor-scene', 'editor-scene-readers', 'editor-tooling', 'editor-editing')) {
        $input = Join-Path $qroot "consumer-input/$consumer"
        if (!(Test-Path -LiteralPath $input)) {
            New-Item -ItemType Directory -Path (Split-Path $input) -Force | Out-Null
            Copy-Item -LiteralPath "$clone/cmake/installed-consumers/$consumer" -Destination $input -Recurse
        }
        $output = Join-Path $qroot "consumers/$location/$consumer"
        Invoke-Logged "$location-$consumer-configure" $CMake @('-S', $input, '-B', $output, '-G', 'Ninja',
            "-DCMAKE_CXX_COMPILER=$compiler",
            "-DCMAKE_MAKE_PROGRAM=$Ninja", '-DCMAKE_BUILD_TYPE=RelWithDebInfo', "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
            "-DCMAKE_PREFIX_PATH=$prefix;$ToolsetPrefix;$CxxPrefix", '-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF',
            '-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
            "-DLUX_TEST_FONT=$TestFont")
        Invoke-Logged "$location-$consumer-build" $CMake @('--build', $output, '--target', 'all', '-j', '4', '--', '-k', '0')
        Invoke-Logged "$location-$consumer-noop" $CMake @('--build', $output, '--target', 'all', '-j', '4', '--', '-k', '0')
        if ((Get-Content -LiteralPath "$logs/$location-$consumer-noop.log" -Raw) -notmatch 'ninja: no work to do') {
            throw "Consumer second build was not a no-op: $location/$consumer"
        }
        if ($consumer -eq 'editor-editing') {
            Invoke-Logged "$location-$consumer-test" "$output/lux_editor_editing_consumer.exe" @("$prefix/bin")
        } else {
            Invoke-Logged "$location-$consumer-test" $ctest @('--test-dir', $output, '--output-on-failure', '-j', '1')
        }
        $commands = (Get-Content -LiteralPath "$output/compile_commands.json" -Raw) +
            (Get-Content -LiteralPath "$output/build.ninja" -Raw)
        $excludedLocations = @($source, $clone, $build, $BuildDependencyPrefix)
        if ($location -eq 'relocated-sdk') { $excludedLocations += $sdk }
        foreach ($forbidden in $excludedLocations) {
            $normalized = $forbidden.Replace('\', '/').TrimEnd('/')
            if ($commands.Replace('\\', '/').Replace('\', '/').IndexOf($normalized, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "Consumer uses excluded source/build/dependency location: $forbidden"
            }
        }
    }
}
$identity = [ordered]@{ source_commit = $revision; source = $clone; build = $build;
    sdk = $sdk; relocated_sdk = $relocated; configuration = 'RelWithDebInfo'; diagnostics = $false;
    er1_complete = $false; limitation = 'This qualifies the retained-entry continuation snapshot; deletion and cost gates are separate';
    artifacts = @(Get-ChildItem -LiteralPath "$build/bin" -File | Where-Object Extension -in '.dll','.exe' | ForEach-Object {
        @{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    }) }
$identity | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$qroot/qualification.json" -Encoding utf8
Write-Output "Clean candidate qualification PASS: $revision (ER-1 completion not asserted)"
