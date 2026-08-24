## engine/hosts/launcher — slim Project Launcher composition root.
##
## Independent process that pops up a project picker (Recent / New / Open),
## then `CreateProcessW`'s `lux_editor.exe --project <path>` and exits. The
## editor's full asset / shader / EditorBuiltins init happens only inside
## the spawned editor process, not here.
##
## Dependency budget:
##   - lux::engine::function::ui     (UISystem + UIRenderFrameSession +
##                                    UIRenderServer = same ImGui-over-Vulkan
##                                    path the editor already uses; one
##                                    render-thread minus all scene state)
##   - lux::engine::authoring::project (Project / ProjectManifest / RecentProjects)
##   - lux::engine::platform::window (GlfwRuntime + LuxWindow)
##
## No gameplay, no asset, no resource, no flowforge — those are the heavy
## bits the editor pulls in. Launcher reuses the ImGui rendering stack but
## paints zero 3D scenes, so the heavy editor-side init never runs here.

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_LAUNCHER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_LAUNCHER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_LAUNCHER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/hosts/launcher/visibility.h
)

set(LUX_LAUNCHER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LauncherApp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectPickerPanel.cpp
)

add_component(
    COMPONENT_NAME  launcher_host
    STATIC
    NAMESPACE       lux::engine
    OUTPUT_NAME     lux_engine_host_launcher
    SOURCE_FILES    ${LUX_LAUNCHER_SRCS}
)

lux_classify_target(
    TARGET  launcher_host
    LAYER   HOST
    PRODUCT EDITOR
    ROLE    COMPOSITION
)

component_include_directories(
    launcher_host
    BUILD_TIME_EXPORT   ${CMAKE_CURRENT_SOURCE_DIR}/include
                        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME        include
)

target_compile_definitions(
    launcher_host
    PRIVATE
    LUX_ENGINE_LAUNCHER_LIBRARY
    PUBLIC
    LUX_ENGINE_LAUNCHER_DLL_DISABLE
)

target_link_libraries(
    launcher_host
    PUBLIC
    lux::engine::authoring::project
    lux::engine::function::ui_vulkan
    lux::engine::platform::window
    lux::engine::core::log             # 校验层文本 + 渲染错误的落点(§7.1)
    lux::engine::core::events          # main-thread committed facts
    lux::engine::runtime::runtime_render_scene         # installRenderErrorLogging(共享诊断出口)
    lux::engine::runtime::runtime_render_backend_host  # UI render thread/channel owner
    lux::engine::runtime::runtime_frame # shared frame lifecycle coordinator
    # editor: linked for `lux::editor::writeDemoScene` (the launcher's
    # "New Demo Project" template builder). Pulls in the editor's
    # gameplay-component + Scene::save plumbing; unused symbols
    # (LuxEditor, panels, etc.) are dead-code-stripped at link time
    # for the launcher exe.
    lux::engine::editor::editor
)

# ──────────────────────────────────────────────────────────────────────────
# Launcher executable — lux_launcher
# ──────────────────────────────────────────────────────────────────────────
add_executable(
    lux_launcher
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp
)

lux_classify_target(
    TARGET  lux_launcher
    LAYER   HOST
    PRODUCT EDITOR
    ROLE    COMPOSITION
)

target_link_libraries(
    lux_launcher
    PRIVATE
        launcher_host
)

# Reflection sidecar — same `/INCLUDE:` linker trick the editor uses:
# linking gameplay_meta forces OS load of its DLL at process start,
# firing MetaModuleRegistrar ctors so `lux::meta::meta_module_init()`
# can drain them. The launcher needs the gameplay component types in
# the reflection registry so `lux::editor::writeDemoScene` →
# `Scene::save` produces a non-empty scene file.
if(TARGET ecs_meta)
    target_link_libraries(lux_launcher PRIVATE ecs_meta)
endif()
foreach(_sc
        render_components_meta
        render_components_2d_meta
        render_components_3d_meta
        spatial2d_components_meta
        spatial3d_components_meta
        pixel_meta
        animation_2d_meta)
    if(TARGET ${_sc})
        target_link_libraries(lux_launcher PRIVATE ${_sc})
    endif()
endforeach()

set_target_properties(
    lux_launcher PROPERTIES
    OUTPUT_NAME "lux_launcher"
)

install(TARGETS lux_launcher
        RUNTIME DESTINATION bin
        COMPONENT lux_editor)
