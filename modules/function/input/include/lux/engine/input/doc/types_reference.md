# Input System — 类型参考手册

本文档描述 `lux::input` 输入系统中所有核心类型的作用、字段含义和设计意图。

> 头文件路径前缀: `lux/engine/input/`
> 命名空间: `lux::input`

---

## 目录

1. [ActionId — 动作标识](#actionid)
2. [InputValue — 输入值](#inputvalue)
3. [InputValueType — 值类型枚举](#inputvaluetype)
4. [InputActionDesc — 动作描述](#inputactiondesc)
5. [InputActionRegistry — 动作注册表](#inputactionregistry)
6. [PhysicalInput — 物理输入源](#physicalinput)
7. [ActionBinding — 绑定关系](#actionbinding)
8. [ActionMap — 绑定容器](#actionmap)
9. [InputContext — 输入上下文](#inputcontext)
10. [InputContextStack — 上下文栈](#inputcontextstack)
11. [ActionMapper — 核心处理器](#actionmapper)
12. [ActionState — 动作状态快照](#actionstate)
13. [ActionEvent — 事件标志位](#actionevent)
14. [TriggerDesc — 触发器描述](#triggerdesc)
15. [TriggerKind — 触发器类型](#triggerkind)
16. [TriggerState — 触发器状态](#triggerstate)
17. [TriggerLogicType — 触发器组合逻辑](#triggerlogictype)
18. [TriggerRuntimeState — 触发器运行时状态](#triggerruntimestate)
19. [ModifierSpec — 修改器描述](#modifierspec)
20. [ModifierKind — 修改器类型](#modifierkind)
21. [InputBindingState — 绑定运行时状态](#inputbindingstate)
22. [BindingId — 绑定标识](#bindingid)
23. [IActionDispatcher — 事件分发接口](#iactiondispatcher)

---

<a id="actionid"></a>
## 1. ActionId

**头文件**: `ActionId.hpp`

```cpp
using ActionId = uint32_t;
inline constexpr ActionId InvalidActionId = 0;
```

运行时分配的动作标识符。由 `InputActionRegistry` 通过内部 `OffsetAutoSparseSet` 自动分配，
从 1 开始递增。0 保留作为无效/未初始化的哨兵值。

**设计要点**:
- 不再使用编译期 FNV-1a 哈希，而是运行时小整数，可作为 SparseSet 的紧凑键。
- 所有 ActionId 必须通过 `InputActionRegistry::registerAction()` 获取。
- `InvalidActionId = 0` 可安全用于默认初始化和"未绑定"判断。

---

<a id="inputvalue"></a>
## 2. InputValue

**头文件**: `InputValue.hpp`

输入系统的通用值容器，是一个**带标签的联合体**（tagged union），可承载四种值类型：

| 工厂方法 | 类型 | 描述 |
|---------|------|------|
| `makeBool(bool)` | Bool | 布尔值（按键是否按下） |
| `makeAxis1D(float)` | Axis1D | 单轴（鼠标滚轮、单按键方向等） |
| `makeAxis2D(float, float)` | Axis2D | 双轴（WASD 合成移动、鼠标位移等） |
| `makeAxis3D(float, float, float)` | Axis3D | 三轴（3D 空间输入） |

**访问器**:
- `asBool()` — 返回 `bool`（|x| > 1e-6）
- `as1D()` — 返回 `float`（x 分量）
- `as2D()` — 返回 `Float2{x, y}`
- `as3D()` — 返回 `Float3{x, y, z}`
- `nearlyZero(eps)` — 值是否接近零

**辅助函数**:
- `zeroOf(InputValueType)` — 创建指定类型的零值
- `accumulateValue(a, b, target_type)` — 类型安全的累加（Bool 取 OR，Axis 取加法）

---

<a id="inputvaluetype"></a>
## 3. InputValueType

**头文件**: `InputValue.hpp`

```cpp
enum class InputValueType : uint8_t { Bool, Axis1D, Axis2D, Axis3D };
```

标识 `InputValue` 的内部数据类型。每个已注册的动作都有固定的 `value_type`，
累加过程保证类型一致性。

---

<a id="inputactiondesc"></a>
## 4. InputActionDesc

**头文件**: `InputActionDesc.hpp`

动作的**静态描述**，定义了一个动作的身份和处理链。

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | `ActionId` | 运行时由注册表分配，初始传 0 |
| `name` | `std::string` | 唯一名称，如 `"player.jump"` |
| `value_type` | `InputValueType` | 产出的值类型 |
| `consume_input` | `bool` | 是否消耗输入（阻止低优先级上下文）|
| `action_modifiers` | `vector<ModifierSpec>` | 动作级修改器（累加后应用） |
| `action_triggers` | `vector<TriggerDesc>` | 动作级触发器（累加后评估） |

**与 ActionBinding 的关系**: 一个 ActionDesc 可有多个 Binding。各 Binding 的贡献先经
各自的绑定级修改器/触发器，再累加到 ActionDesc 上进行动作级处理。

---

<a id="inputactionregistry"></a>
## 5. InputActionRegistry

**头文件**: `InputActionRegistry.hpp`

动作注册表。内部使用 `OffsetAutoSparseSet<uint32_t, InputActionDesc, 1>` 自动分配 ActionId，
以及 `unordered_map<string, ActionId>` 进行名称→ID 的冷路径查找。

**核心方法**:

| 方法 | 说明 |
|------|------|
| `registerAction(desc)` | 注册动作，返回分配的 ActionId。同名重复注册为幂等操作 |
| `unregisterAction(id)` | 取消注册 |
| `find(id)` | 按 ID 查询，返回 `const InputActionDesc*`（未找到返回 nullptr） |
| `findByName(name)` | 按名称查询（冷路径），返回 ActionId |
| `keys()` / `values()` | 遍历所有已注册动作 |

---

<a id="physicalinput"></a>
## 6. PhysicalInput

**头文件**: `PhysicalInput.hpp`

```cpp
using PhysicalInput = std::variant<KeyInput, MouseButtonInput, MouseAxisInput>;
```

物理输入源的类型安全变体。当前支持键盘和鼠标，未来将扩展手柄支持。

| 类型 | 字段 | 说明 |
|------|------|------|
| `KeyInput` | `key: EKey` | 键盘按键 |
| `MouseButtonInput` | `button: EMouseButton` | 鼠标按键 |
| `MouseAxisInput` | `axis: Axis`, `scale: float` | 鼠标轴（DeltaX/DeltaY/ScrollX/ScrollY） |

`MouseAxisInput::scale` 是设备层的原始缩放，在绑定修改器之前生效。
用户层灵敏度应使用 `ModifierKind::Scale`。

---

<a id="actionbinding"></a>
## 7. ActionBinding

**头文件**: `ActionBinding.hpp`

Action Map 中的一条绑定关系：将一个 PhysicalInput 映射到一个 ActionId。

| 字段 | 类型 | 说明 |
|------|------|------|
| `binding_id` | `BindingId` | 全局唯一 ID（自动分配） |
| `action` | `ActionId` | 目标动作 |
| `source` | `PhysicalInput` | 物理输入源 |
| `contribution` | `InputValue` | 贡献向量（如 W 键 → Axis2D(0, 1)） |
| `binding_modifiers` | `vector<ModifierSpec>` | 绑定级修改器链 |
| `binding_triggers` | `vector<TriggerDesc>` | 绑定级触发器（可选，默认 Down） |

**评估流程**:
1. 从 PhysicalInput 提取原始标量
2. 通过 `contribution` 投射为 InputValue
3. 应用 `binding_modifiers`
4. 评估 `binding_triggers`（若通过则参与累加）

---

<a id="actionmap"></a>
## 8. ActionMap

**头文件**: `ActionMap.hpp`

ActionBinding 的容器，提供流式构建 API。

| 方法 | 说明 |
|------|------|
| `bindKey(id, key, ...)` | 绑定键盘按键 |
| `bindMouseButton(id, button, ...)` | 绑定鼠标按键 |
| `bindMouseAxis(id, axis, ...)` | 绑定鼠标轴 |
| `unbindAll(id)` | 移除某动作的所有绑定 |
| `clear()` | 清空所有绑定 |
| `bindings()` | 返回 `span<ActionBinding>` |

所有 `bind*` 方法返回 `ActionMap&`，支持链式调用。

---

<a id="inputcontext"></a>
## 9. InputContext

**头文件**: `InputContext.hpp`

输入上下文，拥有一个 ActionMap 并声明是否消耗键盘/鼠标输入。

| 属性 | 说明 |
|------|------|
| `name` | 上下文名称（如 `"Gameplay"`, `"UI"`, `"Editor"`） |
| `enabled` | 是否激活（禁用时被跳过） |
| `priority` | 优先级（越高越先评估） |
| `consumesKeyboard` | 消耗键盘输入（阻止低优先级上下文） |
| `consumesMouse` | 消耗鼠标输入 |

**三层输入阻塞语义**:

| 层级 | 机制 | 粒度 |
|------|------|------|
| Layer 1 | UI 捕获（`InputSnapshot::keyboard_captured_by_ui`） | 全局 |
| Layer 2 | 上下文消耗（`consumesKeyboard` / `consumesMouse`） | 按输入类别 |
| Layer 3 | 动作消耗（`InputActionDesc::consume_input`） | 按动作（预留） |

---

<a id="inputcontextstack"></a>
## 10. InputContextStack

**头文件**: `InputContextStack.hpp`

InputContext 指针的非拥有式栈。按 `priority` 升序排列，从最高优先级（栈尾）开始处理。

| 方法 | 说明 |
|------|------|
| `push(ctx)` | 按优先级插入（重复推入被忽略） |
| `pop(ctx)` | 移除指定上下文 |
| `pop()` | 弹出最高优先级上下文 |
| `active()` | 返回所有上下文（低→高排列） |

---

<a id="actionmapper"></a>
## 11. ActionMapper

**头文件**: `ActionMapper.hpp`

输入系统的核心处理器。每帧执行五阶段管线：

```
beginFrame → evaluateBindings → accumulateActions → evaluateActionLayer → finalizeEvents
```

通常通过 `update()` 一次调用完成所有阶段。

**查询 API**（在 `update()` 后调用）:

| 方法 | 说明 |
|------|------|
| `triggered(id)` | 本帧触发条件完全满足 |
| `ongoing(id)` | 触发条件正在进行中（如 Hold 计时中） |
| `canceled(id)` | 本帧中断（从活跃→不活跃但触发未完成） |
| `active(id)` | 当前活跃（`down` 或触发状态非 None） |
| `getValue(id)` | 获取多维值（`InputValue`） |
| `state(id)` | 获取完整 ActionState |

**注入 API**（用于程序化/回放输入）:

| 方法 | 说明 |
|------|------|
| `injectTriggered(id, value)` | 注入一帧触发事件 |
| `injectValue(id, value)` | 注入持久值 |

**内部数据结构**: 7 个 `SparseSet<uint32_t, V, 1>`，保证 O(1) 查询，
`clear()` 保留容量（每帧零堆分配）。

---

<a id="actionstate"></a>
## 12. ActionState

**头文件**: `ActionState.hpp`

单个动作在当前帧的完整状态快照。

| 字段 | 类型 | 说明 |
|------|------|------|
| `value` / `prev_value` | `InputValue` | 当前帧和上一帧的累加值 |
| `down` / `prev_down` | `bool` | 是否有物理输入活跃 |
| `held_seconds` | `float` | 持续按压的秒数 |
| `events` | `uint8_t` | ActionEvent 标志位 |
| `trigger_state` | `TriggerState` | 当前触发器评估结果 |
| `dominant_binding` | `BindingId` | 贡献最高优先级触发状态的绑定 |

**查询辅助方法**: `started()`, `ongoing()`, `triggered()`, `completed()`, `canceled()`, `active()`

---

<a id="actionevent"></a>
## 13. ActionEvent

**头文件**: `ActionState.hpp`

```cpp
enum ActionEvent : uint8_t {
    ActionEvent_None      = 0,
    ActionEvent_Started   = 1 << 0,   // 本帧变为活跃
    ActionEvent_Ongoing   = 1 << 1,   // 活跃但触发未完成
    ActionEvent_Triggered = 1 << 2,   // 触发条件完全满足
    ActionEvent_Completed = 1 << 3,   // 从活跃→不活跃
    ActionEvent_Canceled  = 1 << 4,   // 在触发前中断
};
```

这些事件在 `finalizeEvents()` 阶段根据当前/前帧状态自动计算。
游戏逻辑应主要查询 `events` 字段（通过 `started()`、`triggered()` 等辅助方法）。

---

<a id="triggerdesc"></a>
## 14. TriggerDesc

**头文件**: `TriggerDesc.hpp`

触发器的声明式描述。

| 字段 | 说明 |
|------|------|
| `kind` | 触发器类型（Down/Pressed/Hold 等） |
| `logic` | 组合逻辑（Explicit/Implicit/Blocker） |
| `actuation_threshold` | 轴幅度视为"激活"的阈值（默认 0.5） |
| `hold_time_seconds` | Hold/HoldAndRelease 的持续时间 |
| `tap_time_seconds` | Tap 的最大允许时长（默认 0.2s） |
| `pulse_interval` | Pulse 的重复间隔 |
| `chord_action` | ChordAction 所需的伴随动作 ID |

---

<a id="triggerkind"></a>
## 15. TriggerKind

**头文件**: `TriggerDesc.hpp`

| 枚举值 | 说明 |
|--------|------|
| `Down` | 值超过阈值时持续触发 |
| `Pressed` | 上升沿（本帧刚超过阈值） |
| `Released` | 下降沿（本帧刚降至阈值以下） |
| `Hold` | 持续按压 ≥ hold_time_seconds 后触发 |
| `HoldAndRelease` | 持续按压 ≥ hold_time 后松开时触发 |
| `Tap` | 在 tap_time 内完成按下+松开 |
| `Pulse` | 按住期间以 pulse_interval 间隔重复触发 |
| `ChordAction` | 另一个动作同时活跃时触发 |
| `Combo` | 序列组合（尚未实现） |

---

<a id="triggerstate"></a>
## 16. TriggerState

**头文件**: `TriggerDesc.hpp`

```cpp
enum class TriggerState : uint8_t { None, Ongoing, Triggered };
```

| 值 | 含义 |
|----|------|
| `None` | 本帧无关 |
| `Ongoing` | 条件部分满足（如 Hold 计时中） |
| `Triggered` | 条件完全满足 |

---

<a id="triggerlogictype"></a>
## 17. TriggerLogicType

**头文件**: `TriggerDesc.hpp`

```cpp
enum class TriggerLogicType : uint8_t { Explicit, Implicit, Blocker };
```

控制多个触发器如何组合：

| 类型 | 规则 |
|------|------|
| `Explicit` | **任一** Explicit 触发器通过即可 |
| `Implicit` | **所有** Implicit 触发器必须同时通过 |
| `Blocker` | **任一** Blocker 触发则整个动作被阻止 |

遵循 UE5 Enhanced Input 的组合语义。

---

<a id="triggerruntimestate"></a>
## 18. TriggerRuntimeState

**头文件**: `TriggerDesc.hpp`

每个 TriggerDesc 实例的可变运行时状态，跨帧持久化用于计时器。

| 字段 | 说明 |
|------|------|
| `elapsed` | 通用计时器（Hold/Tap/Pulse） |
| `tap_count` | Combo 序列中的计数 |
| `actuated` | 上一帧是否处于激活状态 |

---

<a id="modifierspec"></a>
## 19. ModifierSpec

**头文件**: `InputModifier.hpp`

修改器的声明式描述。

```cpp
struct ModifierSpec {
    ModifierKind kind;
    float x, y, z;   // 参数含义取决于 kind
};
```

---

<a id="modifierkind"></a>
## 20. ModifierKind

**头文件**: `InputModifier.hpp`

| 枚举值 | 参数 | 说明 |
|--------|------|------|
| `None` | — | 无操作 |
| `Scale` | x, y, z | 按分量缩放 |
| `NegateX` | — | 翻转 X 轴 |
| `NegateY` | — | 翻转 Y 轴 |
| `NegateZ` | — | 翻转 Z 轴 |
| `DeadZone` | x=阈值 | 幅度低于阈值时归零 |
| `Normalize2D` | — | 将 XY 向量归一化到单位长度 |
| `Clamp1D` | x=min, y=max | 将 Axis1D 值夹到 [x, y] 范围 |

修改器可以链式应用。绑定级修改器在 `contribution` 投影后应用，
动作级修改器在所有绑定累加后应用。

---

<a id="inputbindingstate"></a>
## 21. InputBindingState

**头文件**: `InputBindingState.hpp`

单个绑定在一帧内的中间运行时状态。在 `evaluateBindings()` 中填充，在 `accumulateActions()` 中消费。

| 字段 | 说明 |
|------|------|
| `binding_id` / `action_id` | 标识 |
| `raw_value` / `modified_value` | 原始值和修改后的值 |
| `raw_pressed_edge` / `raw_released_edge` | 边沿检测 |
| `down` / `prev_down` / `held_seconds` | 按压状态和持续时间 |
| `trigger_state` / `prev_trigger_state` | 触发器状态 |
| `blocked_by_ui` / `blocked_by_consume` | 是否被 UI 或消耗标志阻塞 |

---

<a id="bindingid"></a>
## 22. BindingId

**头文件**: `InputBindingState.hpp`

```cpp
using BindingId = uint32_t;
inline constexpr BindingId InvalidBindingId = 0;
```

全局唯一的绑定标识符，由 `BindingIdAllocator::next()` 单调递增分配。
不同 ActionMap 中的绑定具有不同的 BindingId，保证全局唯一性。

---

<a id="iactiondispatcher"></a>
## 23. IActionDispatcher / ActionMapperDispatcher

**头文件**: `ActionDispatcher.hpp`

事件分发接口，用于解耦 UI/游戏逻辑。

```cpp
class IActionDispatcher {
    virtual void dispatchTriggered(ActionId id, const InputValue& value) = 0;
    virtual void dispatchValue(ActionId id, const InputValue& value) = 0;
};
```

`ActionMapperDispatcher` 是具体实现，将调用转发到 `ActionMapper::injectTriggered()` / `injectValue()`。

---

## 架构图

```
InputSnapshot (平台层)
    │
    ▼
ActionMapper::update()
    │
    ├─ beginFrame()             保存前帧状态，应用注入
    ├─ evaluateBindings()       遍历 ContextStack → ActionMap → Binding
    │   └─ extractRaw()         从 InputSnapshot 提取原始值
    │   └─ projectContribution()投射为 InputValue
    │   └─ applyModifiers()     绑定级修改器
    │   └─ evaluateTriggerGroup() 绑定级触发器
    ├─ accumulateActions()      合并通过触发器的绑定 → ActionState.value
    ├─ evaluateActionLayer()    动作级修改器 + 动作级触发器
    └─ finalizeEvents()         计算 Started/Ongoing/Triggered/Completed/Canceled
    │
    ▼
查询: triggered(), getValue(), state() ...
```
