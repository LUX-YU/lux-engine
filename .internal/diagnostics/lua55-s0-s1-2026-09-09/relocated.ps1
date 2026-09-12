param([Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference = 'Stop'
$dev = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/s5/source'
$qual = 'E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-region-opt/final-source'
$out = Join-Path $Root 'relocated'
if (Test-Path -LiteralPath $out) { throw 'Fresh relocation root required' }
New-Item -ItemType Directory -Path $out | Out-Null
$pairs = @(
    @('E:/SyncForder/CodeRepos/install/o/q/s1-lua55', "$out/sdk"),
    @('E:/SyncForder/CodeRepos/install/o/q/s1-t', "$out/tools"),
    @('E:/SyncForder/CodeRepos/install/o/lua55', "$out/vm"))
foreach ($pair in $pairs) { Copy-Item -LiteralPath $pair[0] -Destination $pair[1] -Recurse }
$sha = (git -C $qual rev-parse HEAD).Trim()
foreach ($name in @('script-lua-values','lua-script-packager')) {
    New-Item -ItemType Directory -Path "$out/$name" | Out-Null
    Copy-Item -LiteralPath "$qual/cmake/installed-consumers/$name" -Destination "$out/$name/source" -Recurse
}
. 'D:/Development/Mircosoft/VisualStudio/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$prefixes = "$out/sdk;$out/tools;$out/vm;E:/SyncForder/CodeRepos/install/q2/toolset;E:/SyncForder/CodeRepos/install/q2/c"
$env:PATH = ((@($prefixes.Split(';') | ForEach-Object { "$_/bin" }) +
    @('D:/Development/vcpkg/installed/x64-windows/bin') + @($env:PATH.Split(';') | Where-Object {
        $_ -notmatch 'CodeRepos[/\\](install|build)' -and $_ -notmatch 'vcpkg[/\\]installed'
    })) -join ';')
$hidden = @()
$results = @()
try {
    # Explicitly named controlled directories only. No delete, replacement or computed outside target.
    $unavailable = @($qual,
        'E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/d',
        'E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/l',
        'E:/SyncForder/CodeRepos/build/RelWithDebInfo/o/w/t') + @($pairs | ForEach-Object { $_[0] })
    foreach ($directory in $unavailable) {
        $resolved = (Resolve-Path -LiteralPath $directory).Path
        $expected = [IO.Path]::GetFullPath($directory)
        if ($resolved -ne $expected -or !$resolved.StartsWith('E:\SyncForder\CodeRepos\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Invalid controlled path: $resolved"
        }
        $destination = "$resolved.s1-unavailable"
        if (Test-Path -LiteralPath $destination) { throw "Existing relocation target: $destination" }
        Move-Item -LiteralPath $resolved -Destination $destination
        $hidden += @{original=$resolved;temporary=$destination}
    }
    $hidden | ConvertTo-Json | Set-Content "$out/unavailable-paths.json"
    foreach ($name in @('script-lua-values','lua-script-packager')) {
        $consumer = "$out/$name"
        & cmake -S "$consumer/source" -B "$consumer/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `
            -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF "-DCMAKE_PREFIX_PATH=$prefixes" `
            '-DCMAKE_IGNORE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/RelWithDebInfo;E:/SyncForder/CodeRepos/install/o/q/d;E:/SyncForder/CodeRepos/install/o/q/t' `
            -DVCPKG_INSTALLED_DIR=D:/Development/vcpkg/installed `
            -DCMAKE_TOOLCHAIN_FILE=D:/Development/vcpkg/scripts/buildsystems/vcpkg.cmake *> "$consumer/configure.log"
        if ($LASTEXITCODE) { throw "$name configure failed" }
        & cmake --build "$consumer/build" --target all -j 4 -- -k 0 *> "$consumer/build.log"
        if ($LASTEXITCODE) { throw "$name build failed" }
        $exe = if ($name -eq 'script-lua-values') { 'lux_script_lua_values_consumer.exe' } else { 'lua_script_packager_consumer.exe' }
        & "$consumer/build/$exe" *> "$consumer/run.log"
        if ($LASTEXITCODE) { throw "$name execution failed" }
        & cmake --build "$consumer/build" --target all -j 4 -- -k 0 *> "$consumer/noop.log"
        if ($LASTEXITCODE -or !(Select-String -LiteralPath "$consumer/noop.log" -Pattern 'no work to do' -SimpleMatch -Quiet)) {
            throw "$name second build was not idle"
        }
        $commands = Get-Content "$consumer/build/compile_commands.json" -Raw
        if ($commands -match 'final-source|/o/w/[dlt]/|/s5/source|/pinclude/|/sinclude/') { throw 'Private/source path leaked' }
        $results += @{consumer=$name;source_commit=$sha;status='PASS';original_paths_unavailable=$true;
            exe_sha256=(Get-FileHash "$consumer/build/$exe").Hash}
        $results | ConvertTo-Json | Set-Content "$out/results.json"
        Write-Output "$name relocated PASS"
    }
    & "$dev/cmake/RunScriptSR5ValueIncremental.ps1" -ConsumerRoot "$out/script-lua-values" `
        -InstalledTemplate "$out/sdk/share/lux-engine-function/script_lua/cmake_scripts/template/lua_value.template" `
        -EvidenceRoot "$out/incremental"
    if ($LASTEXITCODE) { throw 'Incremental qualification failed' }
} finally {
    [array]::Reverse($hidden)
    foreach ($entry in $hidden) {
        if (Test-Path -LiteralPath $entry.original) { throw "Cannot restore occupied path: $($entry.original)" }
        Move-Item -LiteralPath $entry.temporary -Destination $entry.original
    }
    Write-Output 'Controlled source/build/SDK paths restored'
}
