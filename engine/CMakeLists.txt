message("-------- Engine Products --------")

## engine/ — product and integration layers above modules/ and ecs/.
##
## Runtime orchestration, Authoring models, Toolchain transforms, Editor
## domains and Hosts live in separate subtrees.  modules/* and ecs/* must
## never depend on this directory; concrete extensions remain leaves.
##
## Component gating:
##   editor              — selected only by Editor-capable profiles.
##   toolchain_flowforge  — selected by Toolchain-capable profiles and its own option.
##                         (the option + early return() live in its own
##                         CMakeLists, as before).

# Product-level visibility macros, consumed only by the Editor domain. The
# Toolchain components own independent visibility headers and never include
# Editor headers. The reusable UI framework lives in
# modules/function/ui and carries its own component-level visibility header.
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/visibility.h
)

# CORE-001: establish the Engine-owned Extension API target before moving the
# ABI headers out of modules/core. The compatibility implementation remains
# lux::engine::core::extension_abi until CORE-002..005 are complete.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/extensions/api/CMakeLists.txt)
    add_subdirectory(extensions/api)
endif()


# The cooked game-launch protocol is shared by Player front ends and the
# offline exporter. It is an Engine product contract, not a Resource format.
if((LUX_PROFILE_HAS_RUNTIME OR LUX_PROFILE_HAS_TOOLCHAIN) AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/game/deployment/CMakeLists.txt)
    add_subdirectory(game/deployment)
endif()

# Runtime foundation comes before every runtime domain.  A TOOLCHAIN-only
# product consumes Resource formats and Authoring models directly; it does not
# configure the game scheduler, renderer integration or scene lifecycle.
if(LUX_PROFILE_HAS_RUNTIME AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/runtime/CMakeLists.txt)
    add_subdirectory(runtime)
endif()

# Platform-neutral playable application. Both the reference desktop Player and
# Android NativeActivity adapter link this installed SDK component. A future
# game project can do the same while owning its executable metadata and icon.
if(LUX_PROFILE_HAS_REFERENCE_PLAYER AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/hosts/game_application/CMakeLists.txt)
    add_subdirectory(hosts/game_application)
endif()

# Reference desktop Player adapter: GLFW window, native handle, input snapshots
# and the process event loop. Runtime composition is owned by game_application.
if(LUX_PROFILE_HAS_REFERENCE_PLAYER AND
   NOT LUX_TARGET_OS STREQUAL "ANDROID" AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/hosts/player/CMakeLists.txt)
    add_subdirectory(hosts/player)
endif()

# Developer-only deterministic World runner. It reuses the installed Player
# composition but remains a TEST product and never enters a deployment.
if(LUX_BUILD_PROFILE STREQUAL "DEVELOPER" AND
   NOT LUX_TARGET_OS STREQUAL "ANDROID" AND
   EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/hosts/world_benchmark/CMakeLists.txt)
    add_subdirectory(hosts/world_benchmark)
endif()

set(_lux_host_components "")
foreach(_host_component IN ITEMS game_application player_host)
    if(TARGET ${_host_component})
        list(APPEND _lux_host_components ${_host_component})
    endif()
endforeach()
if(_lux_host_components)
    install_components(
        PROJECT_NAME    lux-engine-host
        VERSION         0.0.1
        NAMESPACE       lux::engine::host
        COMPONENTS      ${_lux_host_components}
    )
endif()

# Authoring, offline compilers and editor UI are never part of a Player-only
# product. DEVELOPER/EDITOR configure them in the same tree while preserving
# target-level Player cleanliness.
if(LUX_PROFILE_HAS_EDITOR OR LUX_PROFILE_HAS_TOOLCHAIN)
    # Source-workspace data is shared by Editor and Toolchain. Player and
    # Android consume only cooked game contracts and never configure this
    # desktop-host subtree.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/authoring/CMakeLists.txt)
        add_subdirectory(authoring)
    endif()

    # Authoring/cook compilers live above Runtime. Adding this subtree only in
    # editor/toolchain profiles keeps compiler libraries out of Player closure.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/toolchain/CMakeLists.txt)
        add_subdirectory(toolchain)
    endif()
    # (runtime/execution lives OUTSIDE this gate — see the runtime block above:
    #  the game host streams assets through the same executor.)
    if(LUX_PROFILE_HAS_EDITOR AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt)
        add_subdirectory(editor)
    endif()

    if(TARGET editor)
        install_components(
            PROJECT_NAME    lux-engine-editor
            VERSION         0.0.1
            NAMESPACE       lux::engine::editor
            COMPONENTS      editor_graphkit editor
        )
    endif()


    # launcher — slim Project Launcher exe + tiny static library. Added
    # AFTER editor because the install rule for `lux_launcher` is part
    # of this subdirectory and is independent of the editor's exports.
    # launcher needs authoring::project and platform::window
    # which are both already configured at this point.
    if(LUX_PROFILE_HAS_EDITOR AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/hosts/launcher/CMakeLists.txt)
        add_subdirectory(hosts/launcher)
    endif()

    if(LUX_PROFILE_HAS_EDITOR AND
       EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/hosts/editor/CMakeLists.txt)
        add_subdirectory(hosts/editor)
    endif()
endif()
