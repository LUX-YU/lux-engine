find_package(lux-cxx REQUIRED COMPONENTS algorithm container)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_TILEMAP_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_TILEMAP_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_TILEMAP_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/tilemap/visibility.h
)

add_component(
    COMPONENT_NAME tilemap
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_tilemap
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapSystem.cpp
)

lux_classify_target(
    TARGET  tilemap
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    tilemap
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)

target_compile_definitions(
    tilemap
    PRIVATE LUX_ENGINE_ECS_TILEMAP_LIBRARY
)

target_link_libraries(
    tilemap
    PUBLIC
        lux::engine::ecs::core
        lux::cxx::algorithm
        lux::engine::resource::asset_core
        lux::engine::resource::description
        lux::engine::ecs::scene_format
        lux::engine::resource::spatial
        lux::engine::core::meta
        lux::cxx::container
)

component_add_transitive_commands(
    tilemap
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core scene_format)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm container)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core description spatial)"
)

add_subdirectory(test)

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(tilemap_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET tilemap_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(tilemap_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_tilemap_meta)
    target_link_libraries(tilemap_meta
        PRIVATE tilemap lux::engine::core::meta)
    target_compile_definitions(tilemap_meta
        PRIVATE LUX_FUNCTION_LIBRARY)

    add_library(tilemap_lua_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET tilemap_lua_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(tilemap_lua_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_tilemap_lua_meta)
    target_link_libraries(tilemap_lua_meta
        PRIVATE
            tilemap
            lux::engine::core::meta
            lux::engine::function::script_lua)
    target_compile_definitions(tilemap_lua_meta
        PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET tilemap
        SIDECAR_TARGET tilemap_meta
        LUA_SIDECAR_TARGET tilemap_lua_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/tilemap/components/TilemapComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/tilemap/components/TileChunk2DComponent.hpp
        LUA_BINDING
    )
endif()
