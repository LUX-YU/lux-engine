# lux::engine::ecs::physics — 物理域的**中立层**：没有求解器，也不链任何第三方。
#
# 这里只有三样东西，共同点是「与维度无关、与求解器无关」：
#   · Simulation2DSystem / FixedStepConfig —— 每个定步能力共用的那一个累加器。
#     像素场的 ApplyFieldCommands / SimulateFields 相位也挂在它上面，所以它不能
#     跟着 2D 刚体走。（名字里的 2D 是历史，它其实维度中立。）
#   · CollisionProbe2D —— 碰撞探针的**缝**（Aabb2 / ICollision2DProbe /
#     CollisionProbes2D）。纯词汇，没有实现。
#
# 求解器在两个兄弟目标里：`ecs/physics2d`（Box2D）与 `ecs/physics3d`（Jolt）。
#
# ── 为什么拆 ──────────────────────────────────────────────────────────────
# 拆之前这一个目标同时链着 box2d **和** Jolt，于是「只做 2D 的游戏」照样把 Jolt
# 拖进构建产物，反之亦然。而 `JoltPhysics3D` 当时全仓零用户 —— 那是为一份没人调
# 的 78 行代码付一整个第三方库。用户裁定：上层不用哪种物理，就不该引入它的依赖。
# namespace: lux::ecs

set(LUX_ECS_PHYSICS_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation2DSystem.cpp
)

add_component(
    COMPONENT_NAME  physics
    NAMESPACE       lux::engine::ecs
    OUTPUT_NAME     lux_engine_ecs_physics
    SOURCE_FILES    ${LUX_ECS_PHYSICS_SRCS}
)

lux_classify_target(
    TARGET physics
    LAYER ECS
    PRODUCT RUNTIME
    ROLE DOMAIN
)

component_include_directories(
    physics
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME
        include
)

target_compile_definitions(
    physics
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

target_link_libraries(
    physics
    PUBLIC
    lux::engine::ecs::core             # EntityRegistry / World / ISystem
    lux::engine::core::math            # Aabb2 等碰撞词汇
    lux::engine::core::meta
)

component_add_transitive_commands(
    physics
    "find_package (lux-engine-ecs  REQUIRED COMPONENTS core)"
    "find_package (lux-engine-core REQUIRED COMPONENTS math meta)"
)
