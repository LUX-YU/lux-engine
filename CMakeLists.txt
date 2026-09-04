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
set(LUX_LUA_VM "LUAJIT" CACHE STRING "Lua VM implementation used by the Lua script backend")
set_property(CACHE LUX_LUA_VM PROPERTY STRINGS LUAJIT LUA54)
string(TOUPPER "${LUX_LUA_VM}" LUX_LUA_VM)
set(_lux_supported_lua_vms LUAJIT LUA54)
if(LUX_SCRIPT_HAS_LUA AND NOT LUX_LUA_VM IN_LIST _lux_supported_lua_vms)
    message(FATAL_ERROR "Unknown LUX_LUA_VM='${LUX_LUA_VM}'. Expected LUAJIT or LUA54")
endif()
unset(_lux_supported_lua_vms)
option(
    LUX_BUILD_PHYSICS2D_SOURCE_PACKAGE
    "Build the source-composed canonical Physics2D Simulation package"
    OFF
)

# Asset and shader cooking is an explicit build-tool path. Default runtime
# profiles do not invoke a host packer; qualification/toolchain builds opt in.
option(LUX_BUILD_PACKED_RENDER_CONTENT "Compile and embed built-in render content" OFF)
option(ENABLE_RENDER_TEST "Enable render integration tests" OFF)
set(LUX_ASSET_PACKER_EXECUTABLE "" CACHE FILEPATH "Optional explicit host lux_asset_packer executable")
set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable legacy asset integration tests" FORCE)

file(GLOB_RECURSE lux_architecture_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/cmake/*.cmake"
    "${CMAKE_SOURCE_DIR}/modules/*.hpp"
    "${CMAKE_SOURCE_DIR}/modules/*.cpp"
    "${CMAKE_SOURCE_DIR}/modules/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/domain/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/domain/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/domain/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/process/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/process/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/process/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/scene/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/scene/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/scene/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/editor/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/editor/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/editor/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/engine/toolchain/*.hpp"
    "${CMAKE_SOURCE_DIR}/engine/toolchain/*.cpp"
    "${CMAKE_SOURCE_DIR}/engine/toolchain/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/extensions/*.hpp"
    "${CMAKE_SOURCE_DIR}/extensions/*.cpp"
    "${CMAKE_SOURCE_DIR}/extensions/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/test/architecture_probes/*.cpp"
    "${CMAKE_SOURCE_DIR}/test/architecture_probes/CMakeLists.txt"
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

set(lux_source_style_stamp "${CMAKE_BINARY_DIR}/lux-source-style-check.stamp")
add_custom_command(
    OUTPUT "${lux_source_style_stamp}"
    COMMAND ${CMAKE_COMMAND}
        "-DLUX_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ValidateSourceStyle.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${lux_source_style_stamp}"
    DEPENDS ${lux_architecture_inputs}
    COMMENT "Validate engine source style"
    VERBATIM
)
add_custom_target(lux_source_style_check ALL DEPENDS "${lux_source_style_stamp}")

cmake_language(DEFER CALL lux_validate_foundation_targets)
cmake_language(DEFER CALL lux_validate_target_dag)
cmake_language(DEFER CALL lux_validate_all_production_targets)
cmake_language(DEFER CALL lux_validate_source_boundaries)
cmake_language(DEFER CALL lux_write_unclassified_target_report)

add_subdirectory(modules)
add_subdirectory(engine)
if(LUX_BUILD_PHYSICS2D_SOURCE_PACKAGE)
    add_subdirectory(extensions)
endif()
if(BUILD_TESTING)
    add_subdirectory(test/architecture_probes)
    if(TARGET flowforge_compiler AND TARGET simulation_script_native)
        add_subdirectory(test/runtime_scripting)
    endif()
    set(lux_architecture_layer_probe_source "${CMAKE_SOURCE_DIR}/cmake/tests/architecture-layer-probe")
    function(lux_add_architecture_layer_probe name consumer dependency should_fail)
        set(probe_binary "${CMAKE_BINARY_DIR}/architecture-layer-probes/${name}")
        add_test(
            NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                "-S${lux_architecture_layer_probe_source}"
                "-B${probe_binary}"
                "-DLUX_ENGINE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
                "-DCONSUMER_LAYER=${consumer}"
                "-DDEPENDENCY_LAYER=${dependency}"
        )
        if(should_fail)
            set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
        endif()
        set_tests_properties(${name} PROPERTIES LABELS "architecture;l1" TIMEOUT 30)
    endfunction()
    lux_add_architecture_layer_probe(architecture_world_domain_positive WORLD DOMAIN FALSE)
    lux_add_architecture_layer_probe(architecture_simulation_domain_positive SIMULATION DOMAIN FALSE)
    lux_add_architecture_layer_probe(architecture_simulation_world_negative SIMULATION WORLD TRUE)
    lux_add_architecture_layer_probe(architecture_domain_world_negative DOMAIN WORLD TRUE)
    lux_add_architecture_layer_probe(architecture_domain_simulation_negative DOMAIN SIMULATION TRUE)
    lux_add_architecture_layer_probe(architecture_process_world_loading_positive PROCESS WORLD FALSE)
    lux_add_architecture_layer_probe(architecture_scene_process_positive SCENE PROCESS FALSE)
    lux_add_architecture_layer_probe(architecture_process_scene_negative PROCESS SCENE TRUE)
    lux_add_architecture_layer_probe(architecture_scene_editor_negative SCENE EDITOR TRUE)
    lux_add_architecture_layer_probe(architecture_toolchain_function_positive TOOLCHAIN FUNCTION FALSE)
    lux_add_architecture_layer_probe(architecture_toolchain_editor_negative TOOLCHAIN EDITOR TRUE)
    lux_add_architecture_layer_probe(architecture_editor_toolchain_positive EDITOR TOOLCHAIN FALSE)
    lux_add_architecture_layer_probe(architecture_function_toolchain_negative FUNCTION TOOLCHAIN TRUE)
endif()
