find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_TERRAIN_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_TERRAIN_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_TERRAIN_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/terrain/visibility.h
)

add_component(
    COMPONENT_NAME terrain_components
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_terrain_components
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TerrainTileCodec.cpp
)

lux_classify_target(
    TARGET  terrain_components
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    terrain_components
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_link_libraries(
    terrain_components
    PUBLIC
        lux::cxx::compile_time
        lux::engine::ecs::core
        lux::engine::core::math
        lux::engine::core::meta
        lux::engine::ecs::scene_format
    PRIVATE
        lux::engine::core::serialization
)

target_compile_definitions(
    terrain_components PRIVATE LUX_ENGINE_ECS_TERRAIN_LIBRARY)

component_add_transitive_commands(
    terrain_components
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math meta serialization)"
)

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(terrain_components_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET  terrain_components_meta
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    set_target_properties(
        terrain_components_meta
        PROPERTIES OUTPUT_NAME lux_engine_ecs_terrain_components_meta
    )
    target_link_libraries(
        terrain_components_meta
        PRIVATE
            terrain_components
            lux::engine::core::meta
    )
    target_compile_definitions(
        terrain_components_meta PRIVATE LUX_FUNCTION_LIBRARY)

    set(_terrain_lua_meta_args "")
    if(LUX_BUILD_COMPONENT_LUA_META)
        add_library(terrain_components_lua_meta SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
        lux_classify_target(
            TARGET  terrain_components_lua_meta
            LAYER   ECS
            PRODUCT RUNTIME
            ROLE    INTEGRATION
        )
        set_target_properties(
            terrain_components_lua_meta
            PROPERTIES OUTPUT_NAME lux_engine_ecs_terrain_components_lua_meta
        )
        target_link_libraries(
            terrain_components_lua_meta
            PRIVATE
                terrain_components
                lux::engine::core::meta
                lux::engine::function::script_lua
        )
        target_compile_definitions(
            terrain_components_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)
        list(APPEND _terrain_lua_meta_args
            LUA_SIDECAR_TARGET terrain_components_lua_meta
            LUA_BINDING)
    endif()

    engine_enable_module_meta(
        TARGET             terrain_components
        SIDECAR_TARGET     terrain_components_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/terrain/components/TerrainTileComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/terrain/components/TerrainLodNodeComponent.hpp
        ${_terrain_lua_meta_args}
    )
endif()
