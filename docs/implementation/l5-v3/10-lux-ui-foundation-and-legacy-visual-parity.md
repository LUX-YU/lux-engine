# Lux UI Foundation、Dear ImGui Isolation 与 Legacy Visual / Interaction Parity

Status: **Normative L0 UI / L5 Editor Integration Design (v3 docset)**  
Parent documents: `00-L5-architecture-overview.md`, `07-implementation-roadmap-and-gates.md`, `08-normative-execution-contract.md`

---

## 1. 目标

Lux 需要尽快进入真实 Editor UI 开发，但不能让每个 L5 feature 直接绑定 Dear ImGui，也不能在 Dear ImGui 上重新制造一套 retained-mode GUI framework。

本设计冻结以下目标：

```text
1. modules/function/ui 是 Lux 公共 UI 边界。
2. Dear ImGui / imgui-node-editor 只存在于该 module 的 private backend implementation。
3. Editor、generated binding、plugin SDK 只看见 Lux UI 类型与 API。
4. 长生命周期语义对象采用 object-oriented 组织；leaf widget 保持 immediate-mode。
5. Legacy 可作为 selected visual / interaction / behavior reference，但不是 ownership/architecture source of truth。
6. UI styling 通过 Theme/design tokens 收敛，不在各个窗口复制 magic colors/spacing。
7. 不为理论上的第二 backend 提前创建 IUiBackend 虚接口体系。
8. 首轮 UI foundation 只实现 C/D/E/G/H/I 真实需要的 surface；不造通用 GUI framework。
```

---

## 2. 层级与依赖方向

物理 owner 冻结为：

```text
modules/function/ui/
    public Lux UI API
    Pane / UISession / CommandRouter / ViewportElement
    frame/scope/value-edit/theme contracts
    optional NodeCanvas public abstraction

    private backend implementation
        Dear ImGui
        imgui-node-editor where required
```

依赖方向：

```text
L5 Editor / generated UI / plugin contribution
                ↓
         Lux UI public API
                ↓
       modules/function/ui
                ↓ private only
      Dear ImGui / node backend
```

MUST NOT：

```text
EntityInspector -> imgui.h
AssetBrowser    -> imgui.h
SceneEditor     -> imgui.h
MaterialEditor  -> imgui.h
FlowForgeEditor -> imgui.h
plugin SDK      -> imgui.h
generated code  -> imgui.h
engine/editor/* -> imgui-node-editor.h
```

The only normal production target allowed to compile/link those backend APIs directly is the private Lux UI backend target/source set. Backend-specific tests may be colocated with that implementation.

---

## 3. Public API 不得泄漏 backend vocabulary

Lux public UI headers MUST NOT expose or transitively require：

```text
ImGuiContext
ImGuiIO
ImGuiStyle
ImGuiID
ImVec2 / ImVec4
ImTextureID
ImGuiWindowFlags
ImGuiTableFlags
ImGuiDockNode
ImDrawList
imgui_node_editor types
```

Public API 使用 Lux-owned semantic types，例如：

```text
ui::Vec2
ui::Color
ui::Id / WidgetId
ui::TextureHandle
ui::WindowOptions
ui::TableOptions
ui::DragPayload
ui::Theme
ui::ViewportElement
ui::NodeCanvas
```

这些类型不应只是把全部 ImGui flags 一比一重命名。公共 surface 只暴露 Lux Editor 真正需要、能稳定承诺的语义。

如果一个 feature 需要一个当前 public Lux UI 不能表达的 backend-specific capability，coding agent MUST first determine whether it is a real reusable UI semantic. It MUST NOT solve the problem by leaking `ImGui*` through an escape hatch.

---

## 4. Object-oriented semantic UI + immediate-mode leaf controls

Lux UI 的“面向对象感”冻结在正确层级。

### 4.1 长生命周期语义对象

这些对象有身份、生命周期、command/focus/context state，适合 object-oriented 表达：

```text
ui::UISession
ui::Pane
ui::CommandRouter
ui::ViewportElement

EntityInspector
AssetBrowser
SceneEditor
SceneOutliner when independent pane
MaterialEditor
FlowForgeEditor
```

典型形态：

```cpp
class AssetBrowser final : public ui::Pane
{
public:
    explicit AssetBrowser(EditorContext& context);
    void draw(ui::Frame& frame) override;

private:
    EditorContext& context_;
    std::string current_vpath_;
    std::string search_;
    EViewMode view_mode_{};
};
```

Exact virtual/non-virtual `Pane` signature may follow the existing package convention, but feature code MUST receive/use Lux UI frame capability rather than global ImGui calls.

### 4.2 Frame / scope 对象

Begin/End-style lifetime is represented by short-lived RAII（Resource Acquisition Is Initialization，资源获取即初始化）scope objects where this makes misuse harder：

```cpp
auto window = frame.window(window_spec);
if (!window.visible())
    return;

auto table = frame.table(table_spec);
auto disabled = frame.disabled(read_only);
auto child = frame.child(child_spec);
```

Scope object：

```text
lifetime = current draw call / nested UI scope
non-copyable where appropriate
no persistent widget tree ownership
no cross-frame backend pointer retention
```

### 4.3 Leaf widgets

Leaf controls remain immediate-mode operations：

```cpp
if (frame.button("Save"))
    save();

frame.checkbox("Enabled", enabled);
frame.editScalar("Mass", mass, spec);
frame.combo("Mode", mode, choices);
```

MUST NOT turn ordinary controls into persistent objects such as：

```text
Button save_button_;
Label title_label_;
TextBox path_box_;
TreeView asset_tree_;
```

unless a specific control has real persistent semantic state that cannot be represented by feature-local model state + immediate draw.

---

## 5. 不创建 retained-mode widget framework

The Lux UI layer is not Qt/WPF-like retained-mode UI and is not a second scene graph.

MUST NOT introduce as part of Wave U：

```text
Widget base-class tree
WidgetManager
visual tree / logical tree
layout object graph retained across frames
property binding framework
signal-per-widget framework
generic data-binding runtime
UI reflection property bag
```

Feature data remains owned by the feature/domain. The UI renders current state and emits/directly applies semantic actions through existing contracts.

This keeps the useful Dear ImGui execution model while preventing backend API leakage.

---

## 6. 不提前创建 `IUiBackend`

Current architecture has one real UI backend. Therefore v1 MUST NOT introduce：

```cpp
class IUiBackend
{
public:
    virtual bool button(...) = 0;
    virtual bool checkbox(...) = 0;
    virtual void text(...) = 0;
    // hundreds of virtual leaf calls
};
```

Backend hiding is achieved by C++ target/source/private implementation boundaries, not by virtual dispatch at every widget call.

A backend interface may only be designed after a second real backend exists or an independently approved test/backend requirement proves the need. Until then, adding an abstract backend is over-abstraction and a STOP/review condition.

---

## 7. UISession、Frame 与 backend ownership

`EditorApplication` continues to own `ui::UISession`; `EditorContext` only references it.

Conceptual ownership：

```text
EditorApplication
    └─ UISession
         ├─ Lux UI session state
         ├─ font/icon/theme state
         ├─ docking/window session state where applicable
         ├─ private ImGui context/backend bridge
         └─ per-frame Lux UI Frame capability
```

The private backend may own/borrow platform/render integration needed by Dear ImGui, but it MUST NOT move RenderRuntime/device ownership into L0 UI or L5 windows.

Frame semantics：

```text
application begins UI frame
    -> UISession creates/exposes ui::Frame
    -> panes draw through ui::Frame
    -> frame closes
    -> backend produces draw data
    -> application/render integration submits/presents under existing ownership
```

Feature code MUST NOT call backend global frame functions directly.

---

## 8. Theme / design tokens

Legacy-like visual consistency MUST be represented through an explicit Lux theme rather than repeated literal styling in panes.

Minimum semantic theme groups：

```text
Theme
├─ Typography
├─ Palette
├─ Spacing
├─ Metrics
├─ WindowStyle
├─ ToolbarStyle
├─ PropertyStyle
├─ TreeStyle
├─ TableStyle
├─ AssetTileStyle
├─ ViewportStyle
└─ GraphStyle
```

Examples of data that belong in Theme/tokens：

```text
panel padding
row height
item spacing
section spacing
selection/background/border colors
toolbar height
property label/value column behavior
tree indentation
asset thumbnail/tile metrics
graph node chrome/pin/link metrics
```

Feature code MAY choose semantic variants (`warning`, `selected`, `disabled`, `primary action`, domain node category), but SHOULD NOT duplicate raw RGB values and global spacing constants.

DPI/font scaling must be handled at UISession/Theme/backend boundary. Panes must not assume a fixed framebuffer pixel density.

---

## 9. Legacy reference contract

Legacy is explicitly upgraded from “behavior/code mine only” to：

> **visual + interaction + behavior reference where the current Lux Editor intentionally wants continuity; never an architectural source of truth.**

### 9.1 What SHOULD be preserved when useful

```text
recognizable panel composition
major toolbar placement
spacing rhythm
icons / icon semantics
selection/highlight language
property-row organization
AssetBrowser grid/list/search/breadcrumb UX
Scene Outliner row/context-menu behavior
rename/delete/create interaction
Graph node visual language
connection gesture
context menus
common shortcuts
focus/reveal behavior
```

### 9.2 What MUST NOT be copied merely for parity

```text
Legacy ownership trees
EditorScene monolith
manager/service webs
callback injection webs
raw ImGui call-site structure
panel-owned compiler/runtime
runtime-reflection UI hot path
legacy duplicate graph model
legacy state synchronization hacks
```

### 9.3 Visual parity target

The target is **recognizable visual/interaction parity**, not pixel-perfect identity.

Pixel-perfect output is not a normative requirement because font rasterization, DPI, platform, renderer backend and dependency versions can change pixels without changing the intended design language.

For important surfaces, side-by-side reference screenshots MAY be used as review evidence. Screenshot tests SHOULD focus on layout/style regressions only after the UI is deterministic enough to make them useful.

---

## 10. Generated Inspector / Value Binding contract

Generated UI MUST target Lux UI, not Dear ImGui.

Correct path：

```text
LUX annotations
    ↓ codegen
Generated ComponentEditorBinding
    ↓ typed C++
EditorValueBinding<T>
    ↓
Lux UI public API
    ↓ private
ImGui backend
```

Forbidden path：

```text
generated source
    -> #include <imgui.h>
    -> ImGui::DragScalar(...)
```

`EditorValueBinding<T>` is the stable typed semantic layer for primitive/property editing. It receives enough Lux UI/context capability to draw and report mutation/gesture lifecycle without knowing backend types.

Example：

```cpp
template<>
struct EditorValueBinding<double>
{
    static ui::EditResult edit(
        ui::Frame& frame,
        std::string_view label,
        double& value,
        const GeneratedFieldSpec& spec);
};
```

The private ImGui backend MUST preserve `double` precision using appropriate scalar APIs; generated/public code never names `ImGuiDataType_Double`.

---

## 11. Property editing and gesture semantics

Lux UI must expose enough edit lifecycle to support live update + one undo operation per gesture.

Required semantic facts include, where applicable：

```text
value changed this frame
item/gesture activated
item/gesture deactivated after edit
commit/cancel semantics for text/popup editors
read-only/disabled state
```

The public API SHOULD expose these as Lux semantic result/status rather than forcing Editor code to query ImGui item state.

Example conceptual result：

```cpp
struct EditResult final
{
    bool changed{};
    bool began{};
    bool committed{};
};
```

Exact names are implementation-level, but EntityInspector/Gizmo/Graph editing MUST be able to implement canonical undo gesture semantics without using ImGui internals.

---

## 12. Tables、Trees、Menus、Popup、Drag/Drop

Wave U must support the structural primitives needed by the first real Editor slices：

```text
window / child region
toolbar / menu / context menu
popup/modal when actually required
table/property rows
tree rows with flat/hierarchy presentation
splitter/regions if required by current shell
search/input/filters
tooltip
drag source / drop target
focus/hover/activation facts
```

Drag/drop payload must carry Lux/domain stable values, not backend pointers.

Asset example：

```text
AssetBrowser
    -> ui::DragPayload{AssetId, optional AssetTypeId}
    -> Scene/Inspector/Material receiver
    -> resolve stable identity through Context
```

MUST NOT expose ImGui payload pointer lifetime as the domain contract.

---

## 13. Texture / image presentation seam

Public UI MUST NOT expose `ImTextureID`.

Use an opaque Lux presentation handle such as：

```text
ui::TextureHandle
```

or an already-existing equivalent render/UI presentation handle if the current codebase provides one.

Mapping to backend texture identifiers belongs to `UISession`/private render bridge.

This seam is used by：

```text
Asset thumbnails
Material preview
Scene viewport presentation
icons/atlas where applicable
```

The UI layer does not own the underlying RenderRuntime/device/resource lifetime unless an existing narrow UI resource contract explicitly says so.

---

## 14. ViewportElement

`ui::ViewportElement` remains a backend-independent UI interaction/presentation seam.

It may expose Lux semantics such as：

```text
content size/origin
local pointer coordinates
hover/focus
resize
mouse/button interaction
drag/drop target
presentation texture/target handle
```

It MUST NOT learn about：

```text
Scene
Entity
Material
Model
RenderRuntime ownership
Vulkan/DirectX/Metal backend objects
ImGui types
```

SceneEditor interprets those Lux UI facts using L3/L5 capabilities.

---

## 15. Graph NodeCanvas isolation

Current graph architecture already separates source/editing/render protocol. Wave U must finish backend isolation without redesigning those semantics.

Target path：

```text
GraphTopology / GraphLayout
        ↓
GraphRenderProtocol
        ↓
DefaultNodeGraphRenderer
        ↓
ui::NodeCanvas
        ↓ private backend
imgui-node-editor / Dear ImGui
```

`ui::NodeCanvas` is a presentation/interaction primitive, not a graph source model. It MUST NOT own Material/Flow topology or semantic rules.

Public `NodeCanvas` may express：

```text
begin/end node
node/pin presentation ids
node position/layout facts
pin/link draw calls
selection/hover
connection gesture
context gesture
canvas zoom/pan facts
```

Graph `NodeId`/`PinId` remain owned by `modules/function/graph`; the renderer adapts them to any UI presentation identity needed by `NodeCanvas`.

MUST NOT move Graph undo/topology mutation into `NodeCanvas`.

---

## 16. Plugin UI contract

Third-party Editor plugins use the same Lux UI/codegen path as first-party Editor code.

Public plugin SDK MUST provide/allow：

```text
ui::Pane / approved Lux UI API
annotation/codegen toolchain
generated ComponentEditorBinding
generated Graph presentation binding
command contribution
window factory/contribution
```

It MUST NOT require plugin authors to include Dear ImGui or imgui-node-editor.

Plugin code that intentionally bypasses this and directly links ImGui is outside the supported Editor UI ABI/SDK contract and MUST NOT be required for normal plugin functionality.

---

## 17. Source topology recommendation

The exact file names may follow repository conventions, but semantic ownership should converge toward：

```text
modules/function/ui/
  pinclude/lux/ui/
    Pane.hpp
    UISession.hpp
    Frame.hpp
    Geometry.hpp
    Theme.hpp
    Style.hpp
    CommandRouter.hpp
    ViewportElement.hpp
    DragDrop.hpp
    ValueEdit.hpp
    NodeCanvas.hpp          # only when U2 begins

  sinclude/
    ... private Lux implementation contracts ...

  src/
    ... public Lux UI implementation ...
    backend/imgui/
      ... Dear ImGui bridge ...
      ... imgui-node-editor bridge ...
```

This is not permission to create all files up front. Wave U should add only the public concepts actually consumed by C/D/E/G/H/I.

---

## 18. Wave U implementation split

### U0 — Boundary + Frame + Theme

Prerequisite: R0 passes and `EditorApplication/UISession` lifecycle is stable.

MUST establish：

```text
public/private target boundary
no ImGui types in public Lux headers
ui::Frame or equivalent explicit per-frame capability
Pane integration with Lux UI frame
Theme/design token owner
basic geometry/color/id/options types
```

### U1 — Editor primitives

Implement only primitives required by C/D/E：

```text
text/button/input/checkbox/scalar/vector/enum editing
window/child/table/tree/menu/context/popup
property rows
search/filter helpers where genuinely shared
drag/drop semantic payload
edit gesture status for undo
ViewportElement integration
TextureHandle/presentation seam if required
```

After U1, C/D/E may proceed in parallel.

### U2 — NodeCanvas backend isolation

Prerequisites: U0/U1 + existing G render protocol.

MUST：

```text
add ui::NodeCanvas semantic primitive
move imgui-node-editor dependency behind UI private backend
make default graph renderer depend on Lux UI only
remove backend types/includes from engine/editor graph public/private feature code
```

H/I require U2 for their graph UI path.

---

## 19. Qualification / dependency guards

Wave U must add automated guards, not only code-review convention.

At minimum：

```text
1. modules/function/ui public headers compile without imgui include paths.
2. engine/editor/** has no direct include of imgui.h/imgui_internal.h/imgui-node-editor headers.
3. generated Inspector/plugin bindings compile without imgui include paths.
4. only approved private UI backend target directly links Dear ImGui/node-editor.
5. public installed Lux UI package contains no ImGui types in signatures.
6. Editor closure still builds in supported profiles.
```

A repository dependency probe/grep is acceptable as an additional guard, but compile/link closure is the stronger proof.

---

## 20. Legacy parity review gate

For each migrated major surface, implementation completion should include a small visual/interaction parity checklist against the selected Legacy reference：

```text
EntityInspector:
    property grouping / row rhythm / vector editing / readonly / asset picker

AssetBrowser:
    breadcrumb / grid-list / selection / search / context actions / drag source

SceneOutliner:
    row selection / context menu / rename-delete-create / hierarchy visuals when enabled

Graph:
    node chrome / pin/link readability / selection / drag-connect / popup workflow
```

Parity review MUST NOT block architecture improvements merely because Legacy internal behavior depended on broken ownership. In conflicts, current normative architecture wins.

---

## 21. Explicit non-goals

Wave U is NOT：

```text
a new retained-mode GUI framework
a full declarative UI DSL
a reactive data-binding runtime
a generic application UI framework for all future products
a multi-backend abstraction exercise
a reason to move domain state into widgets
a reason to make every UI element a LuxObject
a reason to expose ImGui escape hatches
```

---

## 22. MUST NOT list

```text
No ImGui types in public Lux UI API.
No direct ImGui calls in L5 feature code after U migration.
No direct imgui-node-editor calls outside UI backend after U2.
No generated/plugin ImGui dependency.
No persistent object for every leaf widget.
No retained widget tree framework.
No IUiBackend before a second real backend/approved requirement.
No feature-local duplicated global Theme literals.
No ImTextureID/ImDrawList escape hatch in public contracts.
No backend pointer in drag payloads.
No NodeCanvas ownership of Graph source/edit semantics.
No Legacy ownership architecture copied for visual parity.
```

---

## 23. Ready definition

Lux UI Foundation is ready when：

```text
public Editor/plugin/generated UI code compiles without Dear ImGui headers
Editor panes render through Lux UI frame/scope/widget APIs
Theme centralizes shared visual language
EntityInspector can implement typed field editing + undo gesture without backend calls
AssetBrowser/SceneEditor primitives are expressible without backend escape hatches
Viewport presentation uses Lux-owned handle/capability
Graph default renderer reaches imgui-node-editor only through ui::NodeCanvas private backend
Legacy visual/interaction review can be performed independently from architecture ownership
no retained-mode widget framework or speculative backend abstraction was introduced
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md` and the execution order in `07-implementation-roadmap-and-gates.md`.
