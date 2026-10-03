cmake_minimum_required(VERSION 3.24)
project(ec2_capabilities LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(EC2_MODE "NATIVE" CACHE STRING "NATIVE, LUA, or SCENE")
foreach(retired lux/engine/editor/storage/ProjectOpenData.hpp lux/engine/editor/assets/AssetImporter.hpp
        lux/engine/editor/material/MaterialPreviewStore.hpp)
    if(EXISTS "${SDK_PREFIX}/include/${retired}")
        message(FATAL_ERROR "retired_installed_interface: ${retired}")
    endif()
endforeach()
find_package(lux-engine-scene-script-assets REQUIRED COMPONENTS scene_script_assets)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_cpp_static)
set(engine_source "E:/SyncForder/CodeRepos/lux-engine-ec2-qualified")
set(EC2_MODE SCENE)
enable_testing()

# Test bodies are copied; all production headers, libraries and generators come from the SDK.
function(fixture origin destination)
    configure_file("${engine_source}/${origin}" "${CMAKE_CURRENT_BINARY_DIR}/${destination}" COPYONLY)
endfunction()
if(EC2_MODE STREQUAL "NATIVE")
    fixture(engine/scene/scripting/assets/test/access.cpp access.cpp)
    fixture(engine/scene/scripting/assets/test/lifecycle.cpp lifecycle.cpp)
    add_executable(ec2_consumer "${CMAKE_CURRENT_BINARY_DIR}/access.cpp" "${CMAKE_CURRENT_BINARY_DIR}/lifecycle.cpp")
    target_link_libraries(ec2_consumer PRIVATE lux::engine::scene::scene_script_assets
        lux::engine::simulation::simulation_script_cpp_static)
    add_test(NAME installed.ec2.native COMMAND ec2_consumer)
    foreach(forbidden lux::engine::function::script_lua lux::engine::simulation::simulation_script_lua
            lux::engine::editor::editor_storage lux::engine::function::render LuxLua55::Runtime LLVMCore)
        if(TARGET ${forbidden})
            message(FATAL_ERROR "Native asset capability imported ${forbidden}")
        endif()
    endforeach()
else()
    find_package(lux-engine-scene-script-assets-lua REQUIRED COMPONENTS scene_script_assets_lua)
    fixture(engine/scene/scripting/assets_lua/test/assets.lua assets.lua)
    if(EC2_MODE STREQUAL "LUA")
        fixture(engine/scene/scripting/assets_lua/test/runtime.cpp runtime.cpp)
        add_executable(ec2_consumer "${CMAKE_CURRENT_BINARY_DIR}/runtime.cpp")
        target_link_libraries(ec2_consumer PRIVATE lux::engine::scene::scene_script_assets_lua
            lux::engine::simulation::simulation_script)
        add_test(NAME installed.ec2.lua COMMAND ec2_consumer "${CMAKE_CURRENT_BINARY_DIR}/assets.lua")
    elseif(EC2_MODE STREQUAL "SCENE")
        find_package(lux-engine-scene-script-runtime REQUIRED COMPONENTS scene_script_runtime)
        find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)
        find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)
        find_package(lux-cmake-toolset CONFIG REQUIRED)
        include_component_cmake_scripts(lux::engine::function::script_core)
        include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/schemas.cpp" "int ec2_schema_anchor;\n")
        add_library(ec2_schemas STATIC "${CMAKE_CURRENT_BINARY_DIR}/schemas.cpp")
        target_link_libraries(ec2_schemas PUBLIC lux::engine::scene::scene_script_assets
            lux::engine::scene::scene_script_skeleton lux::engine::simulation::simulation_script)
        lux_script_abilities(TARGET ec2_schemas SOURCES
            "${SDK_PREFIX}/include/lux/engine/scene/scripting/AssetAbility.hpp"
            "${SDK_PREFIX}/include/lux/engine/scene/scripting/SkeletonAbility.hpp"
            "${SDK_PREFIX}/include/lux/engine/simulation/abilities/DelayAbility.hpp"
            LOGICAL_PATHS lux/engine/scene/scripting/AssetAbility.hpp lux/engine/scene/scripting/SkeletonAbility.hpp
                lux/engine/simulation/abilities/DelayAbility.hpp)
        fixture(engine/scene/integration/script/test/symbols.json symbols.json)
        lux_package_lua_script(NAME ec2_package SOURCE "${CMAKE_CURRENT_BINARY_DIR}/assets.lua"
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets.lux" MODULE ec2.assets ENTRY EC2Assets SCOPE ENTITY
            SYMBOL_LEDGER "${CMAKE_CURRENT_BINARY_DIR}/symbols.json"
            ASSET_ID 12345678-1111-2222-3333-123456789abc ABILITY_TARGETS ec2_schemas)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/AssetSceneFixture.hpp" "${CMAKE_CURRENT_BINARY_DIR}/AssetSceneFixture.hpp" COPYONLY)
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_BINARY_DIR}/asset_scene.cpp" COPYONLY)
        add_executable(ec2_consumer "${CMAKE_CURRENT_BINARY_DIR}/asset_scene.cpp")
        target_link_libraries(ec2_consumer PRIVATE lux::engine::scene::scene_script_runtime
            lux::engine::scene::scene_script_assets_lua lux::engine::scene::scene_asset)
        add_dependencies(ec2_consumer ec2_package)
        add_test(NAME installed.ec2.scene COMMAND ec2_consumer
            "${CMAKE_CURRENT_BINARY_DIR}/assets.lux" "${CMAKE_CURRENT_BINARY_DIR}/assets.pak")
    else()
        message(FATAL_ERROR "Unknown EC2_MODE")
    endif()
endif()
if(MSVC)
    target_compile_options(ec2_consumer PRIVATE /UNDEBUG /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
else()
    target_compile_options(ec2_consumer PRIVATE -UNDEBUG)
endif()


find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell)
find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)
find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution)
find_package(lux-engine-function REQUIRED COMPONENTS ui_rendering render_features)
target_link_libraries(ec2_consumer PRIVATE lux::engine::editor::scene_ui lux::engine::editor::scene_execution
    lux::engine::editor::desktop_shell lux::engine::function::ui_rendering lux::engine::function::render_features)
