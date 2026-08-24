# lux::engine::ecs::physics3d — 3D 刚体：Jolt 接线骨架。
#
# 从原 `ecs/physics` 拆出（批 6-B）。**今天全仓零用户** —— 它是 3D 物理阶段的
# 骨架。拆出来的收益正在这里：拆之前 Jolt 与 box2d 链在同一个目标上，于是每一个
# 只做 2D 的游戏都为这 78 行没人调的代码付一整个 Jolt。现在「不用 3D 物理就不链
# 本目标」，Jolt 从那些构建里彻底掉出去。
#
# Jolt 是 IMPLEMENTATION detail（ecs-layer ADR §8，用户裁定）：PRIVATE 链接，
# 头与符号都留在本 dll 里。
# namespace: lux::ecs

find_package(Jolt CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

set(LUX_ECS_PHYSICS3D_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/JoltPhysics3D.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics3DScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics3DSystem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticColliderBatch3DCodec.cpp
)

add_component(
    COMPONENT_NAME  physics3d
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_physics3d
    SOURCE_FILES    ${LUX_ECS_PHYSICS3D_SRCS}
)

lux_classify_target(
    TARGET physics3d
    LAYER ECS
    PRODUCT RUNTIME
    ROLE DOMAIN
)

target_link_libraries(physics3d PRIVATE Jolt::Jolt TBB::tbb)

component_include_directories(
    physics3d
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    physics3d
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

# Jolt is built without RTTI on Android, and engine code does not use RTTI.
# Keep the private adapter ABI identical on every platform; otherwise the
# derived JobSystem vtable asks the no-RTTI Jolt archive for missing typeinfo.
if(MSVC)
    target_compile_options(physics3d PRIVATE /GR-)
else()
    target_compile_options(physics3d PRIVATE -fno-rtti)
endif()

target_link_libraries(
    physics3d
    PUBLIC
    lux::engine::ecs::core
    lux::engine::core::math
    lux::engine::core::meta
    lux::engine::ecs::scene_format
    lux::cxx::compile_time
    PRIVATE
    lux::engine::core::serialization
)

component_add_transitive_commands(
    physics3d
    "find_package (lux-engine-ecs  REQUIRED COMPONENTS core scene_format)"
    "find_package (lux-engine-core REQUIRED COMPONENTS math meta serialization)"
    "find_package (lux-cxx REQUIRED COMPONENTS compile_time)"
)
# 注意：Jolt 刻意不在传递命令里（内部封装裁定）。

if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(physics3d_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET physics3d_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(physics3d_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_physics3d_meta)
    target_link_libraries(physics3d_meta
        PRIVATE
            physics3d
            lux::engine::core::meta
    )
    set(_physics3d_lua_meta_args "")
    if(LUX_BUILD_COMPONENT_LUA_META)
        target_link_libraries(physics3d_meta
            PRIVATE lux::engine::function::script_lua)
        list(APPEND _physics3d_lua_meta_args LUA_BINDING)
    endif()
    target_compile_definitions(physics3d_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         physics3d
        SIDECAR_TARGET physics3d_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/physics3d/components/Physics3DComponents.hpp
        ${_physics3d_lua_meta_args}
    )
endif()

if(NOT LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    add_subdirectory(streaming)
endif()

add_subdirectory(test)
