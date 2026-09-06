# lux::engine::ecs::scripting — the ScriptSystem folder (scripting ADR v3.3).
#
# Per-entity script behaviors: ONE ScriptComponent + ONE ScriptSystem driving
# bind-time-resolved ABI calls (C++, Lua and native AOT). Native/C++ code is
# trusted process code; an ABI-reported non-zero result disables the script,
# while a native memory fault follows the host process crash policy.
#
# NOTE: CMake target is `scripting` (not `script`) — modules/core/script already
# owns the target name `script`. Alias lux::engine::ecs::scripting; DLL
# lux_engine_ecs_script; code namespace lux::ecs.

set(LUX_ECS_SCRIPT_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptEventRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptAssetRequestSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystem.cpp
)

add_component(
    COMPONENT_NAME  scripting
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_script
    SOURCE_FILES    ${LUX_ECS_SCRIPT_SRCS}
)

lux_classify_target(
    TARGET scripting
    LAYER ECS
    PRODUCT RUNTIME
    ROLE INTEGRATION
)

component_include_directories(
    scripting
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(
    scripting
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    scripting
    PUBLIC
    lux::engine::ecs::core                  # World / ISystem / EntityRegistry / EntityHandle / ComponentTypeCatalog
    lux::engine::core::meta                 # RefClass/RefField/EBaseType (reflection get/set)
    lux::engine::resource::asset            # ScriptComponent.script = asset_id_t + asset routing (rdesc::Script/ScriptAsset)
    lux::engine::function::script_core      # public ScriptBehavior ABI types
    PRIVATE
    lux::engine::core::log
)

component_add_transitive_commands(
    scripting
    "find_package (lux-engine-ecs  REQUIRED COMPONENTS core)"
    "find_package (lux-engine-core REQUIRED COMPONENTS meta)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package (lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package (lux-engine-core REQUIRED COMPONENTS log)"
)

# Session backends are separate Runtime components. A Player that installs
# only ScriptSystem and native C++ behaviours carries neither LuaJIT nor the
# native dynamic-module backend.
if(LUX_SCRIPT_HAS_LUA)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
    find_package(sol2 CONFIG REQUIRED)
    find_package(LuaJIT REQUIRED)

    add_component(
        COMPONENT_NAME scripting_lua
        NAMESPACE       lux::engine::ecs
        OUTPUT_NAME     lux_engine_ecs_script_lua
        SOURCE_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/LuaScriptBackend.cpp
    )
    lux_classify_target(
        TARGET scripting_lua
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    component_include_directories(
        scripting_lua
        BUILD_TIME_EXPORT
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
    )
    target_compile_definitions(scripting_lua PRIVATE LUX_FUNCTION_LIBRARY)
    target_link_libraries(
        scripting_lua
        PUBLIC lux::engine::ecs::scripting
        PRIVATE
            sol2::sol2
            LuaJIT::LuaJIT
            lux::engine::function::input
            lux::engine::core::log
    )
    component_add_transitive_commands(
        scripting_lua
        [[list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")]]
        "find_package(LuaJIT REQUIRED)"
        "find_package(sol2 CONFIG REQUIRED)"
        "find_package(lux-engine-ecs REQUIRED COMPONENTS scripting)"
        "find_package(lux-engine-function REQUIRED COMPONENTS input)"
        "find_package(lux-engine-core REQUIRED COMPONENTS log)"
    )
endif()

add_component(
    COMPONENT_NAME scripting_native
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_script_native
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backends/NativeModuleScriptBackend.cpp
)
lux_classify_target(
    TARGET scripting_native
    LAYER ECS
    PRODUCT RUNTIME
    ROLE INTEGRATION
)
component_include_directories(
    scripting_native
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scripting_native PRIVATE LUX_FUNCTION_LIBRARY)
target_link_libraries(
    scripting_native
    PUBLIC lux::engine::ecs::scripting
    PRIVATE lux::engine::function::script_native
)
component_add_transitive_commands(
    scripting_native
    "find_package(lux-engine-ecs REQUIRED COMPONENTS scripting)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_native)"
)

# ---------------------------------------------------------------------------
# Reflection sidecar for ScriptComponent — so it survives a scene save/load
# and appears in Add Component. Mirrors ecs/scene's scene_meta.
# slug = `scripting` → LuxRegisterScriptingMetas.
#
# NOT editor-only despite the old label: "survives a scene save/load" is a
# runtime property, not an editor one. A runtime that loads a scene containing
# scripted entities needs this registered, or those entities come back without
# their ScriptComponent. See LUX_BUILD_COMPONENT_META in ecs/core/CMakeLists.txt.
#
# (By contrast render/CMakeLists.txt's render_meta stays Editor-only:
#  it reflects Tonemap/SpatialCull/ShadowQuality *Params*, which are editor
#  tuning panels and never appear in a saved scene.)
# ---------------------------------------------------------------------------
if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(script_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET script_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(script_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_script_meta)
    target_link_libraries(script_meta
        PRIVATE
            scripting
            lux::engine::core::meta
    )
    target_compile_definitions(script_meta PRIVATE LUX_FUNCTION_LIBRARY)

    set(script_meta_args
        TARGET         scripting
        SIDECAR_TARGET script_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/script/components/ScriptComponent.hpp
    )
    if(LUX_BUILD_COMPONENT_LUA_META)
        add_library(script_lua_meta SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
        lux_classify_target(
            TARGET script_lua_meta
            LAYER ECS
            PRODUCT RUNTIME
            ROLE INTEGRATION
        )
        set_target_properties(script_lua_meta PROPERTIES
            OUTPUT_NAME lux_engine_ecs_script_lua_meta)
        target_link_libraries(script_lua_meta
            PRIVATE
                scripting
                lux::engine::core::meta
                lux::engine::function::script_lua
        )
        target_compile_definitions(
            script_lua_meta PRIVATE LUX_FUNCTION_LIBRARY
        )
        list(APPEND script_meta_args
            LUA_SIDECAR_TARGET script_lua_meta
            LUA_BINDING
        )
    endif()

    engine_enable_module_meta(${script_meta_args})
endif()
