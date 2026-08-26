cmake_minimum_required(VERSION 3.22)
project(lux-engine VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

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
include(TargetArchitecture)
include(ModuleLayout)
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

# The four historical command-line values remain accepted while upper product
# layers are absent. They do not change the L1 target graph.
set(LUX_BUILD_PROFILE "DEVELOPER" CACHE STRING "Compatibility build profile")
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
lux_validate_module_layout("${CMAKE_CURRENT_SOURCE_DIR}/modules")

# L1 has no Editor/Toolchain/Host product composition. Module targets may use
# these compatibility values only to disable optional upper-layer sidecars.
set(LUX_PROFILE_HAS_RUNTIME OFF)
set(LUX_PROFILE_HAS_REFERENCE_PLAYER OFF)
set(LUX_PROFILE_HAS_EDITOR OFF)
set(LUX_PROFILE_HAS_TOOLCHAIN OFF)
set(LUX_BUILD_COMPONENT_LUA_META OFF CACHE BOOL "Build Lua component metadata" FORCE)
set(LUX_ENABLE_FLOWFORGE_MLIR OFF CACHE BOOL "Build FlowForge MLIR backend" FORCE)
if(LUX_TARGET_OS STREQUAL "ANDROID")
    set(LUX_SCRIPT_HAS_LUA OFF CACHE BOOL "Build Lua backend" FORCE)
else()
    option(LUX_SCRIPT_HAS_LUA "Build Lua backend" ON)
endif()

# Asset and shader cooking belongs to a future content/toolchain layer. The L0
# libraries compile without invoking the quarantined Engine host tools.
set(LUX_BUILD_PACKED_RENDER_CONTENT OFF CACHE BOOL
    "Compile and embed built-in render content" FORCE)
set(ENABLE_RENDER_TEST OFF CACHE BOOL "Enable render integration tests" FORCE)
set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable legacy asset integration tests" FORCE)

file(GLOB_RECURSE lux_architecture_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/cmake/*.cmake"
    "${CMAKE_SOURCE_DIR}/modules/*.hpp"
    "${CMAKE_SOURCE_DIR}/modules/*.cpp"
    "${CMAKE_SOURCE_DIR}/modules/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/world/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/world/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/world/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/simulation/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/simulation/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/simulation/CMakeLists.txt"
)
set(lux_architecture_stamp "${CMAKE_BINARY_DIR}/lux-architecture-check.stamp")
add_custom_command(
    OUTPUT "${lux_architecture_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DLUX_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DLUX_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DLUX_REPORT_PATH=${CMAKE_BINARY_DIR}/semantic-architecture-debt.txt"
        -P "${CMAKE_SOURCE_DIR}/cmake/ValidateSourceArchitecture.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${lux_architecture_stamp}"
    DEPENDS ${lux_architecture_inputs}
    COMMENT "Validate vNext source architecture boundaries"
    VERBATIM
)
add_custom_target(lux_architecture_check ALL DEPENDS "${lux_architecture_stamp}")

cmake_language(DEFER CALL lux_validate_foundation_targets)
cmake_language(DEFER CALL lux_validate_target_dag)
cmake_language(DEFER CALL lux_validate_all_production_targets)
cmake_language(DEFER CALL lux_validate_source_boundaries)
cmake_language(DEFER CALL lux_write_unclassified_target_report)

add_subdirectory(modules)
add_subdirectory(engine)
