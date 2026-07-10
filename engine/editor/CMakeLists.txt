## engine::editor — the lux-engine editor application.
##
## Holds the engine-coupled editor panels (node graph editor, lua console,
## asset browser) plus the editor shell. It links the reusable UI framework
## (function::ui) and the engine subsystems those panels drive (flowforge,
## script, asset). The FlowForge MLIR compiler is linked only when
## LUX_ENABLE_FLOWFORGE_MLIR is ON.
##
## This component is gated by LUX_BUILD_EDITOR (see engine/CMakeLists.txt) and
## is never linked into a shipped game runtime.

find_package(imgui       REQUIRED COMPONENTS glfw3 vulkan)
find_package(node_editor REQUIRED COMPONENTS node_editor)
find_package(lux-cxx     REQUIRED COMPONENTS container reflection_runtime event)

# std::format compatibility detection — LuaConsole formats console output
# through FormatCompat.h, generated from FormatCompat.h.in.
include(${CMAKE_SOURCE_DIR}/cmake/CheckFormatSupport.cmake)
check_format_support()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/app/FormatCompat.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lux/engine/editor/app/FormatCompat.h
)

# Asset path header — EditorScene loads the built-in skybox texture
# (modules/function/render/assets/textures/blue_nebulae_1.luxasset) so the
# SkyboxPass node has a real texture to render against. The render
# component already registers its runtime assets, so we just generate a
# header pointing at that root.
include_component_cmake_scripts(lux::engine::resource::asset)
component_generate_runtime_asset_path_header(
    COMPONENT     render
    OUTPUT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/include/lux/engine/editor/content/RuntimeAssetPath.hpp
    SYMBOL        editor_runtime_asset_path
)

# ──────────────────────────────────────────────────────────────────────────
# Engine content path — bakes cube/plane/whitePbr .luxasset files into
# ${CMAKE_BINARY_DIR}/engine_content/ via the lux_content_baker tool;
# EditorBuiltins loads from this directory at runtime, falling back to
# in-memory generation only if the files are missing.
# ──────────────────────────────────────────────────────────────────────────
set(LUX_ENGINE_CONTENT_DIR "${CMAKE_BINARY_DIR}/engine_content")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/content/EngineContentPath.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lux/engine/editor/content/EngineContentPath.hpp
    @ONLY
)

# Render-shaders SSOT path — ShaderGen's bake (#include real gbuffer_encode /
# lighting_common / brdf SSOT) needs the render module's assets/shaders dir as the
# shaderc include search root. Same configure_file pattern as EngineContentPath.
set(LUX_RENDER_SHADERS_DIR "${CMAKE_SOURCE_DIR}/modules/function/render/assets/shaders")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/content/RenderShadersPath.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lux/engine/editor/content/RenderShadersPath.hpp
    @ONLY
)

# Source tree mirrors include/lux/engine/editor/ — each .cpp sits under the same
# relative path as its header (app/, content/, import/, panels/, scene/, thumbnail/).
# AssetRegistry.cpp / EditorTextureCache.cpp stay at src/ root (their headers are at
# the include root). FileDialog / MaterialGraphPanel / MaterialGraphBake are appended
# further below (they carry extra per-target wiring).
set(LUX_EDITOR_SRCS
    # src/ root (headers at include root)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorTextureCache.cpp
    # app/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorActions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorMenuBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ImportDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ImportController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/SceneController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProjectController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/RenderThreadOrchestrator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/LuxEditor.cpp
    # content/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/content/EditorBuiltins.cpp
    # import/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/import/AssetImporter.cpp
    # panels/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/AssetBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/FlowGraphPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/HierarchyPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/InspectorPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/LuaConsole.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/SceneFeatureSettingPanel.cpp
    # scene/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/DemoSceneTemplate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/EditorScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/Scene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/systems/CameraSceneSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/systems/SelectionSceneSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/systems/StreamingSceneSystem.cpp
    # thumbnail/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ImageCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/PreviewScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailSpecProviders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailService.cpp
)

add_component(
    COMPONENT_NAME editor
    STATIC
    NAMESPACE      lux::engine
    SOURCE_FILES   ${LUX_EDITOR_SRCS}
)

component_include_directories(
    editor
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        # src/ as a private include root: editor-internal headers that have no
        # public counterpart (panels/MaterialGraphPanel.hpp, import/MaterialGraphBake.hpp,
        # panels/FlowGraphPanel.hpp) are included as src-root-relative paths, so they
        # resolve the same regardless of which src subdir the includer lives in.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # SpawnHelpers.hpp is header-only and lives under engine/launcher/.
        # Editor source uses it from File→Switch Project; we don't want
        # to link the launcher static library (that would drag the picker
        # panel + duplicate UISystem ownership) — only the header.
        ${CMAKE_SOURCE_DIR}/engine/launcher/include
    INSTALL_TIME
        include
)

target_compile_definitions(
    editor
    PRIVATE
    LUX_EDITOR_LIBRARY
    PUBLIC
    LUX_EDITOR_DLL_DISABLE
)

component_add_transitive_commands(
    editor
    "find_package (lux-engine-function REQUIRED COMPONENTS ui flowforge ecs render_bridge)"
    "find_package (lux-engine-core REQUIRED COMPONENTS script meta)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package (lux-engine-serialize REQUIRED COMPONENTS serialize)"
    "find_package (lux-engine-project REQUIRED COMPONENTS project)"
    "find_package (lux-engine-execution REQUIRED COMPONENTS execution)"
    "find_package (lux-engine-graphkit REQUIRED COMPONENTS graphkit)"
    "find_package (imgui REQUIRED COMPONENTS glfw3 vulkan)"
    "find_package (node_editor REQUIRED COMPONENTS node_editor)"
    "find_package (lux-cxx REQUIRED COMPONENTS container reflection_runtime event)"
)

target_link_libraries(
    editor
    PUBLIC
    lux::engine::function::ui
    lux::engine::function::flowforge
    lux::engine::function::ecs             # World / ComponentTypeRegistry
    lux::engine::function::render_bridge   # RenderableSystem (bridge kernel)
    lux::engine::function::input           # ActionMapper / input action mapping
    lux::engine::core::script
    lux::engine::core::meta
    lux::engine::resource::asset
    lux::engine::serialize
    lux::engine::project
    lux::engine::execution
    graphkit
    imgui::glfw3
    imgui::vulkan
    imgui::node_editor
    lux::cxx::container
    lux::cxx::reflection_runtime
    lux::cxx::event                        # Selection::changed signal (signal/slot decoupling)
)

# ──────────────────────────────────────────────────────────────────────────
# Phase C: a few editor TUs build stdexec sender pipelines on EngineExecutor's
# schedulers (EditorTextureCache.cpp now; thumbnail/preview TUs later). stdexec is
# an opt-in INCLUDER concern — confine its heavy headers + the MSVC conformance
# switches to JUST those TUs (per-file), NOT the whole editor (which would push the
# ~30 imgui/node_editor/lua TUs through /permissive-, a large blast radius).
#
# Export-clean, mirroring the gameplay_meta / nfd pattern above: stdexec is
# header-only, so we add its HEADERS to the editor's private include path and do
# NOT link STDEXEC::stdexec (a link would leak a $<LINK_ONLY> ref into the editor's
# install export and fail at generate). The public headers (EngineExecutor.hpp via
# execution, EditorTextureCache.hpp via pimpl) stay stdexec-free, so no consumer of
# the editor needs stdexec — no transitive find_package required.
# Guard the find: the execution module already found stdexec and STDEXEC::stdexec is
# globally visible here, so re-running find_package would re-import the export set and
# fail ("some but not all targets already defined"). Only find it if not yet present.
if(NOT TARGET STDEXEC::stdexec)
    find_package(stdexec CONFIG REQUIRED)
endif()
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:STDEXEC::stdexec,INTERFACE_INCLUDE_DIRECTORIES>)
if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EditorTextureCache.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/PreviewScene.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailService.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor")
endif()


# D1: LuaConsole registers gameplay's generated sol2 bindings. The editor
# STATIC lib must NOT link the sidecar (the editor target is in an installed
# export set; gameplay_meta deliberately isn't — even a PRIVATE link leaks a
# $<LINK_ONLY> reference into the export and fails at generate time). It only
# needs the sidecar's BUILD include dir to compile the aggregator header
# (gameplay_lua_registration.hpp); the registration symbols themselves are
# dllexported from gameplay_meta.dll and resolve at the lux_editor exe link
# (which links gameplay_meta explicitly further below).
# pack_d3 is a packs/-tier library OUTSIDE every export set — the exported
# editor STATIC lib takes ONLY its include dirs (same trap/pattern as the
# meta sidecars below); the symbols resolve when lux_editor links pack_d3.
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:pack_d3,INTERFACE_INCLUDE_DIRECTORIES>)

if(TARGET ecs_meta)
    target_include_directories(editor PRIVATE
        $<TARGET_PROPERTY:ecs_meta,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(editor PRIVATE LUX_EDITOR_HAS_GAMEPLAY_LUA)
endif()
if(TARGET pack_d3_meta)
    target_include_directories(editor PRIVATE
        $<TARGET_PROPERTY:pack_d3_meta,INTERFACE_INCLUDE_DIRECTORIES>)
endif()


# Native file dialog (btzy/nativefiledialog-extended, vendored under
# engine/thirdparty/) — MANDATORY, like the shadergen backend below. Provides
# the editor's native open/save/import dialogs (engine/editor/src/app/FileDialog.cpp,
# the lux::editor::*Dialog facade). SAME export-cleanliness pattern as the
# gameplay_meta sidecar above: the editor STATIC lib is in an installed export
# set, so it must NOT link `nfd` (a PRIVATE link leaks a $<LINK_ONLY:nfd> ref
# into the export and fails at generate, since nfd is not exported). The lib only
# compiles FileDialog.cpp against nfd's HEADERS; the nfd symbols resolve at the
# lux_editor exe link (which links `nfd` explicitly, further below).
if(NOT TARGET nfd)
    message(FATAL_ERROR
        "editor requires the `nfd` target (native file dialogs). It is vendored at "
        "engine/thirdparty/nativefiledialog and added in engine/CMakeLists.txt "
        "before editor — check that directory exists + the add_subdirectory order.")
endif()
target_sources(editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/FileDialog.cpp)
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:nfd,INTERFACE_INCLUDE_DIRECTORIES>)

# FlowGraphPanel's IR pane uses MLIR via lux::engine::flowforge_compiler. When
# the compiler is enabled, link it (provides the PUBLIC LUX_FLOWFORGE_HAS_MLIR
# define plus the MLIR include paths). Otherwise the IR pane compiles out,
# guarded by the same macro.
if(LUX_ENABLE_FLOWFORGE_MLIR AND TARGET lux::engine::flowforge_compiler)
    target_link_libraries(editor PUBLIC lux::engine::flowforge_compiler)
endif()

# Material-graph backend (MaterialGraphPanel + MaterialGraphBake) — MANDATORY.
# The node graph is the editor's SOLE material path; EditorBuiltins / PreviewScene /
# ThumbnailService (compiled unconditionally) all synthesize materials through
# MaterialGraphBake, which bakes via ShaderGen. engine/CMakeLists.txt forces
# LUX_ENABLE_SHADERGEN ON for editor builds; fail loudly if the target is missing.
if(NOT TARGET shadergen_glsl)
    message(FATAL_ERROR
        "editor requires the shadergen_glsl target (the material codegen path). "
        "It should have been force-enabled in engine/CMakeLists.txt — check the "
        "LUX_ENABLE_SHADERGEN force + add_subdirectory order.")
endif()
target_sources(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/MaterialGraphPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/import/MaterialGraphBake.cpp)
# ShaderGen is the material codegen backend — MaterialGraphBake / MaterialGraphPanel
# bake graphs through it, and its neutral core carries the import converter
# (lux::shadergen::material::materialToGraph, used by AssetImporter). The old
# material_graph / material_graph_glsl are no longer linked here (retired).
target_link_libraries(editor PUBLIC shadergen_glsl)

# Link format library if needed
if(USE_LIBFMT)
    if(FMT_TARGET)
        target_link_libraries(editor PRIVATE ${FMT_TARGET})
    elseif(FMT_LIBRARIES)
        target_link_libraries(editor PRIVATE ${FMT_LIBRARIES})
        target_include_directories(editor PRIVATE ${FMT_INCLUDE_DIRS})
    endif()
endif()

# ──────────────────────────────────────────────────────────────────────────
# Editor executable — lux_editor
#
# Standalone GUI binary that boots LuxEditor. Kept as a separate target so
# the engine::editor static library can also be linked into other hosts
# (tests, alternative shells) without dragging in a main() symbol.
# ──────────────────────────────────────────────────────────────────────────
add_executable(
    lux_editor
    ${CMAKE_CURRENT_SOURCE_DIR}/app/main.cpp
)

target_link_libraries(
    lux_editor
    PRIVATE
        editor
)

# main.cpp uses lux::launcher::spawnLauncher() — the helper lives in a
# header-only file under engine/launcher/include. We only need the include
# directory, NOT the launcher static library (which would drag in the
# whole UI / render server stack again). Add it as a PRIVATE include for
# the executable target.
target_include_directories(
    lux_editor
    PRIVATE
        ${CMAKE_SOURCE_DIR}/engine/launcher/include
)

# Reflection sidecar — linked directly on the executable so the editor
# static library's install(EXPORT) interface stays clean (gameplay_meta is
# a developer-only sidecar; it has no place in the editor's install
# component set). The sidecar's INTERFACE_LINK_OPTIONS still publish
# `/INCLUDE:<func>_meta` linker flags to whichever consumer links it —
# applied to lux_editor here — so OS load of the .exe imports
# `lux_engine_function_gameplay_meta.dll`, firing every
# MetaModuleRegistrar static at process start.
target_link_libraries(lux_editor PRIVATE pack_d3)
if(TARGET ecs_meta)
    target_link_libraries(lux_editor PRIVATE ecs_meta)
endif()
if(TARGET pack_d3_meta)
    target_link_libraries(lux_editor PRIVATE pack_d3_meta)
endif()

# render_meta: the render module's editor-only reflection sidecar (feature
# param structs, e.g. TonemapParams). Same rationale + /INCLUDE:<func>_meta
# auto-load mechanism as gameplay_meta above — linked on the exe so the OS
# loads lux_engine_function_render_meta.dll and its MetaModuleRegistrar statics
# register the feature param types into ReflectionRegistry at process start.
if(TARGET render_meta)
    target_link_libraries(lux_editor PRIVATE render_meta)
endif()


# nativefiledialog — the editor STATIC lib compiled FileDialog.cpp against nfd's
# headers but deliberately did not link it (export cleanliness). Resolve the nfd
# symbols here, on the executable (which is never exported). `editor` is listed
# before `nfd` above so static-archive link order is GNU-ld-friendly too.
if(TARGET nfd)
    target_link_libraries(lux_editor PRIVATE nfd)
endif()

set_target_properties(
    lux_editor PROPERTIES
    OUTPUT_NAME "lux_editor"
)

set(ENABLE_EDITOR_TEST OFF CACHE BOOL "Enable Editor Test")
if(ENABLE_EDITOR_TEST)
    add_subdirectory(test)
endif()

# ──────────────────────────────────────────────────────────────────────────
# Content baker — adds the build-time tool that materialises engine_content/.
# Must come AFTER `editor` is defined because the baker links it for the
# UUID literals + BuiltinGeometry header.
# ──────────────────────────────────────────────────────────────────────────
add_subdirectory(baker)

# Bake cube / plane / white-PBR into ${LUX_ENGINE_CONTENT_DIR}.  The skybox
# texture is NOT bundled into engine_content — it stays in the render
# module's runtime-assets tree and EditorBuiltins loads it directly from
# `editor_runtime_asset_path` (unchanged from M1.5). Bringing the texture
# into engine_content would require depending on the render module's
# texture-bake target; not worth the build-graph complexity for an asset
# that already lives at a well-known location.
add_custom_command(
    OUTPUT
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Cube.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Plane.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Sphere.luxasset"
    COMMAND $<TARGET_FILE:lux_content_baker> "${LUX_ENGINE_CONTENT_DIR}"
    DEPENDS lux_content_baker
    COMMENT "Baking engine content into ${LUX_ENGINE_CONTENT_DIR}"
    VERBATIM
)

add_custom_target(engine_content_bake ALL
    DEPENDS
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Cube.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Plane.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Sphere.luxasset"
)

# Editor must be built BEFORE the baker runs (baker links editor), and the
# baked outputs must exist BEFORE lux_editor runs (so we add the bake step
# as a runtime dependency of the editor exe).
add_dependencies(lux_editor engine_content_bake)

# Install: ship engine_content alongside the executable.
install(DIRECTORY ${LUX_ENGINE_CONTENT_DIR}/
        DESTINATION bin/engine_content)
