generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/game_export/visibility.h
)

find_package(tomlplusplus CONFIG REQUIRED)

add_component(
    COMPONENT_NAME toolchain_game_export
    STATIC
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_game_export
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/GameExporter.cpp
)

lux_classify_target(
    TARGET  toolchain_game_export
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    COMPOSITION
)

component_include_directories(
    toolchain_game_export
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    toolchain_game_export
    PUBLIC LUX_ENGINE_TOOLCHAIN_GAME_EXPORT_DLL_DISABLE
)

target_link_libraries(
    toolchain_game_export
    PUBLIC
        lux::engine::authoring::project
        lux::engine::toolchain::toolchain_asset_cook
        lux::engine::toolchain::toolchain_texture_importer
        lux::engine::resource::asset_codecs
        lux::engine::resource::deployment
    PRIVATE
        lux::engine::toolchain::toolchain_spatial3d_scene_cook
        lux::engine::ecs::core
        lux::engine::core::meta
        tomlplusplus::tomlplusplus
)

# GameExporter executables are Toolchain composition roots: the Spatial3D
# adapter must materialize the exact built-in component schemas before it can
# transcode an Authoring LXWA document into LXES columns. Keep raw reflection
# sidecars on executable closures (they are not installed SDK components), so
# their registrars run before meta_module_init() without leaking non-exported
# targets through the static library's install interface.
set(_lux_game_export_component_meta
    ecs_meta
    render_components_meta
    render_components_3d_meta
    spatial3d_components_meta
    terrain_components_meta
    physics3d_meta
    navigation3d_meta
)
foreach(_meta_target IN LISTS _lux_game_export_component_meta)
    if(NOT TARGET ${_meta_target})
        message(FATAL_ERROR
            "toolchain_game_export requires component meta sidecar '${_meta_target}'")
    endif()
endforeach()
set_property(
    GLOBAL PROPERTY
        LUX_GAME_EXPORT_COMPONENT_META_TARGETS
        "${_lux_game_export_component_meta}"
)

function(lux_link_game_export_component_meta target)
    # CMake functions use the caller's dynamic variable scope.  This helper is
    # also called by extension tests configured from sibling directories, where
    # the private `_lux_game_export_component_meta` directory variable is not
    # visible.  Resolve the stable target list here so every composition root
    # receives the same sidecars and their INTERFACE /INCLUDE anchors.  A
    # global property keeps the target list single-sourced without relying on
    # directory-variable visibility.
    get_property(
        _component_meta_targets
        GLOBAL PROPERTY LUX_GAME_EXPORT_COMPONENT_META_TARGETS
    )
    if(NOT _component_meta_targets)
        message(FATAL_ERROR
            "game export component meta target list is unavailable")
    endif()
    target_link_libraries(
        ${target}
        PRIVATE
            ${_component_meta_targets}
    )
endfunction()

add_executable(
    lux_game_exporter
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp
)

lux_classify_target(
    TARGET  lux_game_exporter
    LAYER   TOOLCHAIN
    PRODUCT TOOLCHAIN
    ROLE    COMPOSITION
)

target_link_libraries(
    lux_game_exporter
    PRIVATE lux::engine::toolchain::toolchain_game_export
)
lux_link_game_export_component_meta(lux_game_exporter)

set_target_properties(
    lux_game_exporter
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(WIN32)
    add_custom_command(
        TARGET lux_game_exporter
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:lux_game_exporter>
                $<TARGET_FILE_DIR:lux_game_exporter>
        COMMAND_EXPAND_LISTS
        COMMENT "Stage lux_game_exporter runtime dependencies"
    )
endif()

install(
    TARGETS lux_game_exporter
    RUNTIME DESTINATION bin
    COMPONENT lux_toolchain
)

if(TARGET lux_player AND TARGET render_features)
    add_executable(
        game_export_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test/game_export_smoke.cpp
    )
    lux_classify_target(
        TARGET  game_export_smoke
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    target_link_libraries(
        game_export_smoke
        PRIVATE
            lux::engine::toolchain::toolchain_game_export
            lux::engine::authoring::authoring_world
            lux::engine::resource::classic_mesh_content
    )
    lux_link_game_export_component_meta(game_export_smoke)
    target_compile_definitions(
        game_export_smoke
        PRIVATE
            "LUX_GAME_EXPORT_TEST_ASSET=\"${CMAKE_BINARY_DIR}/assets/shaders/canvas2d/group_composite.frag.luxasset\""
            "LUX_GAME_EXPORT_TEST_RUNTIME_ROOT=\"${CMAKE_BINARY_DIR}\""
    )
    add_dependencies(game_export_smoke lux_player render_features)
endif()
