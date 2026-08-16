message("-------- Packs --------")
# ─────────────────────────────────────────────────────────────────────────────
#  engine/runtime/packs/ — 引擎自带的**装配配方**（pack），不是 ECS 的功能部分。
#
#  一个 pack 说的是「这个玩法域由哪些系统 + 哪些渲染子系统 + 哪些组件组成」——
#  它跨 animation / physics / transform / render / pixel 五个域，没有任何单一
#  ECS 域能拥有它。所以它不在 `ecs/` 下：那里只放真正的 ECS 功能域。
#
#  ── 为什么在 engine/ 而不是仓库根，也不在 editor 里 ──────────────────────
#  消费者有四类：`engine/editor`、`engine/hosts/player`、`platforms/android/game`、
#  以及 `modules/function/render/test/` 的四个可视 demo。放进 editor 会让渲染
#  模块的测试反向依赖编辑器。`engine/` 是「宿主/工具层」，四类都够得着。
#  launcher 选「2D 场景」就预装配 2D pack —— 那正是它的消费方式。
#
#  ── 通往数据化的落脚点 ────────────────────────────────────────────────────
#  终局是 pack 变成**数据文件**（`scene2d.luxpack`），由反射生成的
#  「系统名 → 工厂」表装配出来。那条路已评估过（见本次工作线的计划），缺的是
#  生成器里一个 `LUX_SYSTEM()` 分支 + 两张注册表。届时这两个目录里的
#  静态 pack descriptor 可原地变成 `.luxpack` + 一个 loader，目录不必再动一次。
#
#  每个领域 contribution 都以 `lux::runtime` 中的 descriptor 工厂装配；include
#  前缀跟着职责位置走：`lux/engine/runtime/packs/`。
# ─────────────────────────────────────────────────────────────────────────────

add_subdirectory(scene2d)
add_subdirectory(scene3d)

# Export is owned by engine/runtime/CMakeLists.txt together with the other
# runtime components.  A second package/export set here would make the same
# target appear to belong to two products.
