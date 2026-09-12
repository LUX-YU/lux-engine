param([Parameter(Mandatory=$true)][string]$Root, [string]$Label='vm-version-negative')
$ErrorActionPreference = 'Stop'
$out = Join-Path $Root $Label
if (Test-Path -LiteralPath $out) { throw 'Fresh version probe required' }
New-Item -ItemType Directory -Path "$out/source" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/vm-version.c" -Destination "$out/source/main.c"
@'
cmake_minimum_required(VERSION 3.22)
project(vm_version LANGUAGES C)
find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)
add_executable(vm_version main.c)
target_link_libraries(vm_version PRIVATE LuxLua55::Runtime)
'@ | Set-Content "$out/source/CMakeLists.txt"
cmake -S "$out/source" -B "$out/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    -DCMAKE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/o/lua55 *> "$out/configure.log"
if ($LASTEXITCODE) { throw 'version probe configure' }
cmake --build "$out/build" --target all -j 4 -- -k 0 *> "$out/build.log"
if ($LASTEXITCODE) { throw 'version probe build' }
cmake --build "$out/build" --target all -j 4 -- -k 0 *> "$out/noop.log"
if ($LASTEXITCODE -or !(Select-String -LiteralPath "$out/noop.log" -Pattern 'no work to do' -SimpleMatch -Quiet)) {
    throw 'version probe noop'
}
$results=@()
foreach ($vm in @('match','mismatch')) {
    New-Item -ItemType Directory -Path "$out/$vm" | Out-Null
    Copy-Item -LiteralPath "$out/build/vm_version.exe" -Destination "$out/$vm/vm_version.exe"
    $dll=if ($vm -eq 'match') { 'E:/SyncForder/CodeRepos/install/o/lua55/bin/lux_lua55.dll' } else {
        'E:/SyncForder/CodeRepos/build/deps/lua54-vcpkg/x64-windows/bin/lua.dll' }
    Copy-Item -LiteralPath $dll -Destination "$out/$vm/lux_lua55.dll"
    & "$out/$vm/vm_version.exe" *> "$out/$vm.log"
    $code=$LASTEXITCODE
    if ($code -ne $(if ($vm -eq 'match') {0} else {13})) { throw "version probe $vm failed: $code" }
    $results+=@{case=$vm;exit=$code;dll_sha256=(Get-FileHash $dll).Hash;
        executable_sha256=(Get-FileHash "$out/$vm/vm_version.exe").Hash}
}
$results | ConvertTo-Json | Set-Content "$out/results.json"
Write-Output 'VM_VERSION_NEGATIVE matching=0 mismatch=13 PASS'
