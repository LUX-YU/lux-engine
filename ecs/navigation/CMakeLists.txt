set(LUX_ECS_NAVIGATION3D_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Navigation3DSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NavigationQueryService.cpp
)

add_component(
    COMPONENT_NAME navigation3d
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_navigation3d
    SOURCE_FILES   ${LUX_ECS_NAVIGATION3D_SRCS}
)

lux_classify_target(
    TARGET  navigation3d
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    navigation3d
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    navigation3d
    PRIVATE LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    navigation3d
    PUBLIC
        lux::engine::ecs::core
        lux::engine::function::navigation
        lux::engine::function::navigation_detour3d
        lux::engine::resource::entity_scene
        lux::engine::core::meta
)

component_add_transitive_commands(
    navigation3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation navigation_detour3d)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(navigation3d_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET  navigation3d_meta
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    set_target_properties(navigation3d_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_navigation3d_meta)
    target_link_libraries(
        navigation3d_meta
        PRIVATE
            navigation3d
            lux::engine::core::meta
    )
    set(_navigation3d_lua_meta_args "")
    if(LUX_BUILD_COMPONENT_LUA_META)
        target_link_libraries(navigation3d_meta
            PRIVATE lux::engine::function::script_lua)
        list(APPEND _navigation3d_lua_meta_args LUA_BINDING)
    endif()
    target_compile_definitions(
        navigation3d_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         navigation3d
        SIDECAR_TARGET navigation3d_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/navigation/components/NavigationRegion3DComponent.hpp
        ${_navigation3d_lua_meta_args}
    )
endif()

add_subdirectory(test)
