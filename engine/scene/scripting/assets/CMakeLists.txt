generate_visibility_header(
    ENABLE_MACRO_NAME LUX_SCENE_SCRIPT_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME LUX_SCENE_SCRIPT_ASSETS_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_SCENE_SCRIPT_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/script_assets/visibility.h
)
add_component(
    COMPONENT_NAME scene_script_assets
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_script_assets
    SOURCE_FILES src/ScriptAssetAccess.cpp
)
component_include_directories(
    scene_script_assets
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_script_assets PRIVATE LUX_SCENE_SCRIPT_ASSETS_LIBRARY)
target_link_libraries(scene_script_assets PUBLIC
    lux::engine::process::process_asset_loading
    lux::engine::simulation::simulation_scripting_core
)
component_add_transitive_commands(scene_script_assets
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting_core)"
)
lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-script-assets
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_script_assets
)

if(BUILD_TESTING)
    add_executable(scene_script_assets_test test/access.cpp test/lifecycle.cpp)
    target_link_libraries(scene_script_assets_test PRIVATE scene_script_assets simulation_script_cpp_static)
    if(MSVC)
        target_compile_options(scene_script_assets_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_assets_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_asset_scopes COMMAND scene_script_assets_test)
    set_tests_properties(scene.script_asset_scopes PROPERTIES TIMEOUT 45 LABELS "native;EC2")
endif()
