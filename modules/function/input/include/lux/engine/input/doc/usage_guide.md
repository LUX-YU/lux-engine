# Input System — 使用指南

本文档说明如何在 `lux-engine` 中使用输入系统模块。
所有头文件在 `lux/engine/gameplay/input/` 下，命名空间为 `lux::gameplay`。

---

## 目录

1. [快速上手](#快速上手)
2. [注册动作](#注册动作)
3. [创建绑定](#创建绑定)
4. [设置上下文与栈](#设置上下文与栈)
5. [更新与查询](#更新与查询)
6. [修改器（Modifiers）](#修改器)
7. [触发器（Triggers）](#触发器)
8. [注入 API（程序化输入）](#注入-api)
9. [多上下文与优先级](#多上下文与优先级)
10. [UI 阻塞](#ui-阻塞)
11. [实战示例：FPS 角色控制器](#fps-控制器)
12. [架构管线总览](#架构管线总览)

---

<a id="快速上手"></a>
## 1. 快速上手

最小可运行示例 — 绑定空格键跳跃并检测触发：

```cpp
#include <lux/engine/gameplay/input/ActionMapper.hpp>

using namespace lux::gameplay;

// 1) 创建处理器
ActionMapper mapper;
auto& reg = mapper.actionRegistry();

// 2) 注册动作（运行时分配 ID）
ActionId jump = reg.registerAction({0, "jump", InputValueType::Bool});

// 3) 创建输入上下文，构建绑定
InputContext ctx("gameplay");
ctx.actionMap()
    .bindKey(jump, KeyEnum::KEY_SPACE, InputValue::makeBool(true));

// 4) 准备上下文栈
InputContextStack stack;
stack.push(&ctx);

// 5) 每帧更新
// InputSnapshot snap = window.captureInputSnapshot();
mapper.update(snap, stack, dt);

// 6) 查询结果
if (mapper.triggered(jump)) {
    // 玩家按下了空格
}
```

---

<a id="注册动作"></a>
## 2. 注册动作

所有动作必须先注册才能使用。 `InputActionRegistry` 自动分配从 1 递增的 `ActionId`。

```cpp
ActionMapper mapper;
auto& reg = mapper.actionRegistry();

// 简写方式：只需名称和值类型
ActionId jump   = reg.registerAction({0, "jump",   InputValueType::Bool});
ActionId move   = reg.registerAction({0, "move",   InputValueType::Axis2D});
ActionId look   = reg.registerAction({0, "look",   InputValueType::Axis2D});
ActionId zoom   = reg.registerAction({0, "zoom",   InputValueType::Axis1D});
```

**幂等性**: 同名重复注册返回相同 ID，不会创建新条目。

```cpp
ActionId a = reg.registerAction({0, "jump", InputValueType::Bool});
ActionId b = reg.registerAction({0, "jump", InputValueType::Bool});
assert(a == b);  // 幂等
```

**高级定义**（带动作级修改器/触发器）:

```cpp
InputActionDesc aim_desc;
aim_desc.name          = "aim";
aim_desc.value_type    = InputValueType::Bool;
aim_desc.action_triggers = {
    {TriggerKind::Hold, TriggerLogicType::Explicit, 0.5f, /*hold_time=*/0.5f}
};
ActionId aim = reg.registerAction(aim_desc);
```

**名称查找**（冷路径，不宜放入热循环）:

```cpp
ActionId id = reg.findByName("jump");
assert(id != InvalidActionId);
```

---

<a id="创建绑定"></a>
## 3. 创建绑定

通过 `ActionMap` 的流式 API 创建绑定。每个绑定将一个物理输入映射到一个动作，并声明贡献值。

### 3.1 键盘绑定

```cpp
ActionMap& am = ctx.actionMap();

// Bool 动作 — 按下即触发
am.bindKey(jump, KeyEnum::KEY_SPACE, InputValue::makeBool(true));

// Axis2D 动作 — 多键累加
am.bindKey(move, KeyEnum::KEY_W, InputValue::makeAxis2D( 0.f,  1.f))   // 前
  .bindKey(move, KeyEnum::KEY_S, InputValue::makeAxis2D( 0.f, -1.f))   // 后
  .bindKey(move, KeyEnum::KEY_A, InputValue::makeAxis2D(-1.f,  0.f))   // 左
  .bindKey(move, KeyEnum::KEY_D, InputValue::makeAxis2D( 1.f,  0.f));  // 右
```

WASD 同时按下 W + D 时，两个贡献值会被累加为 `(1.0, 1.0)`。
如果动作定义了 `Normalize2D` 修改器，会在累加后归一化到单位长度。

### 3.2 鼠标按键绑定

```cpp
am.bindMouseButton(fire, MouseButton::MOUSE_BUTTON_LEFT,
                   InputValue::makeBool(true));
```

### 3.3 鼠标轴绑定

```cpp
// 鼠标位移 → 视角
am.bindMouseAxis(look, MouseAxisInput::Axis::DeltaX,
                 InputValue::makeAxis2D(1.f, 0.f))
  .bindMouseAxis(look, MouseAxisInput::Axis::DeltaY,
                 InputValue::makeAxis2D(0.f, 1.f));

// 滚轮 → 缩放
am.bindMouseAxis(zoom, MouseAxisInput::Axis::ScrollY,
                 InputValue::makeAxis1D(1.f));
```

`DeltaX` 和 `DeltaY` 在本帧的位移像素值会分别作为标量乘以贡献向量，
然后累加到 `look` 的 Axis2D 值上。

### 3.4 带绑定级修改器和触发器

```cpp
// 绑定级修改器：应用 2 倍缩放
am.bindKey(move, KeyEnum::KEY_W, InputValue::makeAxis2D(0.f, 1.f),
           /*modifiers=*/{{ModifierKind::Scale, 2.f, 2.f, 0.f}},
           /*triggers=*/{});

// 绑定级触发器：仅在上升沿触发
am.bindKey(jump, KeyEnum::KEY_SPACE, InputValue::makeBool(true),
           /*modifiers=*/{},
           /*triggers=*/{{TriggerKind::Pressed, TriggerLogicType::Explicit, 0.5f}});
```

### 3.5 清理

```cpp
am.unbindAll(move);  // 移除指定动作的所有绑定
am.clear();          // 清空所有绑定
```

---

<a id="设置上下文与栈"></a>
## 4. 设置上下文与栈

### InputContext

```cpp
// 构造：名称, 是否消耗键盘, 是否消耗鼠标, 优先级
InputContext gameplay_ctx("gameplay", false, false, 0);
InputContext ui_ctx("ui", /*consumesKB=*/true, /*consumesMouse=*/true, /*priority=*/10);
```

重要属性：
- **enabled** — 禁用的上下文被跳过
- **priority** — 越高优先级越先评估
- **consumesKeyboard / consumesMouse** — 消耗后阻止低优先级上下文接收相应输入

### InputContextStack

```cpp
InputContextStack stack;
stack.push(&gameplay_ctx);
stack.push(&ui_ctx);
// 按优先级自动排序：ui(10) > gameplay(0)

// 临时移除
stack.pop(&ui_ctx);

// 查询
if (stack.contains(&gameplay_ctx)) { /* ... */ }
```

---

<a id="更新与查询"></a>
## 5. 更新与查询

### 每帧更新

```cpp
// 从窗口层获取快照
InputSnapshot snap = window.captureInputSnapshot();
float dt = snap.sample_dt;

// 更新管线（五阶段自动执行）
mapper.update(snap, stack, dt);

// 可选：传入 UI 阻塞标志
mapper.update(snap, stack, dt,
              /*allowKeyboard=*/!snap.keyboard_captured_by_ui,
              /*allowMouse=*/!snap.mouse_captured_by_ui);
```

### 查询 API

在 `update()` 之后调用查询：

```cpp
// 布尔查询
if (mapper.triggered(jump))  { /* 本帧触发条件完全满足 */ }
if (mapper.ongoing(aim))     { /* 触发中（如 Hold 计时中） */ }
if (mapper.canceled(aim))    { /* 触发在完成前被中断 */ }
if (mapper.active(move))     { /* 有物理输入活跃 */ }

// 取值
InputValue val = mapper.getValue(move);
auto [x, y] = val.as2D();   // 解构为 Float2

float zoom_delta = mapper.getValue(zoom).as1D();

// 完整状态
const ActionState& st = mapper.state(sprint);
if (st.started())   { /* 本帧从非活跃变为活跃 */ }
if (st.completed()) { /* 本帧从活跃变为非活跃 */ }
float held = st.held_seconds;  // 持续按压的秒数
```

---

<a id="修改器"></a>
## 6. 修改器（Modifiers）

修改器在绑定级和动作级均可使用。绑定级在 contribution 投影后应用，动作级在所有绑定累加后应用。

### 可用修改器

| ModifierKind | 参数 | 效果 |
|--------------|------|------|
| `Scale` | x, y, z | 按分量乘以缩放系数 |
| `NegateX` | — | 翻转 X 轴 |
| `NegateY` | — | 翻转 Y 轴 |
| `NegateZ` | — | 翻转 Z 轴 |
| `DeadZone` | x=threshold | 值幅度低于阈值时归零 |
| `Normalize2D` | — | XY 向量归一到单位长度 |
| `Clamp1D` | x=min, y=max | 限制 1D 值到 [min, max] |

### 典型用法

**动作级 Normalize2D** — WASD 对角线移动归一化：

```cpp
InputActionDesc move_desc;
move_desc.name       = "move";
move_desc.value_type = InputValueType::Axis2D;
move_desc.action_modifiers = {{ModifierKind::Normalize2D}};
ActionId move = reg.registerAction(move_desc);
```

**绑定级 Scale** — 灵敏度调整：

```cpp
am.bindMouseAxis(look, MouseAxisInput::Axis::DeltaX,
                 InputValue::makeAxis2D(1.f, 0.f),
                 /*modifiers=*/{{ModifierKind::Scale, 0.5f, 0.5f, 0.f}});
```

**链式修改器** — 先死区过滤，再缩放：

```cpp
am.bindMouseAxis(look, MouseAxisInput::Axis::DeltaX,
                 InputValue::makeAxis2D(1.f, 0.f),
                 /*modifiers=*/{
                     {ModifierKind::DeadZone, 0.05f},
                     {ModifierKind::Scale, 2.f, 2.f, 0.f}
                 });
```

---

<a id="触发器"></a>
## 7. 触发器（Triggers）

默认情况下（无触发器声明），绑定使用隐式 `Down` 触发器 — 值超过阈值即触发。

### 7.1 常用触发器

**Pressed 上升沿** — 仅按下瞬间触发一次：
```cpp
am.bindKey(jump, KeyEnum::KEY_SPACE, InputValue::makeBool(true),
           {}, {{TriggerKind::Pressed, TriggerLogicType::Explicit, 0.5f}});
```

**Hold 长按** — 持续按住一定时间后触发：
```cpp
InputActionDesc aim_desc;
aim_desc.name       = "aim";
aim_desc.value_type = InputValueType::Bool;
aim_desc.action_triggers = {
    {TriggerKind::Hold, TriggerLogicType::Explicit, 0.5f, /*hold_time=*/0.5f}
};
```
在 hold_time 达到之前，`mapper.ongoing(aim)` 返回 true。

**Tap 快速点击** — 在时限内按下+松开：
```cpp
am.bindKey(quicktap, KeyEnum::KEY_Q, InputValue::makeBool(true),
           {}, {{TriggerKind::Tap, TriggerLogicType::Explicit, 0.5f, 0.f, /*tap_time=*/0.3f}});
```

**HoldAndRelease 蓄力释放** — 长按后松开时触发：
```cpp
am.bindKey(charge, KeyEnum::KEY_E, InputValue::makeBool(true),
           {}, {{TriggerKind::HoldAndRelease, TriggerLogicType::Explicit, 0.5f, /*hold_time=*/0.4f}});
```

**Pulse 连发** — 按住期间以固定间隔重复触发：
```cpp
am.bindKey(autofire, KeyEnum::KEY_R, InputValue::makeBool(true),
           {}, {{TriggerKind::Pulse, TriggerLogicType::Explicit, 0.5f, 0.f, 0.f, /*interval=*/0.2f}});
```

**ChordAction 组合键** — 要求另一动作同时活跃：
```cpp
am.bindKey(sprint_fire, KeyEnum::KEY_F, InputValue::makeBool(true),
           {}, {{TriggerKind::ChordAction, TriggerLogicType::Explicit, 0.5f,
                 0.f, 0.f, 0.f, /*chord_action=*/sprint}});
// --> 必须同时按住 Shift（Sprint）+ F 才会触发
```

### 7.2 触发器组合逻辑

多个触发器可以组合在同一个绑定或动作上，通过 `TriggerLogicType` 控制：

| 类型 | 规则 |
|------|------|
| `Explicit` | 任意一个 Explicit 触发器通过即可 |
| `Implicit` | 所有 Implicit 触发器必须全部通过 |
| `Blocker` | 任一 Blocker 触发则整个动作被阻止 |

```cpp
// 示例：需要 ChordAction(Sprint) 作为 Implicit + Down 作为 Explicit
am.bindKey(sprint_fire, KeyEnum::KEY_F, InputValue::makeBool(true), {},
    {
        {TriggerKind::ChordAction, TriggerLogicType::Implicit, 0.5f, 0.f, 0.f, 0.f, sprint},
        {TriggerKind::Down, TriggerLogicType::Explicit, 0.5f}
    });
```

---

<a id="注入-api"></a>
## 8. 注入 API（程序化输入）

用于回放系统、AI 控制、自动化测试等场景。

### injectTriggered — 单帧触发

```cpp
mapper.injectTriggered(jump, InputValue::makeBool(true));
// 下一帧 update() 后 mapper.triggered(jump) == true
// 再下一帧自动清除
```

### injectValue — 持久值

```cpp
mapper.injectValue(move, InputValue::makeAxis2D(1.f, 0.f));
// 维持直到被新的 injectValue 覆盖或 injectValue(move, zero) 清除
```

### ActionMapperDispatcher

通过 `IActionDispatcher` 接口解耦调用方：

```cpp
ActionMapperDispatcher dispatcher(mapper);
dispatcher.dispatchTriggered(jump, InputValue::makeBool(true));
dispatcher.dispatchValue(move, InputValue::makeAxis2D(0.f, 1.f));
```

---

<a id="多上下文与优先级"></a>
## 9. 多上下文与优先级

输入系统支持多个上下文通过优先级系统共存：

```cpp
// UI 层：高优先级，消耗键盘（游戏层收不到键盘输入）
InputContext ui_ctx("ui", /*consumesKB=*/true, /*consumesMouse=*/false, /*priority=*/10);
ui_ctx.actionMap()
    .bindKey(confirm, KeyEnum::KEY_SPACE, InputValue::makeBool(true));

// 游戏层：低优先级
InputContext gameplay_ctx("gameplay", false, false, 0);
gameplay_ctx.actionMap()
    .bindKey(jump, KeyEnum::KEY_SPACE, InputValue::makeBool(true));

InputContextStack stack;
stack.push(&gameplay_ctx);
stack.push(&ui_ctx);

// 按 Space：UI 的 confirm 触发，gameplay 的 jump 不触发
mapper.update(snap, stack, dt);
assert(mapper.triggered(confirm));
assert(!mapper.triggered(jump));
```

临时禁用上下文：

```cpp
ui_ctx.setEnabled(false);
// 现在键盘输入不再被消耗，gameplay 层重新可用
```

---

<a id="ui-阻塞"></a>
## 10. UI 阻塞

平台层提供 UI 输入捕获标志（如 ImGui `WantCaptureKeyboard`）：

```cpp
InputSnapshot snap = window.captureInputSnapshot();
mapper.update(snap, stack, dt,
              /*allowKeyboard=*/!snap.keyboard_captured_by_ui,
              /*allowMouse=*/!snap.mouse_captured_by_ui);
```

当 `allowKeyboard = false` 时，所有键盘绑定被标记为 `blocked_by_ui`，不参与评估。
鼠标同理。这是**最高优先级**的全局阻塞层，在上下文消耗之上。

---

<a id="fps-控制器"></a>
## 11. 实战示例：FPS 角色控制器

以下展示一个完整的游戏循环中如何集成输入系统：

```cpp
#include <lux/engine/gameplay/input/ActionMapper.hpp>

using namespace lux::gameplay;

// ── 动作 ID 全局变量 ──
ActionId act_move;
ActionId act_look;
ActionId act_jump;
ActionId act_sprint;
ActionId act_fire;
ActionId act_aim;

void setupInput(ActionMapper& mapper) {
    auto& reg = mapper.actionRegistry();

    // 注册动作
    act_jump   = reg.registerAction({0, "jump",   InputValueType::Bool});
    act_sprint = reg.registerAction({0, "sprint", InputValueType::Bool});
    act_fire   = reg.registerAction({0, "fire",   InputValueType::Bool});
    act_look   = reg.registerAction({0, "look",   InputValueType::Axis2D});

    // 带 Normalize2D 的移动动作
    InputActionDesc move_desc;
    move_desc.name       = "move";
    move_desc.value_type = InputValueType::Axis2D;
    move_desc.action_modifiers = {{ModifierKind::Normalize2D}};
    act_move = reg.registerAction(move_desc);

    // 带 Hold 触发器的瞄准动作
    InputActionDesc aim_desc;
    aim_desc.name       = "aim";
    aim_desc.value_type = InputValueType::Bool;
    aim_desc.action_triggers = {
        {TriggerKind::Hold, TriggerLogicType::Explicit, 0.5f, 0.3f}
    };
    act_aim = reg.registerAction(aim_desc);
}

void setupBindings(InputContext& ctx) {
    auto& am = ctx.actionMap();

    // WASD 移动
    am.bindKey(act_move, KeyEnum::KEY_W, InputValue::makeAxis2D( 0.f,  1.f))
      .bindKey(act_move, KeyEnum::KEY_S, InputValue::makeAxis2D( 0.f, -1.f))
      .bindKey(act_move, KeyEnum::KEY_A, InputValue::makeAxis2D(-1.f,  0.f))
      .bindKey(act_move, KeyEnum::KEY_D, InputValue::makeAxis2D( 1.f,  0.f));

    // 鼠标视角
    am.bindMouseAxis(act_look, MouseAxisInput::Axis::DeltaX,
                     InputValue::makeAxis2D(1.f, 0.f))
      .bindMouseAxis(act_look, MouseAxisInput::Axis::DeltaY,
                     InputValue::makeAxis2D(0.f, 1.f));

    // 按键
    am.bindKey(act_jump,   KeyEnum::KEY_SPACE,      InputValue::makeBool(true));
    am.bindKey(act_sprint, KeyEnum::KEY_LEFT_SHIFT,  InputValue::makeBool(true));

    // 鼠标
    am.bindMouseButton(act_fire, MouseButton::MOUSE_BUTTON_LEFT,
                       InputValue::makeBool(true));
    am.bindMouseButton(act_aim,  MouseButton::MOUSE_BUTTON_RIGHT,
                       InputValue::makeBool(true));
}

// ── 游戏循环 ──
void gameLoop(LuxWindow& window) {
    ActionMapper mapper;
    setupInput(mapper);

    InputContext ctx("gameplay");
    setupBindings(ctx);

    InputContextStack stack;
    stack.push(&ctx);

    while (!window.shouldClose()) {
        window.pollEvents();
        InputSnapshot snap = window.captureInputSnapshot();
        float dt = snap.sample_dt;

        mapper.update(snap, stack, dt,
                      !snap.keyboard_captured_by_ui,
                      !snap.mouse_captured_by_ui);

        // ── 移动 ──
        if (mapper.active(act_move)) {
            auto [mx, my] = mapper.getValue(act_move).as2D();
            float speed = mapper.active(act_sprint) ? 10.f : 5.f;
            // player.move(mx * speed * dt, my * speed * dt);
        }

        // ── 视角 ──
        if (mapper.active(act_look)) {
            auto [yaw, pitch] = mapper.getValue(act_look).as2D();
            float sensitivity = 0.003f;
            // camera.rotate(yaw * sensitivity, pitch * sensitivity);
        }

        // ── 跳跃 ──
        if (mapper.triggered(act_jump)) {
            // player.jump();
        }

        // ── 射击 ──
        if (mapper.triggered(act_fire)) {
            // weapon.fire();
        }

        // ── 瞄准（Hold 0.3s 后触发） ──
        if (mapper.triggered(act_aim)) {
            // camera.toggleADS(true);
        }
        if (mapper.canceled(act_aim)) {
            // camera.toggleADS(false);  // 松开太早，取消瞄准
        }

        // ── 按压时长 ──
        float sprint_time = mapper.state(act_sprint).held_seconds;
        // 可用于体力消耗计算等

        // window.swapBuffers();
    }
}
```

---

<a id="架构管线总览"></a>
## 12. 架构管线总览

每帧 `mapper.update()` 内部执行的五个阶段：

```
┌─────────────────────────────────────────────────────────────────┐
│  beginFrame()                                                   │
│    · prev_* = current_*                                         │
│    · 应用一次性注入（injectTriggered）                             │
│    · 应用持久注入（injectValue）                                  │
├─────────────────────────────────────────────────────────────────┤
│  evaluateBindings(snapshot, contextStack)                       │
│    · 遍历 ContextStack（高→低优先级）                              │
│    · 跳过 disabled 上下文                                        │
│    · 跳过被 UI 阻塞的绑定（blocked_by_ui）                        │
│    · 跳过被上下文消耗的绑定（blocked_by_consume）                  │
│    · 对每个绑定：                                                 │
│      1. 从 InputSnapshot 提取原始标量                             │
│      2. 通过 contribution 投射为 InputValue                      │
│      3. 应用绑定级修改器                                          │
│      4. 评估绑定级触发器组                                        │
│    · 写入 InputBindingState                                     │
├─────────────────────────────────────────────────────────────────┤
│  accumulateActions()                                            │
│    · 遍历通过触发器的 InputBindingState                           │
│    · 按 ActionId 累加 modified_value → ActionState.value         │
│    · Bool 取 OR，Axis 取加法                                     │
│    · 选择 dominant_binding（最高触发状态）                         │
├─────────────────────────────────────────────────────────────────┤
│  evaluateActionLayer()                                          │
│    · 对每个有累加值的动作：                                        │
│      1. 应用动作级修改器（如 Normalize2D）                        │
│      2. 评估动作级触发器组（如 Hold）                              │
│    · 更新 ActionState.trigger_state                              │
├─────────────────────────────────────────────────────────────────┤
│  finalizeEvents()                                               │
│    · 根据 prev/current 状态计算事件标志位：                        │
│      Started   = !prev_active && active                         │
│      Completed = prev_active && !active                         │
│      Ongoing   = active && trigger_state == Ongoing             │
│      Triggered = trigger_state == Triggered                     │
│      Canceled  = was_ongoing && !active                         │
│    · 更新 held_seconds                                          │
└─────────────────────────────────────────────────────────────────┘
```

### 数据存储

ActionMapper 内部使用 7 个 `SparseSet<uint32_t, V, 1>`：

| SparseSet | 键 | 值 | 说明 |
|-----------|----|----|------|
| action_states_ | ActionId | ActionState | 当前帧动作状态 |
| prev_action_states_ | ActionId | ActionState | 上一帧快照 |
| binding_states_ | BindingId | InputBindingState | 绑定中间结果 |
| prev_binding_states_ | BindingId | InputBindingState | 上一帧绑定状态 |
| injected_triggered_ | ActionId | InputValue | 单帧触发注入 |
| injected_values_ | ActionId | InputValue | 持久值注入 |
| action_trigger_rts_ | ActionId | vector\<TriggerRuntimeState\> | 动作级触发器计时器 |

所有 SparseSet 的 `clear()` 保留已分配容量，保证每帧零堆分配（稳态下）。
