## engine::host::game_application — platform-neutral playable application.
##
## This target owns the shared Runtime composition and lifecycle used by every
## game binary.  Desktop/Android entry points only adapt platform windows,
## input events, packaged files and the process log outlet.

find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_GAME_APPLICATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_GAME_APPLICATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_GAME_APPLICATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/hosts/game_application/visibility.h
)

add_component(
    COMPONENT_NAME game_application
    NAMESPACE      lux::engine::host
    OUTPUT_NAME    lux_engine_host_game_application
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/GameApplication.cpp
)

lux_classify_target(
    TARGET  game_application
    LAYER   HOST
    PRODUCT PLAYER
    ROLE    COMPOSITION
)

component_include_directories(
    game_application
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    game_application
    PRIVATE
        LUX_GAME_APPLICATION_LIBRARY
        LUX_SCRIPT_HAS_LUA=$<BOOL:${LUX_SCRIPT_HAS_LUA}>
)

target_link_libraries(
    game_application
    PUBLIC
        lux::engine::runtime::runtime_render_scene
        lux::engine::runtime::runtime_render_backend_host
        lux::engine::runtime::runtime_frame
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_assets
        lux::engine::runtime::runtime_scene_script
        lux::engine::ecs::animation_3d
        lux::engine::ecs::physics
        lux::engine::ecs::presentation_2d
        lux::engine::ecs::transform
        lux::engine::runtime::runtime_scene_navigation3d_composition
        lux::engine::runtime::runtime_navigation3d_prepare
        lux::engine::runtime::runtime_physics3d_prepare
        lux::engine::runtime::runtime_scene_physics3d_composition
        lux::engine::runtime::runtime_scene_presentation3d_composition
        lux::engine::runtime::runtime_scene_spatial3d_composition
        lux::engine::runtime::runtime_scene_tilemap_composition
        lux::engine::runtime::runtime_tilemap_prepare
        lux::engine::function::input
        lux::engine::function::navigation
        lux::engine::function::render_client
        lux::engine::resource::asset
        lux::engine::ecs::render_core
        lux::engine::core::log
        lux::engine::core::events
        EnTT::EnTT
    PRIVATE
        lux::engine::runtime::runtime_logging
        lux::engine::ecs::scripting_native
)
if(LUX_SCRIPT_HAS_LUA)
    target_link_libraries(
        game_application PRIVATE lux::engine::ecs::scripting_lua
    )
endif()

component_add_transitive_commands(
    game_application
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS animation_3d physics presentation_2d transform)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution runtime_assets runtime_logging runtime_frame runtime_scene_script runtime_tilemap_prepare runtime_scene_tilemap_composition runtime_navigation3d_prepare runtime_scene_navigation3d_composition runtime_physics3d_prepare runtime_scene_physics3d_composition runtime_scene_presentation3d_composition runtime_scene_spatial3d_composition runtime_render_scene runtime_render_backend_host)"
    "find_package(lux-engine-function REQUIRED COMPONENTS input navigation render_client)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core render_core scripting_native)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log events)"
)
if(LUX_SCRIPT_HAS_LUA)
    component_add_transitive_commands(
        game_application
        "find_package(lux-engine-ecs REQUIRED COMPONENTS scripting_lua)"
    )
endif()
