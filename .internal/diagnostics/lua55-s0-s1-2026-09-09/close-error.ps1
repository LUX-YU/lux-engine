param([Parameter(Mandatory=$true)][string]$Root)
$ErrorActionPreference = 'Stop'
$out = Join-Path $Root 'vm-close-error'
if (Test-Path -LiteralPath $out) { throw 'Fresh close-error probe required' }
New-Item -ItemType Directory -Path "$out/source" | Out-Null
Copy-Item -LiteralPath "$PSScriptRoot/close-error.c" -Destination "$out/source/main.c"
@'
cmake_minimum_required(VERSION 3.22)
project(vm_close_error LANGUAGES C)
find_package(LuxLua55 5.5.1 EXACT CONFIG REQUIRED)
add_executable(vm_close_error main.c)
target_link_libraries(vm_close_error PRIVATE LuxLua55::Runtime)
'@ | Set-Content "$out/source/CMakeLists.txt"
cmake -S "$out/source" -B "$out/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DCMAKE_PREFIX_PATH=$Root/relocated/vm" *> "$out/configure.log"
if ($LASTEXITCODE) { throw 'close-error probe configure' }
cmake --build "$out/build" --target all -j 4 -- -k 0 *> "$out/build.log"
if ($LASTEXITCODE) { throw 'close-error probe build' }
cmake --build "$out/build" --target all -j 4 -- -k 0 *> "$out/noop.log"
if ($LASTEXITCODE -or !(Select-String -LiteralPath "$out/noop.log" -Pattern 'no work to do' -SimpleMatch -Quiet)) {
    throw 'close-error probe noop'
}
$dll = "$Root/relocated/vm/bin/lux_lua55.dll"
Copy-Item -LiteralPath $dll -Destination "$out/build/lux_lua55.dll"
& "$out/build/vm_close_error.exe" *> "$out/run.log"
$code = $LASTEXITCODE
if ($code -ne 0 -or !(Select-String -LiteralPath "$out/run.log" -Pattern 'LUA55_CLOSE_ERROR' -SimpleMatch -Quiet)) {
    throw "close-error probe failed: $code"
}
@{exit=$code;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash;
    source_sha256=(Get-FileHash -LiteralPath "$out/source/main.c").Hash;
    executable_sha256=(Get-FileHash -LiteralPath "$out/build/vm_close_error.exe").Hash;
    scope='Installed official Lua55 C API close error; not a new engine cleanup-error channel'} |
    ConvertTo-Json | Set-Content "$out/result.json"
Get-Content -LiteralPath "$out/run.log"
