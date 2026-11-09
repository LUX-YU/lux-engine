find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_SYSTEM_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_SYSTEM_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_SYSTEM_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/domain/scene/system/visibility.h
)

add_component(
    COMPONENT_NAME scene_system
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_system
    SOURCE_FILES  src/SceneSystem.cpp
)

component_include_directories(
    scene_system
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(scene_system PRIVATE LUX_ENGINE_SCENE_SYSTEM_LIBRARY)

target_link_libraries(
    scene_system
    PUBLIC
        lux::engine::core::object
        lux::engine::scene::scene_description
        lux::engine::system::domain_system_description
        lux::engine::core::serialization
        lux::engine::core::error
        lux::engine::core::core_execution
        lux::engine::ecs::ecs_core
        lux::cxx::compile_time
)
component_add_transitive_commands(
    scene_system
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-domain-system-description REQUIRED COMPONENTS domain_system_description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization error)"
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-system
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_system
)
