generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_SCRIPT_RUNTIME_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_SCRIPT_RUNTIME_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_SCRIPT_RUNTIME_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/script_runtime/visibility.h
)

add_component(
    COMPONENT_NAME scene_script_runtime
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_script_runtime
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptRuntimeSystem.cpp
)

component_include_directories(
    scene_script_runtime
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(scene_script_runtime PRIVATE LUX_ENGINE_SCENE_SCRIPT_RUNTIME_LIBRARY)
target_link_libraries(
    scene_script_runtime
    PUBLIC
        lux::engine::scene::scene_composition
        lux::cxx::concurrent
        lux::engine::scene::scene_script_description
        lux::engine::process::process_execution
        lux::engine::scene::scene_script_assets
)

component_add_transitive_commands(
    scene_script_runtime
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-cxx REQUIRED COMPONENTS concurrent)"
    "find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-scene-script-assets REQUIRED COMPONENTS scene_script_assets)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-script-runtime
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_script_runtime
)

if(BUILD_TESTING)
    add_executable(scene_script_delay_lifetime_test test/real_delay_lifetime.cpp)
    target_link_libraries(scene_script_delay_lifetime_test PRIVATE scene_script_runtime)
    if(MSVC)
        target_compile_options(scene_script_delay_lifetime_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_delay_lifetime_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME scene.script_delay_lifetime COMMAND scene_script_delay_lifetime_test)
    set_tests_properties(scene.script_delay_lifetime PROPERTIES TIMEOUT 30 LABELS "native;LR05")
endif()

if(BUILD_TESTING AND TARGET simulation_script_lua)
    include(${PROJECT_SOURCE_DIR}/engine/toolchain/lua/cmake/lux_package_lua_script.cmake)
    lux_package_lua_script(
        NAME ec2_script_asset_package
        SOURCE ${PROJECT_SOURCE_DIR}/engine/scene/scripting/assets_lua/test/assets.lua
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ec2-assets.lux
        MODULE ec2.assets ENTRY EC2Assets SCOPE ENTITY
        SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/test/symbols.json
        ASSET_ID 12345678-1111-2222-3333-123456789abc
        ABILITY_TARGETS scene_script_assets scene_script_skeleton simulation_script
    )

    add_executable(scene_script_asset_scene_test test/asset_scene.cpp)
    target_link_libraries(scene_script_asset_scene_test PRIVATE scene_script_runtime scene_script_assets_lua scene_asset)
    get_target_property(_delay_generated simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
    target_include_directories(scene_script_asset_scene_test PRIVATE ${_delay_generated})
    add_dependencies(scene_script_asset_scene_test ec2_script_asset_package)
    if(MSVC)
        target_compile_options(scene_script_asset_scene_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(scene_script_asset_scene_test PRIVATE -UNDEBUG)
    endif()
    add_test(
        NAME scene.script_asset_scene COMMAND scene_script_asset_scene_test
        ${CMAKE_CURRENT_BINARY_DIR}/ec2-assets.lux ${CMAKE_CURRENT_BINARY_DIR}/ec2-assets.pak
    )

    set_tests_properties(scene.script_asset_scene PROPERTIES TIMEOUT 45 LABELS "native;EC2")
endif()
