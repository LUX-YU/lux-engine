generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/script_description/visibility.h
)
add_component(
    COMPONENT_NAME scene_script_description
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_script_description
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptRuntimeAssembly.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescriptionCodec.cpp
)
component_include_directories(scene_script_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_script_description PRIVATE LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_LIBRARY)
target_link_libraries(scene_script_description PUBLIC
    lux::engine::simulation::simulation_script
    lux::engine::world::identity
)
component_add_transitive_commands(scene_script_description
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script)"
    "find_package(lux-engine-world-identity REQUIRED COMPONENTS world_identity)"
)

install_components(
    PROJECT_NAME lux-engine-scene-script-description
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_script_description
)
