# L5 EntityInspector：反射 + Codegen 的零解释热路径设计

Status: **Normative L5 v3 Design — Lux UI backend-isolated**

---

## 1. 目标

EntityInspector 是 Wave U0/U1 之后第一个适合落地的真实 UI vertical slice，因为它可以同时验证：

- LUX annotation / generator；
- typed Lux UI binding；
- ECS `ComponentSchema::editor_visible`；
- direct Registry mutation；
- EnTT reactive systems；
- VFS asset picker；
- undo gesture；
- plugin-generated UI contribution；
- EditorContext。

性能目标不是简单的“Reflection 很快”，而是：

> 第一方/插件的已知 component UI 不在每帧解释 runtime reflection metadata；generator 直接产生 typed C++，并只调用 Lux UI public API。

---

## 2. 分层

```text
L0 Meta/TypeInfo
    LUX_COMPONENT / LUX_MEMBER parser annotations
    generator infrastructure

L0 UI
    Lux UI public value/edit/property primitives
    private Dear ImGui backend implementation

L0 Resource
    AssetId / AssetVfs

L1 ECS
    Registry
    ComponentSchemaSet
    ComponentSchema::editor_visible
    concrete Component types

L1 Simulation
    on_update reactive consumers

L5 Editor
    EntityInspector
    generated component editor binding table
    EditorValueBinding<T>
    semantic editor overrides
    undo integration
```

---

## 3. 当前 annotation 基础

现有 `LUX_MEMBER` 已约定：

```text
display_name
tooltip
min
max
color
readonly
labels
```

这些 annotation 在普通 C++ 编译时不产生 runtime object，generator parse 时读取。

建议 L5 扩展（必须经 generator validation）：

```text
widget=default|drag|slider|input|color|asset|enum|readonly
speed=<number>
format=<printf-like stable token if supported>
step=<number>
asset_type=<stable asset type id/token>
semantic_editor=<stable generated semantic editor id>   # 谨慎使用
```

不要无限增加 UI-specific annotation。若一个 component 需要复杂专属 UI，应提供专用 generated/custom binding，而不是把整个 UI DSL 塞进 annotation。

---

## 4. Generator 输出

每个 editor-visible Component 可以生成一个 binding：

```cpp
struct ComponentEditorBinding final
{
    cxx::TypeToken component_type;
    simulation::ecs::ComponentSchemaId schema;
    std::string_view display_name;

    bool (*draw)(
        simulation::ecs::Registry&,
        simulation::ecs::Entity,
        InspectorContext&
    ) noexcept;
};
```

module side 生成：

```cpp
inline constexpr ComponentEditorBinding kTransform3DBinding{...};
```

`InspectorContext`/binding call path MUST provide a Lux UI frame/edit capability。Generated source MUST NOT include Dear ImGui headers or use ImGui types。

Editor composition 收集这些 binding，构建 immutable table。

---

## 5. 热路径

以 Transform3D 为例，generator 可产生：

```cpp
bool drawTransform3D(
    Registry& registry,
    Entity entity,
    InspectorContext& context
) noexcept
{
    auto* component = registry.try_get<Transform3D>(entity);
    if (component == nullptr)
        return false;

    bool changed = false;

    changed |= drawValue(
        "Translation",
        component->translation,
        GeneratedFieldSpec{...},
        context
    );

    changed |= drawValue(
        "Rotation",
        component->rotation,
        GeneratedFieldSpec{...},
        context
    );

    changed |= drawValue(
        "Scale",
        component->scale,
        GeneratedFieldSpec{...},
        context
    );

    if (changed)
        registry.patch<Transform3D>(entity);

    return changed;
}
```

每帧没有：

```text
ReflectionRegistry lookup
RefClass traversal
RefField offset calculation
runtime type switch by string/hash
std::function callback graph
LuxObject component-change signal
```

只有 typed function pointer dispatch 一次，然后内部全部是直接 C++。

---

## 6. EditorValueBinding<T>

共享“类型如何画”应通过模板/overload 明确定义：

```cpp
template<class T>
struct EditorValueBinding;
```

首批：

```text
bool
int32/uint32/int64/uint64
float/double
std::string
Eigen::Vector2f/3f/4f
Eigen::Vector2d/3d/4d
Eigen::Quaternionf/d
AssetId
selected supported enums
```

### 6.1 double

不要为了 backend convenience 把 double narrow 到 float。

Generated/public binding MUST call a Lux-owned scalar edit API that preserves the native type，例如概念上：

```cpp
frame.editScalar<double>(label, value, spec);
```

Dear ImGui backend implementation MAY map this to its scalar APIs and `ImGuiDataType_Double`, but that vocabulary remains private to `modules/function/ui` and MUST NOT appear in generated/Editor/plugin source.

### 6.2 Vector

默认：

```text
Vector2 -> 2 scalar editor
Vector3 -> 3 scalar editor
Vector4 -> 4 scalar editor
```

`labels=R,G,B` 等在生成时变成 constexpr label array。

### 6.3 Quaternion

Quaternion 的 Editor 表现是 UI policy，不应改变 Component 表示。

可选：

```text
default -> Euler degrees editor + normalize on commit
widget=raw -> xyzw
```

需要明确 mutation/normalization 语义，避免每帧累计误差。

---

## 7. Annotation 选择 Widget

一个类型可以有 default binding，但 annotation 覆盖 presentation。

```cpp
LUX_MEMBER(widget=slider, min=-100.0, max=100.0)
double value;
```

Generator 直接产生固定调用，不在 runtime parse `"slider"`。

例如：

```cpp
EditorValueBinding<double>::edit(frame, "Value", component->value, GeneratedFieldSpec{...});
```

因此 annotation 的开销发生在 build/codegen，而不是 Editor hot path。

---

## 8. Component 枚举

EntityInspector 的 outer loop 可以使用 `ComponentSchemaSet`/SceneMeta 中现有 component schema 列表，筛选：

```text
schema.editor_visible == true
schema.operations.has(registry, entity) == true
```

这层是“每个 component 一次”的 type-erased discovery，不是每个 field 的热路径。

随后：

```text
schema.cpp_type -> ComponentEditorBinding table -> generated draw function
```

如果 editor-visible component 没有对应 binding，第一版建议明确显示：

```text
<Editor binding unavailable>
```

而不是 runtime reflection fallback。

这可以暴露插件/package 配置错误，而不是悄悄降级。

---

## 9. Add Component

“Add Component”必须尊重当前 Scene/Simulation 可用 component contract，而不是展示 ReflectionRegistry 中所有类型。

候选来源：

```text
SceneMetaManager / ComponentSchemaSet
+ editor_visible
+ Scene/System capability rules
```

具体 default emplace 需要 component generator 提供 typed editor construction binding，或者使用现有 canonical creation path。

不要为了 Inspector 给 `ComponentOperations` 无条件加入 `defaultEmplace()`，除非 L1 本身有第二个真实消费者。

Editor binding 可以生成：

```cpp
bool (*add_default)(Registry&, Entity) noexcept;
```

它属于 L5 generated projection，不污染 L1 runtime schema。

---

## 10. 修改传播

典型路径：

```text
EntityInspector generated binder
    ↓ direct component field write
registry.patch<T>()
    ↓
EnTT on_update<T>
    ↓
registered Simulation subsystem dirty tracking
    ↓
Simulation stable point
    ↓
derived state update
    ↓
Scene/render consumers
```

当前 TransformSystem 已证明这一模型：它连接 Transform construct/update/destroy，并将变化实体加入 dirty 集合。

因此不应再增加 Editor-specific component changed bus。

---

## 11. Semantic Field：不是所有 public field 都能直接编辑

最重要例子：`Parent`。

其字段结构很简单：

```cpp
struct Parent
{
    Entity entity;
};
```

但合法 mutation 是：

```cpp
reparent(registry, child, parent)
detach(registry, child)
```

因为它们负责 cycle/self-parent/invalid-parent 等 invariant。

所以 generator 必须支持字段/类型的 semantic override：

```text
Plain value field
    -> generated direct binder

Semantic relation/invariant-owned field
    -> custom semantic binding / readonly / hidden
```

禁止 generic Inspector 直接写 `Parent.entity`。

其他未来例子：

- Material inheritance parent；
- World partition membership；
- Script binding identity；
- Asset relationship with validation；
- domain state requiring builder/mutator。

---

## 12. AssetId Editor

AssetId binding 从 `InspectorContext`/`EditorContext` 使用统一的 product-wide `AssetVfsView`；Wave C/D 不引入 AssetIndex：

```text
current AssetId
   ↓ pathOf/id metadata
button + search + drag/drop
   ↓ type validation
write new AssetId
   ↓ registry.patch<Component>()
```

不能跨 frame 保留 component/field raw pointer。

Asset picker 打开时只保留 stable identity：

```text
Scene/Registry identity
Entity
Component type/schema
Field binding identity
```

用户真正选择时重新 resolve component。

---

## 13. Undo/Redo

实时反馈不能被 Command object 中转。

Slider drag：

```text
Item activated
    -> capture OLD typed value

Every drag frame
    -> direct write + patch

Item deactivated-after-edit
    -> capture NEW typed value
    -> append ONE undo operation
```

Undo operation 应通过相同 canonical typed mutation 写回。

若一个字段使用 semantic editor，Undo 也必须调用该 semantic mutation，而不是 memcpy/raw assignment。

---


## 14. Selection：v1 owner 已冻结

Inspector 不关心 SceneOutliner 是 flat/tree/spatial；它只消费 `EditorContext::selection()`。

v1 明确只支持一个 active Scene editing target：

```text
EditorContext
    └─ EditorSelection
         ├─ active Scene identity/handle
         └─ selected Entity/Object stable identity
```

`SceneEditor`、`SceneOutliner`、Viewport picking 写入同一个 Selection；`EntityInspector` 观察它。

```text
SceneOutliner click ──┐
                      ├─> context.selection() -> EntityInspector
Viewport pick ────────┘
```

禁止 v1 自行引入：

```text
per-pane independent selection model
SelectionManager
multi-document selection registry
selection stored as ECS component
raw Registry* in signal payload
```

如果未来需要多个 SceneEditor 同时拥有独立 selection，必须先提交独立 multi-document selection 设计；不能在本 Wave 中预埋 framework。

component hot path 仍然直接访问 active Scene Registry，不通过 Selection signal 转发 field mutation。

## 15. Plugin Components

第三方 plugin：

```text
plugin component header
    + public LUX annotations
    ↓ public generator
plugin.generated.editor.cpp
    ↓
ComponentEditorBinding
```

Editor 在 plugin contribution 安装时加入 immutable binding table。

不要求 plugin component 出现在 runtime RefClass 中才能编辑。

---

## 16. 性能

成本层次：

```text
Outer component discovery:
    O(number of registered editor-visible component schemas)
    operations.has() per schema

Per existing component:
    O(1) TypeToken -> generated binder

Per field:
    direct typed C++ + Lux UI call
```

常见实体几十个 component、几十到百余字段，真正主要成本是 UI widget/layout/backend rendering，不是 binder dispatch。

生成 code 允许 compiler inline `EditorValueBinding<T>` 与 annotation literal。

---

## 17. 测试矩阵

### Codegen

- annotation min/max/speed 正确生成 literal；
- unsupported annotation fail build；
- `readonly` 不生成 mutation；
- double binding 不 narrow；
- vector labels；
- component `editor=false` 不生成/不安装 binding（策略可选，但必须一致）。

### ECS Integration

- editing Transform3D triggers `on_update`；
- TransformSystem dirty path；
- runtime-derived `WorldTransform3D editor=false` 不显示；
- Parent semantic editor 不允许 raw write；
- deleted entity while picker open safely fails resolve。

### Plugin

- plugin generated binding appears；
- missing generated binding visible diagnostic；
- no runtime reflection dependency in editor binding target；
- generated binding target compiles without Dear ImGui include path/link dependency；
- public binding signatures contain no ImGui/imgui-node-editor types。

### Performance

- 1 entity / 100 fields hot path benchmark；
- 100 selected/visible component schema discovery benchmark；
- ensure zero per-frame heap allocation for static inspector table where practical。

---

## 18. 禁止项

```text
No RefClass/RefField hot-path requirement.
No runtime reflection fallback for normal/plugin component editing.
No generic field property bag.
No component LuxObject conversion.
No signal per field/component change.
No raw Parent relationship mutation.
No cross-frame raw component pointer retention.
No Editor-specific mutation seam added to L1 without independent domain justification.
No Dear ImGui include/type/call in generated Inspector or plugin binding code.
No backend escape hatch required to implement undo gesture status.
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md` and `10-lux-ui-foundation-and-legacy-visual-parity.md`.
