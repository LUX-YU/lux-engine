## engine/hosts/player — the desktop Player composition root.
##
## GameHost is now only the GLFW/native-window adapter around the installed
## game_application lifecycle. lux_player is the thin argument-parsing front
## end and remains a reference host, not the final game binary architecture.
##
## Editor-independent BY CONSTRUCTION: builds under the runtime preset
## (PLAYER/DEVELOPER/EDITOR profile). Desktop-only — the Android front end is
## platforms/android's NativeActivity shell (shares SceneRuntime, not the
## GLFW window layer); engine/CMakeLists gates this out under ANDROID.

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_GAME_HOST_LIBRARY
    PUBLIC_MACRO_NAME       LUX_GAME_HOST_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_GAME_HOST_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/hosts/player/visibility.h
)

add_component(
    COMPONENT_NAME  player_host
    NAMESPACE       lux::engine::host
    OUTPUT_NAME     lux_engine_host_player
    SOURCE_FILES    ${CMAKE_CURRENT_SOURCE_DIR}/src/GameHost.cpp
)

lux_classify_target(
    TARGET  player_host
    LAYER   HOST
    PRODUCT PLAYER
    ROLE    COMPOSITION
)

component_include_directories(
    player_host
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    player_host
    PRIVATE
    LUX_GAME_HOST_LIBRARY
)

target_link_libraries(
    player_host
    PUBLIC
        lux::engine::host::game_application
        lux::engine::platform::window
        lux::engine::function::input
        lux::engine::core::log
)

component_add_transitive_commands(
    player_host
    "find_package(lux-engine-host REQUIRED COMPONENTS game_application)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS window)"
    "find_package(lux-engine-function REQUIRED COMPONENTS input)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log)"
)

# ── lux_player — the player executable ──────────────────────────────────────
add_executable(lux_player ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp)
lux_classify_target(
    TARGET  lux_player
    LAYER   HOST
    PRODUCT PLAYER
    ROLE    COMPOSITION
)
target_link_libraries(
    lux_player
    PRIVATE
        player_host
        lux::engine::resource::deployment
)

# Reflection sidecars: each carries `/INCLUDE:<func>_meta` INTERFACE link
# flags, so linking them puts the sidecar DLL into the exe's import table and
# the OS load fires every MetaModuleRegistrar static at process start. WITHOUT
# these the component catalogue is empty and Scene::load produces entities
# that own nothing — file loads "succeed", entity counts match, nothing
# renders (the silent-failure class the drainPending work documented). Same
# quartet lux_editor links.
foreach(_sidecar
        ecs_meta
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial2d_components_meta
        spatial3d_components_meta
        tilemap_meta
        pixel_meta
        animation_2d_meta
        physics2d_meta
        physics3d_meta
        navigation3d_meta
        script_meta)
    if(TARGET ${_sidecar})
        target_link_libraries(lux_player PRIVATE ${_sidecar})
    endif()
endforeach()

set_target_properties(lux_player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(_lux_player_runtime_inventory
    ${CMAKE_BINARY_DIR}/bin/lux_player.runtime-files)

add_custom_command(
    TARGET lux_player
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:lux_player>
            $<TARGET_FILE_DIR:lux_player>
    COMMAND ${CMAKE_COMMAND}
            "-DOUTPUT=${_lux_player_runtime_inventory}"
            "-DBINARY=$<TARGET_FILE:lux_player>"
            "-DSUBJECT=lux_player"
            -P ${PROJECT_SOURCE_DIR}/cmake/WriteRuntimeDependencyInventory.cmake
    COMMAND_EXPAND_LISTS
    COMMENT "Stage and inventory lux_player's runtime dependency closure"
)

install(
    TARGETS lux_player
    RUNTIME DESTINATION bin
    COMPONENT lux_player
)
install(
    FILES ${_lux_player_runtime_inventory}
    DESTINATION bin
    COMPONENT lux_player
)
