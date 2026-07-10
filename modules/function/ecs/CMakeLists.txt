# lux::engine::function::ecs — the ECS kernel (ADR: lux-engine-pack-architecture)
#
# World / ISystem / Hierarchy / Name / ComponentTypeRegistry / DebugDraw.
# Domain-free by contract: no 2D/3D/render types ever land here — domain
# assemblies live in packs/, mechanisms in their capability modules.
# namespace: lux::ecs

set(LUX_ECS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentTypeRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DebugDraw.cpp
)

add_component(
    COMPONENT_NAME  ecs
    NAMESPACE       lux::engine::function
    OUTPUT_NAME     lux_engine_function_ecs
    SOURCE_FILES    ${LUX_ECS_SRCS}
)

component_include_directories(
    ecs
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    ecs
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    ecs
    PUBLIC
    lux::engine::core::math
    lux::engine::core::meta            # EntityRegistry / reflection seam
    lux::engine::core::script          # DebugDraw LUX_FUNC lua-bound free functions
    lux::engine::resource::asset       # AssetLoadFn carries asset_id_t
)

component_add_transitive_commands(
    ecs
    "find_package (lux-engine-core     REQUIRED COMPONENTS math meta script)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset)"
)

# ---------------------------------------------------------------------------
# Reflection + Lua binding code generation (editor-only sidecar) — the split
# heir of the old gameplay_meta: this sidecar carries the ECS-kernel reflected
# types; packs with reflected components ship their own sidecars (pack_d3_meta).
# ---------------------------------------------------------------------------
if(LUX_BUILD_EDITOR)
    include_component_cmake_scripts(meta)

    add_library(ecs_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    set_target_properties(ecs_meta PROPERTIES
        OUTPUT_NAME lux_engine_function_ecs_meta)
    target_link_libraries(ecs_meta
        PRIVATE
            ecs
            lux::engine::core::meta
    )
    target_compile_definitions(ecs_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         ecs
        SIDECAR_TARGET ecs_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/components/NameComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/DebugDraw.hpp
        LUA_BINDING
    )
endif()
