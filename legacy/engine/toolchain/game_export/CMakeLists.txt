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
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GameExporter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/GameBuildClosure.cpp
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

target_include_directories(
    toolchain_game_export
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
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
        lux::engine::resource::asset
        lux::game::deployment
    PRIVATE
        lux::engine::toolchain::toolchain_spatial3d_scene_cook
        lux::engine::ecs::core
        lux::engine::core::meta
        tomlplusplus::tomlplusplus
)

component_add_transitive_commands(
    toolchain_game_export
    "find_package(lux-game REQUIRED COMPONENTS deployment)"
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
