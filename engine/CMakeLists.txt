message("-------- Editor Tier --------")

## engine/ — the editor tier.
##
## Unlike modules/<layer>, engine/ is NOT a runtime layer: it depends on
## modules/* but modules/* must NEVER depend on engine/. It holds the
## lux-engine editor (engine-coupled ImGui panels + shell), the FlowForge
## MLIR compiler, and the build-time asset packer.
##
## Component gating:
##   asset_pipeline      — built UNCONDITIONALLY. lux_asset_packer is a
##                         build-time dependency of the RUNTIME itself
##                         (function::render embeds builtin shaders through
##                         it, resource::asset's add_asset() drives it). It
##                         links ONLY resource::asset, so building it under
##                         LUX_BUILD_EDITOR=OFF leaks no editor dependency.
##   editor              — gated by LUX_BUILD_EDITOR.
##   flowforge_compiler  — gated by LUX_BUILD_EDITOR + LUX_ENABLE_FLOWFORGE_MLIR
##                         (the option + early return() live in its own
##                         CMakeLists, as before).

# Tier-level visibility macros, consumed by the editor-tier components
# (editor, flowforge_compiler). The reusable UI framework lives in
# modules/function/ui and carries its own component-level visibility header.
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/visibility.h
)

# asset_pipeline — unconditional (runtime build-time dependency, see header).
# The root CMakeLists.txt normally adds engine/asset_pipeline EARLY (right after
# the resource layer, before the function layer) so lux_asset_packer exists as a
# target when function/render configures. The `NOT TARGET` guard makes this
# subdirectory self-contained when engine/ is added standalone, without
# double-adding when the root already pulled it in.
if(NOT TARGET lux_asset_packer AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/asset_pipeline/CMakeLists.txt)
    add_subdirectory(asset_pipeline)
endif()

# serialize — binary file I/O for editor + future runtime. Built whenever the
# editor is built (its only consumer today is the editor). When M3b adds
# engine/runtime/, this will move to an unconditional build like asset_pipeline.
if(LUX_BUILD_EDITOR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/serialize/CMakeLists.txt)
    add_subdirectory(serialize)
endif()

# project — Project / ProjectManifest / RecentProjects data layer. Pure data
# (TOML + std::filesystem), zero render dependencies. Both lux_editor and
# lux_launcher link this. Must come BEFORE editor and launcher so its target
# exists when they try to link it.
if(LUX_BUILD_EDITOR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/project/CMakeLists.txt)
    add_subdirectory(project)
endif()

# The editor tier proper is gated by LUX_BUILD_EDITOR.
if(LUX_BUILD_EDITOR)
    # flowforge_compiler MUST be added before editor: editor's conditional
    # `if(LUX_ENABLE_FLOWFORGE_MLIR AND TARGET flowforge_compiler)` link needs
    # the option defined and the target created first.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/flowforge_compiler/CMakeLists.txt)
        add_subdirectory(flowforge_compiler)
    endif()
    # shadergen_glsl — the shader-synthesis backend (ShaderIR -> GLSL -> SPIR-V).
    # The editor's material codegen path (it RETIRED the old material_graph_glsl):
    # MaterialGraphBake / MaterialGraphPanel bake graphs through it; EditorBuiltins /
    # PreviewScene / ThumbnailService all synthesize materials this way. MANDATORY for
    # the editor — force the option ON before its CMakeLists runs.
    set(LUX_ENABLE_SHADERGEN ON CACHE BOOL
        "Build the shadergen GLSL->SPIR-V backend (editor/cook-side)" FORCE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/shadergen_glsl/CMakeLists.txt)
        add_subdirectory(shadergen_glsl)
    endif()
    # graphkit — the shared, domain-agnostic node-graph editor framework. Must
    # come BEFORE editor: the editor panels host gk GraphEditors and link it.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/graphkit/CMakeLists.txt)
        add_subdirectory(graphkit)
    endif()
    # nativefiledialog — btzy/nativefiledialog-extended (zlib), vendored under
    # engine/thirdparty/. Native OS open/save/folder dialogs for the editor's
    # asset-import / open-project / open-scene flows (replaces the old
    # paste-a-path text prompt). Built via its OWN upstream CMake (target `nfd`,
    # Windows IFileDialog backend); we consume it purely in-tree from the
    # lux_editor exe, so force its tests + install OFF. Must precede editor (the
    # editor STATIC lib compiles against nfd's headers; the exe links `nfd`).
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL     OFF CACHE BOOL "" FORCE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/nativefiledialog/CMakeLists.txt)
        add_subdirectory(thirdparty/nativefiledialog)
    endif()
    # execution — engine async runtime (stdexec). EngineExecutor (CPU pool + main
    # sync point + asset lazy-load orchestrator). STATIC, linked by editor + launcher.
    # Must precede editor (the editor constructs an EngineExecutor + injects its
    # requestLoad closure into the gameplay systems).
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/execution/CMakeLists.txt)
        add_subdirectory(execution)
    endif()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt)
        add_subdirectory(editor)
    endif()

    if(TARGET editor)
        install_components(
            PROJECT_NAME    lux-engine-editor
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      editor
        )
    endif()

    if(TARGET serialize)
        install_components(
            PROJECT_NAME    lux-engine-serialize
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      serialize
        )
    endif()

    if(TARGET graphkit)
        install_components(
            PROJECT_NAME    lux-engine-graphkit
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      graphkit
        )
    endif()

    if(TARGET project)
        install_components(
            PROJECT_NAME    lux-engine-project
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      project
        )
    endif()

    if(TARGET execution)
        install_components(
            PROJECT_NAME    lux-engine-execution
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      execution
        )
    endif()

    # launcher — slim Project Launcher exe + tiny static library. Added
    # AFTER editor because the install rule for `lux_launcher` is part
    # of this subdirectory and is independent of the editor's exports.
    # launcher needs `lux::engine::project` and `lux::engine::platform::window`
    # which are both already configured at this point.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/launcher/CMakeLists.txt)
        add_subdirectory(launcher)
    endif()

    # flowforge_compiler is opt-in (LUX_ENABLE_FLOWFORGE_MLIR) and must be
    # installed separately so a default build doesn't try to install a target
    # that was never created.
    if(LUX_ENABLE_FLOWFORGE_MLIR AND TARGET flowforge_compiler)
        install_components(
            PROJECT_NAME    lux-engine-flowforge-compiler
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      flowforge_compiler
        )
    endif()

    # shadergen_glsl is opt-in (LUX_ENABLE_SHADERGEN); install only when created.
    if(LUX_ENABLE_SHADERGEN AND TARGET shadergen_glsl)
        install_components(
            PROJECT_NAME    lux-engine-shadergen-glsl
            VERSION         0.0.1
            NAMESPACE       lux::engine
            COMPONENTS      shadergen_glsl
        )
    endif()
endif()
