add_component(
    COMPONENT_NAME scene_script_assets_lua
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_script_assets_lua
    STATIC
    SOURCE_FILES src/AssetAbilityLua.cpp
)
component_include_directories(scene_script_assets_lua
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(scene_script_assets_lua PUBLIC scene_script_assets scene_script_skeleton simulation_script_lua)
get_target_property(_asset_generated scene_script_assets LUX_SCRIPT_ABILITY_GENERATED_DIR)
target_include_directories(scene_script_assets_lua PRIVATE ${_asset_generated})
get_target_property(_skeleton_generated scene_script_skeleton LUX_SCRIPT_ABILITY_GENERATED_DIR)
target_include_directories(scene_script_assets_lua PRIVATE ${_skeleton_generated})
add_dependencies(scene_script_assets_lua scene_script_assets_script_abilities_generate
    scene_script_skeleton_script_abilities_generate)
component_add_transitive_commands(scene_script_assets_lua
    "find_package(lux-engine-scene-script-assets REQUIRED COMPONENTS scene_script_assets)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_lua)"
    "find_package(lux-engine-scene-script-skeleton REQUIRED COMPONENTS scene_script_skeleton)"
)
lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-script-assets-lua VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene COMPONENTS scene_script_assets_lua
)
if(BUILD_TESTING)
    add_executable(scene_script_asset_values_test test/values.cpp)
    target_link_libraries(scene_script_asset_values_test PRIVATE scene_script_assets_lua LuxLua55::Runtime)
    if(MSVC)
        target_compile_options(scene_script_asset_values_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_asset_values_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_asset_values COMMAND scene_script_asset_values_test)
    set_tests_properties(scene.script_asset_values PROPERTIES TIMEOUT 30 LABELS "native;EC2")
    add_executable(scene_script_asset_runtime_test test/runtime.cpp)
    target_link_libraries(scene_script_asset_runtime_test PRIVATE scene_script_assets_lua simulation_script)
    get_target_property(_delay_generated simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
    target_include_directories(scene_script_asset_runtime_test PRIVATE ${_delay_generated})
    add_dependencies(scene_script_asset_runtime_test simulation_script_script_abilities_generate)
    if(MSVC)
        target_compile_options(scene_script_asset_runtime_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_asset_runtime_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_asset_lua COMMAND scene_script_asset_runtime_test ${CMAKE_CURRENT_SOURCE_DIR}/test/assets.lua)
    set_tests_properties(scene.script_asset_lua PROPERTIES TIMEOUT 45 LABELS "native;EC2")
endif()
