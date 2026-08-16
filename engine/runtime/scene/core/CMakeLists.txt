## Domain-blind ECS/Schedule lifetime and LXSC/LXES scene assembly.

find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_RUNTIME_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_RUNTIME_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_RUNTIME_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/visibility.h
)

add_component(
    COMPONENT_NAME  runtime_scene_core
    NAMESPACE       lux::engine::runtime
    OUTPUT_NAME     lux_engine_runtime_scene_core
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/OwnerThreadCpuClock.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRuntime.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRuntimeClose.cpp
)

component_include_directories(
    runtime_scene_core
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    runtime_scene_core
    PRIVATE LUX_RUNTIME_SCENE_LIBRARY
)

target_link_libraries(
    runtime_scene_core
    PUBLIC
        lux::engine::runtime::runtime_assets
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_extension_loader
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::core
        lux::engine::resource::asset_core
        lux::engine::resource::entity_scene
        lux::cxx::memory
        lux::engine::core::events
        lux::engine::core::log
        EnTT::EnTT
)

component_add_transitive_commands(
    runtime_scene_core
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core entity_scene)"
    "find_package(lux-engine-core REQUIRED COMPONENTS events log)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets runtime_entity_scene runtime_execution runtime_extension_loader runtime_scene_contributions)"
)

lux_classify_target(
    TARGET  runtime_scene_core
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRuntimeClose.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor"
    )
endif()
