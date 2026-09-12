cmake_minimum_required(VERSION 3.22)
project(lux-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(WIN32)
    set(CMAKE_DEBUG_POSTFIX d)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(TargetPlatform)
include(HostTools)

find_package(lux-cmake-toolset CONFIG REQUIRED)

if(CMAKE_CROSSCOMPILING)
    find_package(lux-cxx REQUIRED COMPONENTS
        core memory binary time units diagnostic abi compile_time algorithm
    )
else()
    find_package(lux-cxx REQUIRED COMPONENTS
        core memory binary time units diagnostic abi compile_time algorithm
        reflection_generator
    )
endif()

set(CMAKE_TOOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tools")
set(PROJECT_NAMESPACE lux::engine)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(LUX_BUILD_PROFILE "DEVELOPER" CACHE STRING "Build profile")
set_property(CACHE LUX_BUILD_PROFILE PROPERTY STRINGS DEVELOPER PLAYER EDITOR TOOLCHAIN)
string(TOUPPER "${LUX_BUILD_PROFILE}" LUX_BUILD_PROFILE)
set(_lux_valid_profiles DEVELOPER PLAYER EDITOR TOOLCHAIN)
if(NOT LUX_BUILD_PROFILE IN_LIST _lux_valid_profiles)
    message(FATAL_ERROR
        "Unknown LUX_BUILD_PROFILE='${LUX_BUILD_PROFILE}'. Expected one of: "
        "${_lux_valid_profiles}"
    )
endif()

lux_detect_build_platform()
lux_configure_host_tools()

# Product composition is explicit. Toolchain and Editor are separate upper
# layers; no canonical Authoring layer remains.
set(LUX_PROFILE_HAS_RUNTIME OFF)
set(LUX_PROFILE_HAS_REFERENCE_PLAYER OFF)
if(LUX_BUILD_PROFILE STREQUAL "EDITOR")
    set(LUX_PROFILE_HAS_EDITOR ON)
else()
    set(LUX_PROFILE_HAS_EDITOR OFF)
endif()
if(LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    set(LUX_PROFILE_HAS_TOOLCHAIN ON)
else()
    set(LUX_PROFILE_HAS_TOOLCHAIN OFF)
endif()
set(LUX_BUILD_COMPONENT_LUA_META OFF CACHE BOOL "Build Lua component metadata" FORCE)
if(LUX_TARGET_OS STREQUAL "ANDROID")
    set(LUX_SCRIPT_HAS_LUA OFF CACHE BOOL "Build Lua backend" FORCE)
else()
    option(LUX_SCRIPT_HAS_LUA "Build Lua backend" ON)
endif()
if(DEFINED LUX_LUA_VM)
    message(FATAL_ERROR "LUX_LUA_VM was removed. Lux supports only Lua 5.5; remove the old selector.")
endif()
option(
    LUX_BUILD_PHYSICS2D
    "Build the canonical Physics2D Simulation builtin"
    OFF
)

# Asset and shader cooking is an explicit build-tool path. Default runtime
# profiles do not invoke a host packer; qualification/toolchain builds opt in.
option(LUX_BUILD_PACKED_RENDER_CONTENT "Compile and embed built-in render content" OFF)
set(LUX_ASSET_PACKER_EXECUTABLE "" CACHE FILEPATH "Optional explicit host lux_asset_packer executable")

add_subdirectory(modules)
add_subdirectory(engine)
