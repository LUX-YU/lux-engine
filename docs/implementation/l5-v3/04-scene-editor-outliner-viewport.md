# L5 SceneEditor、SceneOutliner、Viewport 与 Selection 设计

Status: **Normative L5 v3 Scene UI Design — Lux UI backend-isolated**

Implementation gate: **R0 + A + U1**. Scene UI must use the Lux UI boundary from Wave U.

---

## 1. 核心修正：Scene 不等于 Entity Hierarchy

Scene 的基本事实是：

```text
Scene = World + authoritative Registry + Simulation + selected SceneSystems
```

“实体形成树”不是 Scene 的普遍定义。

`Parent` 只是一个可选 Component/semantic relationship。因此 UI 应使用：

```text
SceneOutliner
```

而不是把 `HierarchyPanel` 当作唯一模型。

---

## 2. 层级

```text
L1
├─ Registry / Entity
├─ ComponentSchema
├─ Parent + reparent/detach (when hierarchy package used)
├─ Transform etc.
└─ Simulation systems

L3
├─ Scene
├─ SceneMetaManager
├─ SceneDescription / Builder
└─ optional Render SceneSystem

L5
├─ SceneEditor
├─ SceneOutliner
├─ EntityInspector
├─ selection control state
└─ viewport interaction

Product/Application composition
└─ EditorApplication or generated product owns render/window/frame capability
```

---

## 3. SceneEditor 是窗口级 UI 对象

```cpp
class SceneEditor final : public ui::Pane
{
public:
    SceneEditor(EditorContext& context, EditorSceneHandle scene);
    void draw(ui::Frame& frame) override; // exact Pane signature follows Wave U

private:
    EditorContext& context_;
    EditorSceneHandle scene_;

    ui::ViewportElement viewport_;
    SceneViewportState viewport_state_;
};
```

不要恢复 legacy 巨型 `EditorScene`，它同时拥有 World/Render/Play/Save/Cook/Selection/Camera/Streaming 等过多职责。

新的 SceneEditor 只作为 UI 与 L3 Scene/application composition capabilities 的组合点。

---

## 4. SceneOutliner 是独立窗口或 SceneEditor 子区域

UX 可以决定 Outliner 是否是独立 `ui::Pane`。架构上它消费同一个 scene/selection state。

### 4.1 Projection

Outliner 应允许：

```text
FlatEntities
ParentHierarchy
SpatialPartition
WorldObject grouping
Plugin-defined projection
```

第一版至少：

```text
Flat
Hierarchy-if-available
```

SceneOutliner 的 row density、selection/highlight、context-menu、rename/delete/create interaction MAY intentionally follow Legacy visual/interaction language, but the projection/selection/ownership model remains the current architecture。All drawing goes through Lux UI public API。

---

## 5. Projection 决策

不要通过“Registry 里碰巧存在 Parent”就猜整个 Scene 应显示 hierarchy。

更稳健的来源：

```text
Scene/Editor presentation policy
or active system/capability metadata
```

例如：

```cpp
enum class EOutlinerProjection
{
    FLAT,
    PARENT_HIERARCHY
};
```

未来可由 Scene template / project preference / Editor command切换。

Hierarchy mode 要求 hierarchy semantics 可用；否则按钮 disabled/fallback flat。

---

## 6. Flat Outliner

```text
Scene
├─ Entity #12
├─ Entity #13
├─ Entity #17
└─ Entity #30
```

不要求所有 entity 都有 Transform/Parent/Name。

排序建议第一版 deterministic：

```text
entity stable numeric ordering
```

不要为了 UI 立刻引入 Name component。

---

## 7. Hierarchy Outliner

如果 Parent semantics 可用：

```text
Root A
├─ Child B
│  └─ Child C
└─ Child D
Root E
```

拓扑 query 可以来自 authoritative HierarchyIndex/semantics；mutation 必须调用：

```cpp
reparent(registry, child, parent)
detach(registry, child)
```

禁止直接：

```cpp
registry.patch<Parent>(..., [](Parent& p){ p.entity = ...; });
```

因为 canonical functions 负责 self-parent/cycle/invalid parent invariants。

---


## 8. Selection：v1 由 EditorApplication 拥有，Context 引用

Selection 是 Editor control-plane state，不是 ECS component。v1 只支持一个 active Scene editing target。

Live Scene identity MUST 使用 L5 generational `EditorSceneHandle`，不是 Scene AssetId，也不新增 L3 SceneId：

```cpp
struct EditorSceneHandle final
{
    std::uint32_t slot{};
    std::uint32_t generation{};
};

struct EntitySelection final
{
    EditorSceneHandle scene;
    simulation::ecs::Entity entity;
};
```

ownership：

```text
EditorApplication
    └─ EditorSelection
          │
          └─ EditorContext references it
```

`SceneAsset AssetId` 仅可作为 scene record 的持久化/source metadata；同一 Asset 可对应多个 live Scene instance，reload 后 AssetId 也不会变化，因此不能用于 stale-selection 防护。L3 `Scene` 当前没有独立 runtime identity 需求，不得仅为 Editor 引入 `SceneId`。

Scene destroy/reload/rebind MUST bump/invalidate `EditorSceneHandle` generation and clear selection referencing the old handle。

写入者：

- SceneOutliner click；
- SceneViewport picking；
- create/delete 后的 canonical selection update。

观察者：

- EntityInspector；
- Outliner highlight/reveal；
- Viewport highlight/gizmo；
- frame-selected command。

### 8.1 Signal

Selection 是适合 LuxObject signal 的 control-plane 事件：

```text
selection.changed
    -> EntityInspector refresh target
    -> SceneOutliner reveal/highlight
    -> SceneViewport highlight
```

payload 只含 stable identity，不传 `Registry*`、component pointer、`RefField*`。

### 8.2 Future multi-scene

未来多 Scene window 可以扩展 slot table/focus routing，但 public selection identity 继续使用 `EditorSceneHandle + Entity`，不需要改变为 AssetId/L3 SceneId。v1 不预建 SelectionRegistry/Manager。

## 9. EntityInspector 与 Outliner 解耦

```text
SceneOutliner -----┐
                   ├-> Selection -> EntityInspector
SceneViewport -----┘
```

不存在：

```text
Outliner.setInspector(...)
Viewport.setOutliner(...)
Inspector.setViewport(...)
```

它们通过 shared control state 组合。

组件实际编辑仍直接对 Registry typed patch，不通过 Selection signal。

---

## 10. SceneViewport

当前低层 `ViewportElement` 已能提供：

```text
content size/origin
local pointer
hover/focus
resize
mouse clicks
drag/drop
```

SceneEditor 在自己的 draw 中使用它。`ViewportElement` is part of the Lux UI public seam; it MUST NOT expose Dear ImGui geometry/texture/ID types。

### 10.1 Render ownership

SceneEditor 不创建 Vulkan device/RenderRuntime/thread。

```text
EditorApplication / generated product composition owns RenderRuntime/device/frame lifecycle
L3 optional RenderSystem owns Scene render integration
L5 SceneEditor owns viewport UI state
```

application composition/Render capability 向 SceneEditor 提供可呈现 texture/target handle；UI-facing presentation uses a Lux-owned opaque handle (`ui::TextureHandle` or approved equivalent), never `ImTextureID`。

---

## 11. Picking

```text
Viewport click
    ↓ screen-local coordinates
Scene picking tool/capability
    ↓ Entity/EditorObject identity
Selection.select(...)
```

Picking 算法放置取决于实际 renderer/scene capability；不要把 generic UI `ViewportElement` 变成 Scene-aware。

---

## 12. Gizmo

Gizmo 直接编辑 Transform component：

```text
Viewport gizmo
 -> typed Transform mutation
 -> registry.patch<Transform3D>
 -> TransformSystem dirty
```

与 Inspector 使用同一 authoritative mutation semantics。

Undo gesture 与 Inspector 类似：drag begin capture old，drag end record one operation。

---

## 13. Asset Drop

AssetBrowser drag `AssetId` 到 SceneViewport：

```text
AssetId
 -> SceneEditor recognizes asset type via Context resource/tool info
 -> appropriate spawn/import action
 -> L2 async if loading/cooking needed
 -> main-thread safe ECS mutation
 -> select spawned entity
```

不要在 `ViewportElement` 内认识 Model/Material。

---

## 14. Create / Delete Entity

SceneOutliner command：

```text
editor.scene.create_entity
editor.scene.delete_entity
editor.scene.reparent
```

真正 mutation 可以由 SceneEditor/scene editing helper 执行。

删除需要：

- validate entity；
- handle selected entity；
- hierarchy semantics；
- future persistent object mapping；
- undo capture。

不要让 UI row 直接 `registry.destroy()` 后不处理相关 semantics。

第一版如仅 transient scene editing，可有非常窄的 typed command，但必须记录后续 persistence boundary。

---

## 15. Entity Name

Legacy 有 NameComponent，但 current active architecture 没有对应 canonical component。

不要为了 Outliner 美观立即恢复 NameComponent。

第一版：

```text
Entity #<stable bits/id>
```

未来先回答：

```text
Name 是 durable authored World fact？
还是 editor-only label？
是否需要 runtime gameplay access？
```

再决定 owner。

---

## 16. Scene Description vs Entity Editing

当前 `SceneDescription` 主要描述 SceneSystem/Simulation composition，不应被误当作 Entity document。

因此：

```text
Scene Settings / System composition UI
    -> SceneMetaManager + SceneDescriptionBuilder

Entity Inspector / Outliner
    -> active Scene authoritative Registry (working state)
```

实体持久化/cook 到 World/Scene storage 是独立 future workflow；不要创建泛型 `SceneDocument` 只是为了包住 Builder。

---

## 17. Scene Settings

可以有 SceneEditor 的 settings 区域/独立 window：

```text
available SimulationSystems
available SceneSystems
selected configurations
capabilities/providers
```

这里可以大量使用 generated config UI binding，因为配置 struct 同样是 static known types。

提交结构变更：

```text
edit draft/config
 -> SceneDescriptionBuilder authoritative validation
 -> create/recreate scene at explicit boundary
```

不要直接修改 live immutable metadata graph。

---

## 18. Play/Edit

Legacy 把 Play、Cook、SceneRuntime 全塞入 EditorScene。

新版不在 SceneEditor 第一阶段设计这些。

未来：

```text
SceneEditor command Play
 -> Toolset/project build/cook capability
 -> create separate runtime Scene/product
```

不要让 edit Registry 直接变成 game runtime state，仅因为 UI 点击 Play。

---

## 19. 测试

### Outliner

- Flat scene no Parent；
- hierarchy scene；
- reparent cycle rejection；
- detach；
- delete selected entity；
- projection switch preserves selection。

### Selection

- Outliner -> Inspector；
- Viewport -> Inspector/Outliner；
- entity destroyed clears/invalidates selection；
- scene switch stale selection impossible。

### Inspector integration

- selected entity components update；
- hierarchy presentation does not affect Inspector semantics。

### Viewport

- resize；
- focus/context commands；
- pick；
- gizmo patch triggers Transform system；
- asset drop passes stable AssetId。

---

## 20. 禁止项

```text
No assumption Scene == hierarchy tree.
No giant EditorScene owning every subsystem.
No raw Parent.entity editing.
No UI-owned RenderRuntime/device.
No ViewportElement awareness of Scene/Material/Model.
No automatic NameComponent resurrection for display.
No Editor selection as ECS component.
No SceneDescription treated as generic entity document.
No direct Dear ImGui dependency in SceneEditor/SceneOutliner feature code.
No backend texture/ID types in ViewportElement public contracts.
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md` and `10-lux-ui-foundation-and-legacy-visual-parity.md`.
