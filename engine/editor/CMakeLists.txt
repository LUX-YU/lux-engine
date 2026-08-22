## engine::editor — the lux-engine editor application.
##
## Holds the engine-coupled editor panels (node graph editor, lua console,
## asset browser) plus the editor shell. It links the reusable UI framework
## (function::ui) and the engine subsystems those panels drive (flowforge,
## script, asset). The FlowForge MLIR compiler is linked only when
## LUX_ENABLE_FLOWFORGE_MLIR is ON.
##
## This component is selected only by an Editor-capable build profile.
## is never linked into a shipped game runtime.

find_package(imgui       REQUIRED COMPONENTS glfw3 vulkan)
find_package(node_editor REQUIRED COMPONENTS node_editor)
find_package(lux-cxx     REQUIRED COMPONENTS core container reflection_runtime event)

# Editor-only frameworks are owned and configured by the Editor product. They
# are invisible to Runtime and Toolchain configuration.
add_subdirectory(framework/graphkit)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL     OFF CACHE BOOL "" FORCE)
add_subdirectory(framework/native_dialog)
if(TARGET nfd)
    lux_classify_target(
        TARGET nfd
        LAYER EDITOR
        PRODUCT EDITOR
        ROLE FOUNDATION
    )
endif()

# (格式兼容层的编辑器私有复制已删除 —— lux::format 统一来自
#  lux-cxx core 的 <lux/cxx/core/Format.hpp>。)

# Asset path header — EditorScene loads the built-in skybox texture
# (modules/function/render/features/assets/textures/blue_nebulae_1.luxasset) so the
# SkyboxPass node has a real texture to render against. The render
# component already registers its runtime assets, so we just generate a
# header pointing at that root.
include_component_cmake_scripts(lux::engine::resource::asset)
component_generate_runtime_asset_path_header(
    COMPONENT     render_features
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
set(LUX_RENDER_SHADERS_DIR "${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders")
# The .lglsl emitted tree (a mirror of canonical GLSL): once shared headers
# migrate to .lglslh, the editor's ShaderGen include resolution must check
# the emitted output first, then fall back to the source tree — see
# RenderShadersPath.hpp.in.
set(LUX_RENDER_SHADERS_EMITTED_DIR "${CMAKE_BINARY_DIR}/lglsl_emitted")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/content/RenderShadersPath.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/lux/engine/editor/content/RenderShadersPath.hpp
    @ONLY
)

# Source tree mirrors include/lux/engine/editor/ — each .cpp sits under the same
# relative path as its header (app/, content/, import/, panels/, scene/, thumbnail/).
# AssetRegistry.cpp stays at src/ root (its header is at the include root).
# FileDialog / MaterialGraphPanel are appended further below; material graph
# compilation lives in Toolchain and is shared with offline cooks.
# (they carry extra per-target wiring).
set(LUX_EDITOR_SRCS
    # src/ root (headers at include root)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetRegistry.cpp
    # app/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorActions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorMenuBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ImportDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ImportController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorAsyncService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AssetDeleteController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/AssetFileWatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/SceneController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/ProjectController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorShell.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/LuxEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/SpawnRecipes.cpp
    # extensions/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/extensions/EditorTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/extensions/EditorContributionRegistrar.cpp
    # flow/ — editor-side FlowGraph compile coordination
    ${CMAKE_CURRENT_SOURCE_DIR}/src/flow/FlowGraphCompiler.cpp
    # content/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/content/EditorBuiltins.cpp
    # import/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/import/AssetImporter.cpp
    # panels/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/AssetBrowser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/FlowGraphPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/ExtensionMonitorPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/HierarchyPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/InspectorPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/LuaConsole.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/SceneSettingsPanel.cpp
    # scene/
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/DemoSceneTemplate.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/EditorScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/WorldActorEcsAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/InstanceSpawnClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/systems/CameraSceneSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/systems/SelectionSceneSystem.cpp
    # thumbnail/ — 也承载两个私有预览 SceneRuntime 宿主的共用装配件与活材质预览宿主
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ImageCodec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/PreviewWorldCommon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/MaterialPreviewHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailSpecProviders.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailService.cpp
)

add_component(
    COMPONENT_NAME editor
    STATIC
    NAMESPACE      lux::engine::editor
    SOURCE_FILES   ${LUX_EDITOR_SRCS}
)

lux_classify_target(
    TARGET  editor
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    DOMAIN
)

component_include_directories(
    editor
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        # src/ as a private include root: editor-internal headers that have no
        # public counterpart (panels/MaterialGraphPanel.hpp,
        # panels/FlowGraphPanel.hpp) are included as src-root-relative paths, so they
        # resolve the same regardless of which src subdir the includer lives in.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # SpawnHelpers.hpp is header-only and lives under engine/hosts/launcher/.
        # Editor source uses it from File→Switch Project; we don't want
        # to link the launcher static library (that would drag the picker
        # panel + duplicate UISystem ownership) — only the header.
        ${CMAKE_SOURCE_DIR}/engine/hosts/launcher/include
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
    "find_package (lux-engine-function REQUIRED COMPONENTS ui ui_vulkan navigation)"
    "find_package (lux-engine-ecs REQUIRED COMPONENTS core render_2d render_3d animation_2d animation_3d navigation3d physics presentation_2d transform)"
    "find_package (lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package (lux-engine-function REQUIRED COMPONENTS script_lua)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-content REQUIRED COMPONENTS content)"
    "find_package (lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS project authoring_flowforge authoring_assets authoring_world)"
    "find_package(lux-engine-toolchain REQUIRED COMPONENTS toolchain_spatial3d_scene_cook)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution runtime_logging runtime_frame runtime_assets runtime_scene_script runtime_extension_loader runtime_extension_orchestration runtime_render_scene runtime_render_backend_host runtime_spatial2d_tilemap runtime_tilemap_systems runtime_spatial3d_navigation_adapter runtime_spatial3d_physics)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_api)"
    "find_package (imgui REQUIRED COMPONENTS glfw3 vulkan)"
    "find_package (node_editor REQUIRED COMPONENTS node_editor)"
    "find_package (lux-cxx REQUIRED COMPONENTS core container reflection_runtime)"
)

target_link_libraries(
    editor
    PUBLIC
    lux::cxx::core
    lux::engine::extensions::extension_api
    lux::engine::function::ui
    lux::engine::function::ui_vulkan
    lux::engine::function::navigation
    lux::engine::authoring::authoring_flowforge
    lux::engine::authoring::authoring_world
    lux::engine::ecs::core                 # World / ComponentTypeCatalog / ScheduleBuilder
    lux::engine::ecs::render_2d     # 2D components and extraction
    lux::engine::ecs::render_3d     # 3D components and extraction
    lux::engine::ecs::animation_2d  # flipbook preview/runtime behavior
    lux::engine::ecs::animation_3d  # skeletal preview/runtime behavior
    lux::engine::ecs::navigation3d
    lux::engine::function::input           # ActionMapper / input action mapping
    lux::engine::function::script_lua
    lux::engine::core::meta
    lux::engine::resource::asset
    lux::engine::content
    lux::engine::toolchain::toolchain_model_importer
    lux::engine::toolchain::toolchain_texture_importer
    lux::engine::core::serialization
    lux::engine::ecs::component_archive
    lux::engine::runtime::runtime_render_scene             # the host-neutral scene runtime EditorScene hosts
    lux::engine::runtime::runtime_render_backend_host      # process-level render thread/channel owner
    lux::engine::authoring::project
    lux::engine::authoring::authoring_assets
    lux::engine::toolchain::toolchain_asset_cook
    lux::engine::toolchain::toolchain_spatial3d_scene_cook
    lux::engine::runtime::runtime_execution
    lux::engine::runtime::runtime_logging
    lux::engine::runtime::runtime_assets
    lux::engine::runtime::runtime_spatial2d_tilemap
    lux::engine::runtime::runtime_spatial3d_navigation_adapter
    lux::engine::runtime::runtime_spatial3d_physics
    lux::engine::runtime::runtime_scene_script
    lux::engine::runtime::runtime_frame # shared frame lifecycle; no private pump protocol
    lux::engine::core::log                 # sink assembly + diagnostics (§7.1)
    lux::engine::core::events              # 统一事件总线(批B:宿主装配层骨架)
    lux::engine::platform::filewatch       # 资产热更新触发源(OS 文件事件,批7)
    lux::engine::editor::editor_graphkit
    imgui::glfw3
    imgui::vulkan
    imgui::node_editor
    lux::cxx::container
    lux::cxx::reflection_runtime
)

# ──────────────────────────────────────────────────────────────────────────
# Phase C: a few editor TUs build stdexec sender pipelines on AsyncRuntime's
# schedulers. stdexec is an opt-in INCLUDER concern — confine its
# heavy headers + the MSVC conformance switches to JUST those TUs (per-file), NOT
# the whole editor (which would push the ~30 imgui/node_editor/lua TUs through
# /permissive-, a large blast radius).
#
# Export-clean, mirroring the gameplay_meta / nfd pattern above: stdexec is
# header-only, so we add its HEADERS to the editor's private include path and do
# NOT link STDEXEC::stdexec (a link would leak a $<LINK_ONLY> ref into the editor's
# install export and fail at generate). AsyncRuntime.hpp stays stdexec-free, so
# no consumer of the editor needs stdexec — no
# transitive find_package required.
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/thumbnail/ThumbnailService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/LuxEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/InstanceSpawnClient.cpp
        PROPERTIES COMPILE_OPTIONS "/permissive-;/Zc:__cplusplus;/Zc:preprocessor")
    # EditorScene owns the complete editor/runtime scene composition and also
    # crosses MSVC's legacy COFF section ceiling under Debug /Od.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/EditorScene.cpp
        PROPERTIES COMPILE_OPTIONS
            "/permissive-;/Zc:__cplusplus;/Zc:preprocessor;/bigobj")
    # This TU is the single typed-operation composition root. Each operation
    # instantiates a distinct stdexec pipeline and can exceed COFF's legacy
    # 65,279-section ceiling even though there is no runtime-size problem.
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/app/EditorAsyncService.cpp
        PROPERTIES COMPILE_OPTIONS
            "/permissive-;/Zc:__cplusplus;/Zc:preprocessor;/bigobj")
endif()


# D1: LuaConsole registers gameplay's generated sol2 bindings. The editor
# STATIC lib must NOT link the sidecar (the editor target is in an installed
# export set; gameplay_meta deliberately isn't — even a PRIVATE link leaks a
# $<LINK_ONLY> reference into the export and fails at generate time). It only
# needs the sidecar's BUILD include dir to compile the aggregator header
# (gameplay_lua_registration.hpp); the registration symbols themselves are
# dllexported from gameplay_meta.dll and resolve at the lux_editor exe link
# (which links gameplay_meta explicitly further below).
# 2D and orthogonal 3D assembly targets provide direct installer headers;
# 符号在下面 lux_editor 链这些目标时解析。
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:presentation_2d,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:physics,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:transform,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:animation_3d,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:runtime_tilemap_systems,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:runtime_navigation3d_systems,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:runtime_physics3d_systems,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:runtime_presentation3d_systems,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:runtime_spatial3d_streaming_systems,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:physics2d,INTERFACE_INCLUDE_DIRECTORIES>)

# ecs::scripting — ScriptSystem / ScriptComponent / scriptRegistry, used by EditorScene
# (Edit/Play) + the built-in demo script in LuxEditor. SAME pattern as packs::sceneXd above:
# headers on the editor STATIC lib (so its TUs compile), the target linked on the
# lux_editor EXE below — NOT here, because scripting is not in the editor's install
# export set (a PUBLIC/PRIVATE link would drag it in and fail install(EXPORT)).
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:scripting,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:scripting_lua,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:scripting_native,INTERFACE_INCLUDE_DIRECTORIES>)

if(TARGET ecs_lua_meta)
    target_include_directories(editor PRIVATE
        $<TARGET_PROPERTY:ecs_lua_meta,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(editor PRIVATE LUX_EDITOR_HAS_GAMEPLAY_LUA)
endif()
foreach(_sc
        render_components_lua_meta
        render_components_2d_lua_meta
        render_components_3d_lua_meta
        pixel_lua_meta
        animation_2d_lua_meta
        script_lua_meta)
    if(TARGET ${_sc})
        target_include_directories(editor PRIVATE
            $<TARGET_PROPERTY:${_sc},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()


# Native file dialog (btzy/nativefiledialog-extended, vendored under
# engine/editor/framework/native_dialog) — MANDATORY, like the shader backend below.
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
        "engine/editor/framework/native_dialog and added by engine/editor/CMakeLists.txt "
        "before editor — check that directory exists + the add_subdirectory order.")
endif()
target_sources(editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/FileDialog.cpp)
target_include_directories(editor PRIVATE
    $<TARGET_PROPERTY:nfd,INTERFACE_INCLUDE_DIRECTORIES>)

# FlowGraphPanel's IR pane uses MLIR via lux::engine::toolchain::toolchain_flowforge. When
# the compiler is enabled, link it (provides the PUBLIC LUX_FLOWFORGE_HAS_MLIR
# define plus the MLIR include paths). Otherwise the IR pane compiles out,
# guarded by the same macro.
if(LUX_ENABLE_FLOWFORGE_MLIR AND TARGET lux::engine::toolchain::toolchain_flowforge)
    target_link_libraries(editor PUBLIC lux::engine::toolchain::toolchain_flowforge)
endif()

# Material-graph backend (MaterialGraphPanel + Toolchain compiler) — MANDATORY.
# The node graph is the editor's SOLE material path; EditorBuiltins /
# ThumbnailService (compiled unconditionally) synthesize materials through
# MaterialGraphBake, which bakes via ShaderGen. engine/CMakeLists.txt forces
# LUX_ENABLE_SHADERGEN ON for editor builds; fail loudly if the target is missing.
if(NOT TARGET lux::engine::toolchain::toolchain_shader_compiler)
    message(FATAL_ERROR
        "editor requires the toolchain_shader_compiler target (the material codegen path). "
        "It should have been force-enabled in engine/CMakeLists.txt — check the "
        "LUX_ENABLE_SHADERGEN force + add_subdirectory order.")
endif()
target_sources(editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/panels/MaterialGraphPanel.cpp)
# ShaderGen is the material codegen backend — MaterialGraphBake / MaterialGraphPanel
# bake graphs through it, and its neutral core carries the import converter
# (lux::shadergen::material::materialToGraph, used by AssetImporter). The old
# material_graph / material_graph_glsl are no longer linked here (retired).
target_link_libraries(
    editor
    PUBLIC
        lux::engine::toolchain::toolchain_shader_compiler
        lux::engine::toolchain::toolchain_material_compiler
        lux::engine::toolchain::toolchain_builtin_content
)

set(ENABLE_EDITOR_TEST OFF CACHE BOOL "Enable Editor Test")
if(ENABLE_EDITOR_TEST)
    add_subdirectory(test)
endif()

# ──────────────────────────────────────────────────────────────────────────
# Bake cube / plane / white-PBR into ${LUX_ENGINE_CONTENT_DIR}. The baker is
# a Toolchain target configured before Editor. The skybox
# texture is NOT bundled into engine_content — it stays in the render
# module's runtime-assets tree and EditorBuiltins loads it directly from
# `editor_runtime_asset_path` (unchanged from M1.5). Bringing the texture
# into engine_content would require depending on the render module's
# texture-bake target; not worth the build-graph complexity for an asset
# that already lives at a well-known location.
# OUTPUT 此前只声明三个 mesh,材质是未声明副产物 —— 删掉 Materials/ 不触发
# 重烘焙。补上代表性材质文件(白 + M_Missing;八个自发光同批产出,不逐一列)。
add_custom_command(
    OUTPUT
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Cube.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Plane.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Meshes/SM_Sphere.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Materials/M_WhitePbr.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Materials/M_Missing.luxasset"
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
        "${LUX_ENGINE_CONTENT_DIR}/Materials/M_WhitePbr.luxasset"
        "${LUX_ENGINE_CONTENT_DIR}/Materials/M_Missing.luxasset"
)

# Install: ship engine_content alongside the executable.
install(DIRECTORY ${LUX_ENGINE_CONTENT_DIR}/
        DESTINATION bin/engine_content)
