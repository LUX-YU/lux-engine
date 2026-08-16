# lux-engine overlay triplet: arm64-android.
#
# Same as the vcpkg community triplet PLUS an explicit chainload of the NDK's
# own toolchain file. Why: in classic mode vcpkg hands detect_compiler an
# explicitly-EMPTY VCPKG_CHAINLOAD_TOOLCHAIN_FILE, so vcpkg's internal
# scripts/toolchains/android.cmake (the one that reads ENV{ANDROID_NDK_HOME})
# never loads, and CMake's own Android-Determine then fails with "Neither the
# NDK or a standalone toolchain was found" no matter which env vars are set.
# Pinning the chainload here makes detection deterministic.
#
# Usage:
#   set ANDROID_NDK_HOME to your NDK root (e.g. .../SDK/ndk/28.2.13676358)
#   vcpkg install --triplet arm64-android \
#       --overlay-triplets=<repo>/cmake/triplets <ports...>
#
# Known port gap: luajit does not support android cross-builds ("native"
# supports-clause) — first Android bring-up runs LUX_SCRIPT_HAS_LUA=0; an
# overlay port / NDK-built LuaJIT comes later. See
# .internal/lux-engine-mobile-adaptation-investigation.md (2026-07-20 notes).

if(NOT DEFINED ENV{ANDROID_NDK_HOME})
    message(FATAL_ERROR "arm64-android overlay triplet: set ANDROID_NDK_HOME to your NDK root")
endif()

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 28)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
