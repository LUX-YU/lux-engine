# lux::engine::ecs::physics2d — 2D 刚体：Box2D 求解器 + 它周边的 ECS 收发。
#
# 从原 `ecs/physics` 拆出（批 6-B）。留在那边的是**中立层**（定步累加器 +
# 碰撞探针缝）；这里是所有认识「2D 刚体」的东西：
#   Box2DPhysics2D（真 Box2D-v3 世界，内部封装）/ Physics2DSystem（ECS 收发）/
#   Physics2DWorld（演示级扫掠 AABB 解算器）/ Physics2DComponents / Physics2DConfig /
#   runtime pack owns contribution descriptors. Cross-Pixel/Script adapters
#   位于 ecs/integration，避免基础领域互相拖入。
#
# ── 为什么单独一个目标 ────────────────────────────────────────────────────
# 拆之前 box2d 与 Jolt 链在同一个目标上：只做 2D 的游戏照样把 Jolt 拖进产物。
# 现在「不用 2D 物理就不链本目标」。
#
# Box2D 是 IMPLEMENTATION detail（ecs-layer ADR §8，用户裁定）：PRIVATE 链接，
# 头与符号都留在本 dll 里，下游链的是 ecs::physics2d，绝不是 box2d。
# namespace: lux::ecs

find_package(box2d CONFIG REQUIRED)

set(LUX_ECS_PHYSICS2D_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Box2DPhysics2D.cpp    # 真 Box2D-v3 世界（内部）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DSystem.cpp   # 绕着 Box2DPhysics2D 的 ECS 收发
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DWorld.cpp    # 演示级扫掠 AABB 解算器
)

add_component(
    COMPONENT_NAME  physics2d
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_physics2d
    SOURCE_FILES    ${LUX_ECS_PHYSICS2D_SRCS}
)

lux_classify_target(
    TARGET physics2d
    LAYER ECS
    PRODUCT RUNTIME
    ROLE DOMAIN
)

# 求解器是实现细节 —— PRIVATE，box2d 的头/符号留在 physics2d.dll 内。
target_link_libraries(physics2d PRIVATE box2d::box2d)

component_include_directories(
    physics2d
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    physics2d
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    physics2d
    PUBLIC
    lux::engine::ecs::core             # EntityRegistry + spatial transforms
    lux::engine::ecs::physics          # 定步累加器 + 碰撞探针缝
    lux::engine::core::math
    lux::engine::core::meta
    lux::engine::resource::spatial
)

component_add_transitive_commands(
    physics2d
    "find_package (lux-engine-ecs  REQUIRED COMPONENTS core physics)"
    "find_package (lux-engine-core REQUIRED COMPONENTS math meta)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS spatial)"
)

add_executable(physics2d_ecs_static_collider_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/ecs_static_collider_test.cpp)
lux_classify_target(
    TARGET physics2d_ecs_static_collider_test
    LAYER ECS
    PRODUCT TEST
    ROLE DOMAIN)
target_link_libraries(
    physics2d_ecs_static_collider_test
    PRIVATE physics2d)
add_test(NAME physics2d_ecs_static_collider_test
    COMMAND physics2d_ecs_static_collider_test)
set_tests_properties(
    physics2d_ecs_static_collider_test
    PROPERTIES LABELS "cpu;ecs;physics2d")
if(MSVC)
    target_compile_options(physics2d_ecs_static_collider_test PRIVATE /UNDEBUG)
else()
    target_compile_options(physics2d_ecs_static_collider_test PRIVATE -UNDEBUG)
endif()
# 注意：box2d 刻意不在传递命令里 —— 下游链 ecs::physics2d，不继承任何第三方求解器
# （内部封装裁定）。

# ---------------------------------------------------------------------------
# 2D 刚体的作者组件 的反射 sidecar（批 6-G：反射跟着拥有它的目标走，不再挂在 pack 上 ——
# 链哪个目标就带哪些反射符号，这是 ecs 将来能作为插件模块存在的前提）。
# ---------------------------------------------------------------------------
if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    add_library(physics2d_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET physics2d_meta
        LAYER ECS
        PRODUCT RUNTIME
        ROLE INTEGRATION
    )
    set_target_properties(physics2d_meta PROPERTIES
        OUTPUT_NAME lux_engine_ecs_physics2d_meta)
    target_link_libraries(physics2d_meta
        PRIVATE
            physics2d
            lux::engine::core::meta
            lux::engine::function::script_lua
    )
    target_compile_definitions(physics2d_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         physics2d
        SIDECAR_TARGET physics2d_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/physics2d/components/Physics2DComponents.hpp
        LUA_BINDING
    )
endif()
