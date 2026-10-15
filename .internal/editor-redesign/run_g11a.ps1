param(
    [string]$Source = 'E:\RenderV2Rebuild\G11A\source',
    [string]$Build = 'E:\RenderV2Rebuild\G11A/build/release',
    [string]$Action = 'configure',
    [string]$Config = 'RelWithDebInfo',
    [string]$ExtraFlags = '',
    [string]$Filter = '^render\.'
)
$ErrorActionPreference = 'Stop'
if (-not $env:VSCMD_VER) { & 'D:/Softwares/MicroSoft/VS/2022/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation }
$env:Path = "D:/DevTools/vcpkg/downloads/tools/cmake-3.31.10-windows/cmake-3.31.10-windows-x86_64/bin;D:/Softwares/ninja;D:/Softwares/VulkanSDK/1.3.280.0/Bin;$env:Path"
$env:VULKAN_SDK = 'D:/Softwares/VulkanSDK/1.3.280.0'
$env:ASAN_OPTIONS = 'alloc_dealloc_mismatch=1'
$evidence = 'E:\RenderV2Rebuild\G11A/evidence'
New-Item -ItemType Directory -Force $evidence | Out-Null
$log = Join-Path $evidence "$Action-$Config-$(Get-Date -Format yyyyMMdd-HHmmss-fff).log"
"Source=$Source Build=$Build Action=$Action Config=$Config Flags=$ExtraFlags" | Tee-Object $log
switch ($Action) {
    'configure' {
        & cmake -S "$Source/cmake/render-v2-bootstrap" -B $Build -G Ninja `
            "-DCMAKE_BUILD_TYPE=$Config" -DBUILD_TESTING=ON `
            -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
            -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
            "-DCMAKE_CXX_FLAGS=/EHsc $ExtraFlags" -DCMAKE_CXX_SCAN_FOR_MODULES=OFF `
            -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF `
            "-DCMAKE_INSTALL_PREFIX=$Build/sdk" 2>&1 | Tee-Object -Append $log
    }
    'build' { & cmake --build $Build --target all -j 4 -- -k 0 2>&1 | Tee-Object -Append $log }
    'test' { & ctest --test-dir $Build --output-on-failure -j 1 -R $Filter 2>&1 | Tee-Object -Append $log }
    'snapshot' {
        & cmake "-DLUX_SOURCE_DIR=$Source" -P "$Source/cmake/ValidateTrackedSnapshot.cmake" 2>&1 |
            Tee-Object -Append $log
    }
    'root-configure' {
        & cmake -S $Source -B $Build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
            -DCMAKE_PREFIX_PATH=E:/SyncForder/CodeRepos/install/RelWithDebInfo `
            -DCMAKE_TOOLCHAIN_FILE=D:/DevTools/vcpkg/scripts/buildsystems/vcpkg.cmake `
            -DBUILD_TESTING=ON -DLUX_BUILD_PROFILE=TOOLCHAIN `
            -DLUX_SPIRV_TOOLS_SHARED=D:/Softwares/VulkanSDK/1.3.280.0/Lib/SPIRV-Tools-shared.lib `
            -DLUX_SPIRV_TOOLS_INCLUDE=D:/Softwares/VulkanSDK/1.3.280.0/Include 2>&1 |
            Tee-Object -Append $log
    }
}
$result = $LASTEXITCODE
"EXIT=$result" | Tee-Object -Append $log
exit $result
