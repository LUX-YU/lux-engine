# ECS animation behavior is dimensional. 2D flipbooks and 3D skeletal
# animation share sampling math, but neither target nor schema closure crosses
# the dimensional boundary.

set(LUX_ECS_ANIMATION_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(lux_configure_animation_target target)
    lux_classify_target(
        TARGET  ${target}
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    DOMAIN
    )
    component_include_directories(
        ${target}
        BUILD_TIME_EXPORT
            ${LUX_ECS_ANIMATION_INCLUDE_DIR}
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
    )
    target_compile_definitions(${target} PRIVATE LUX_FUNCTION_LIBRARY)
endfunction()

add_component(
    COMPONENT_NAME animation_2d
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_animation_2d
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FlipbookAnimationSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Flipbook2DResolver.cpp
)
lux_configure_animation_target(animation_2d)
target_link_libraries(
    animation_2d
    PUBLIC
        lux::engine::ecs::render_2d
        lux::engine::function::animation
        lux::engine::resource::asset
)
component_add_transitive_commands(
    animation_2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_2d)"
    "find_package(lux-engine-function REQUIRED COMPONENTS animation)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)

add_component(
    COMPONENT_NAME animation_3d
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_animation_3d
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AnimationSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SkeletalAnimationResolver.cpp
)
lux_configure_animation_target(animation_3d)
target_link_libraries(
    animation_3d
    PUBLIC
        lux::engine::ecs::render_3d
        lux::engine::function::animation
        lux::engine::resource::asset
)
component_add_transitive_commands(
    animation_3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_3d)"
    "find_package(lux-engine-function REQUIRED COMPONENTS animation)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)

# FlipbookAnimationComponent belongs to the 2D animation target. 3D animation
# writes render-owned Animator components, whose schema sidecar is render 3D.
if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(animation_2d_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET  animation_2d_meta
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    set_target_properties(animation_2d_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_animation_2d_meta)
    target_link_libraries(
        animation_2d_meta
        PRIVATE animation_2d lux::engine::core::meta
    )
    target_compile_definitions(
        animation_2d_meta PRIVATE LUX_FUNCTION_LIBRARY)

    add_library(animation_2d_lua_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET  animation_2d_lua_meta
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    set_target_properties(animation_2d_lua_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_animation_2d_lua_meta)
    target_link_libraries(
        animation_2d_lua_meta
        PRIVATE
            animation_2d
            lux::engine::core::meta
            lux::engine::function::script_lua
    )
    target_compile_definitions(
        animation_2d_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET             animation_2d
        SIDECAR_TARGET     animation_2d_meta
        LUA_SIDECAR_TARGET animation_2d_lua_meta
        TARGET_FILES
            ${LUX_ECS_ANIMATION_INCLUDE_DIR}/lux/engine/ecs/animation/components/FlipbookAnimationComponent.hpp
        LUA_BINDING
    )
endif()
