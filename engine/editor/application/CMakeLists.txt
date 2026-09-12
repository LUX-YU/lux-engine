add_library(
    editor_application
    STATIC
    src/EditorApplication.cpp
    src/UiVulkanPresentation.cpp
)
target_include_directories(
    editor_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(editor_application PRIVATE
    LUX_EDITOR_PACKED_CONTENT=$<BOOL:${LUX_BUILD_PACKED_RENDER_CONTENT}>)
target_link_libraries(
    editor_application
    PUBLIC
        lux::engine::editor::editor_context
        lux::engine::function::ui_vulkan_backend
        lux::engine::process::asset_loading
        lux::engine::function::render_vulkan
        lux::engine::platform::window
        lux::engine::scene::scene_render
        lux::engine::editor::editor_scene
        lux::engine::function::render_features
)

add_executable(
    lux_editor
    app/main.cpp
)
target_include_directories(lux_editor PRIVATE app)

target_link_libraries(
    lux_editor
    PRIVATE
        editor_application
        lux::engine::editor::editor_inspector
        lux::engine::scene::scene_meta
)

set_target_properties(lux_editor PROPERTIES OUTPUT_NAME "lux_editor")
set(LUX_EDITOR_SEED_PAK "" CACHE FILEPATH "SV-1 asset package produced by lux_scene_seed")
if(LUX_EDITOR_SEED_PAK)
    if(NOT EXISTS "${LUX_EDITOR_SEED_PAK}")
        message(FATAL_ERROR "LUX_EDITOR_SEED_PAK does not exist: ${LUX_EDITOR_SEED_PAK}")
    endif()
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/share/lux-engine/editor)
    configure_file(${LUX_EDITOR_SEED_PAK} ${CMAKE_BINARY_DIR}/share/lux-engine/editor/sv1.luxpak COPYONLY)
    install(FILES ${LUX_EDITOR_SEED_PAK} DESTINATION share/lux-engine/editor RENAME sv1.luxpak)
endif()

install(
    TARGETS lux_editor
    RUNTIME DESTINATION bin
    COMPONENT lux_editor
)
if(WIN32)
    install(IMPORTED_RUNTIME_ARTIFACTS imgui::core RUNTIME DESTINATION bin COMPONENT lux_editor)
endif()

# Independent new composition while the ER-1 deletion gates are being verified.
add_library(editor_application_candidate STATIC src/Application.cpp)
target_include_directories(editor_application_candidate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)
target_link_libraries(editor_application_candidate PUBLIC lux::engine::editor::editor_scene_ui
    lux::engine::editor::editor_tooling
    PRIVATE lux::engine::platform::window)
