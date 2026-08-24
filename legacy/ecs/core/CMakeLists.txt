# lux::engine::ecs::core — the ECS kernel (ADR: lux-engine-ecs-layer-architecture)
#
# World / ISystem / Hierarchy / Name / ComponentTypeCatalog / DebugDraw.
# Promoted OUT of function (was lux::engine::function::ecs) into the ecs layer.
# C++ namespace `lux::ecs` and include prefix `lux/engine/ecs/` are UNCHANGED.
# (迁移期曾有 lux::engine::function::ecs 的 compat ALIAS,早已移除。)
# Domain-free by contract: no 2D/3D/render types ever land here.

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)
find_package(EnTT CONFIG REQUIRED)

set(LUX_ECS_CORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentTypeCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/DebugDraw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneServices.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Schedule.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScheduleTopology.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ScheduleBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RegistryMemoryResource.cpp
)

add_component(
    COMPONENT_NAME  core
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_core
    SOURCE_FILES    ${LUX_ECS_CORE_SRCS}
)

component_include_directories(
    core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME
        include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

lux_classify_target(
    TARGET core
    LAYER ECS
    PRODUCT RUNTIME
    ROLE FOUNDATION
)

target_compile_definitions(
    core
    PRIVATE
    LUX_ECS_LIBRARY
)

target_link_libraries(
    core
    PUBLIC
    lux::cxx::algorithm                # stable schema ids (FNV-1a)
    lux::cxx::compile_time             # SceneServices type_hash + expected
    lux::engine::core::math
    lux::engine::core::meta            # generic reflection vocabulary
    lux::engine::ecs::identity              # component + persistent entity identity
    EnTT::EnTT
)

component_add_transitive_commands(
    core
    "find_package (lux-cxx             REQUIRED COMPONENTS algorithm compile_time)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS identity)"
    "find_package (lux-engine-core     REQUIRED COMPONENTS math meta)"
    "find_package (EnTT CONFIG REQUIRED)"
)

add_executable(
    schedule_close_frontier_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/schedule_close_frontier_test.cpp
)
target_link_libraries(
    schedule_close_frontier_test
    PRIVATE lux::engine::ecs::core
)
if(MSVC)
    target_compile_options(schedule_close_frontier_test PRIVATE /UNDEBUG)
else()
    target_compile_options(schedule_close_frontier_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  schedule_close_frontier_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME phase9_schedule_reverse_close_frontier
    COMMAND schedule_close_frontier_test
)
set_tests_properties(
    phase9_schedule_reverse_close_frontier
    PROPERTIES LABELS "phase9;cpu"
)

# ---------------------------------------------------------------------------
# Reflection + Lua binding code generation — carries the ECS-kernel reflected
# types (Name / DebugDraw). Shared components (Transform…) own separate
# sidecars in their defining targets.
#
# This used to be gated on Editor builds and described as an "editor-only
# sidecar". That was wrong, and the consequence was silent: the sidecar is what
# populates ComponentTypeCatalog, and the EntityScene stager resolves cooked
# schemas through that catalogue. Keeping the sidecar runtime-visible also
# makes headless and Player publication follow the same strict schema path as
# Editor.
#
# Consequently: any build that must read cooked EntityScene components needs this
# — which includes every runtime target. It is gated on its own option so a
# genuinely reflection-free build is still expressible, defaulting to ON.
# ---------------------------------------------------------------------------
option(LUX_BUILD_COMPONENT_META
       "Build component reflection sidecars required by cooked World entities" ON)

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(ecs_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET ecs_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(ecs_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_core_meta)
    target_link_libraries(ecs_meta
        PRIVATE
            core
            lux::engine::core::meta
    )
    target_compile_definitions(ecs_meta PRIVATE LUX_FUNCTION_LIBRARY)

    set(_ecs_core_lua_meta_args "")
    if(LUX_BUILD_COMPONENT_LUA_META)
        add_library(ecs_lua_meta SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
        lux_classify_target(
            TARGET ecs_lua_meta
            LAYER ECS
            PRODUCT RUNTIME
            ROLE INTEGRATION
        )
        set_target_properties(ecs_lua_meta PROPERTIES
            OUTPUT_NAME lux_engine_ecs_core_lua_meta)
        target_link_libraries(ecs_lua_meta
            PRIVATE
                core
                lux::engine::core::meta
                lux::engine::function::script_lua
        )
        target_compile_definitions(
            ecs_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)
        list(APPEND _ecs_core_lua_meta_args
            LUA_SIDECAR_TARGET ecs_lua_meta
            LUA_BINDING)
    endif()

    engine_enable_module_meta(
        TARGET         core
        SIDECAR_TARGET ecs_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/components/NameComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/DebugDraw.hpp
            # 共享的空间基座（批 6-D 从已删的 ecs/components 折进来）。
            # ★ 批 6-G：反射清单跟着**拥有这些头的目标**走，不再挂在某个 pack 上 ——
            #   链哪个目标，就带哪些反射符号。这是 ecs 将来能作为插件模块存在的前提。
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/components/Transform2DComponent.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/components/Transform3DComponent.hpp
            # Core Math stays reflection-free. ECS owns the non-intrusive
            # reflection adapter needed by nested tagged properties.
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/reflection/SpatialValueReflectionMeta.hpp
        ${_ecs_core_lua_meta_args}
    )

    add_executable(
        spatial_value_reflection_contract_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial_value_reflection_contract_test.cpp
    )
    target_link_libraries(
        spatial_value_reflection_contract_test
        PRIVATE
            core
            ecs_meta
    )
    if(MSVC)
        target_compile_options(
            spatial_value_reflection_contract_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(
            spatial_value_reflection_contract_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  spatial_value_reflection_contract_test
        LAYER   ECS
        PRODUCT TEST
        ROLE    FOUNDATION
    )
endif()

set(ENABLE_ECS_CORE_TEST OFF CACHE BOOL "Enable ECS core tests")
if(ENABLE_ECS_CORE_TEST)
    add_subdirectory(test)
endif()
