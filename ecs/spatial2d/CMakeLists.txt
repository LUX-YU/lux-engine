add_interface_component(
    COMPONENT_NAME spatial2d_components
    NAMESPACE      lux::engine::ecs
)

lux_classify_target(
    TARGET  spatial2d_components
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    spatial2d_components
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_link_libraries(
    spatial2d_components
    INTERFACE
        lux::engine::ecs::core
        lux::engine::core::meta
)

component_add_transitive_commands(
    spatial2d_components
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

add_subdirectory(streaming)

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(spatial2d_components_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET  spatial2d_components_meta
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    set_target_properties(
        spatial2d_components_meta
        PROPERTIES OUTPUT_NAME lux_engine_ecs_spatial2d_components_meta
    )
    target_link_libraries(
        spatial2d_components_meta
        PRIVATE spatial2d_components lux::engine::core::meta
    )
    target_compile_definitions(
        spatial2d_components_meta PRIVATE LUX_FUNCTION_LIBRARY)

    set(spatial2d_meta_args
        TARGET             spatial2d_components
        SIDECAR_TARGET     spatial2d_components_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/spatial2d/components/SpatialInterest2DComponent.hpp
    )
    if(LUX_BUILD_COMPONENT_LUA_META)
        add_library(spatial2d_components_lua_meta SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
        lux_classify_target(
            TARGET  spatial2d_components_lua_meta
            LAYER   ECS
            PRODUCT RUNTIME
            ROLE    INTEGRATION
        )
        set_target_properties(
            spatial2d_components_lua_meta
            PROPERTIES OUTPUT_NAME lux_engine_ecs_spatial2d_components_lua_meta
        )
        target_link_libraries(
            spatial2d_components_lua_meta
            PRIVATE
                spatial2d_components
                lux::engine::core::meta
                lux::engine::function::script_lua
        )
        target_compile_definitions(
            spatial2d_components_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)
        list(APPEND spatial2d_meta_args
            LUA_SIDECAR_TARGET spatial2d_components_lua_meta
            LUA_BINDING
        )
    endif()

    engine_enable_module_meta(${spatial2d_meta_args})
endif()
