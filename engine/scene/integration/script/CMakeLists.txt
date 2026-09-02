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
        lux::engine::simulation::simulation_script
)
component_add_transitive_commands(
    scene_script_runtime
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
)
lux_classify_target(
    TARGET  scene_script_runtime
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(scene_script_runtime_test test/scene_script_runtime_test.cpp)
target_link_libraries(
    scene_script_runtime_test
    PRIVATE
        lux::engine::scene::scene_script_runtime
        lux::engine::core::meta
        lux::engine::world::description
)
if(MSVC)
    target_compile_options(scene_script_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(scene_script_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  scene_script_runtime_test
    LAYER   SCENE
    PRODUCT TEST
    ROLE    INTEGRATION
)
add_test(NAME scene_script_runtime_test COMMAND scene_script_runtime_test)

install_components(
    PROJECT_NAME lux-engine-scene-script-runtime
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_script_runtime
)
