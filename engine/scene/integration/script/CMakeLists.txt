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
        lux::engine::scene::scene_presentation
        lux::engine::scene::scene_script_description
        lux::engine::process::process_execution
)
component_add_transitive_commands(
    scene_script_runtime
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-presentation REQUIRED COMPONENTS scene_presentation)"
    "find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
)

install_components(
    PROJECT_NAME lux-engine-scene-script-runtime
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_script_runtime
)
