# Shared Graph Source、GraphKit 重构、MaterialGraph/FlowGraph 共用拓扑设计

Status: **Normative Graph Source / Editing / Lux UI Integration Design (v3)**

> 这是本轮讨论中影响面最大的一项设计。共识是可以把 GraphKit 真正通用的 topology 下沉成纯 source-data；Material 与 FlowForge 共用 topology/layout/identity，但保持 typed domain payload 和独立 compiler IR。

Current checkpoint: F/G structural/editing separation is implemented direction；Wave U2 is the next graph-UI backend-isolation step.

---

## 1. 当前重复

当前至少存在三份 graph structural vocabulary：

```text
MaterialGraph
    own node storage
    own node id
    own pins/links
    own add/remove/restore/connect
    node ui_pos/ui_placed

FlowGraph
    own node storage
    own stable id
    own pin classes/links
    own add/remove/restore
    variables/exports

Editor GraphKit
    GraphNodeRef
    GraphPinRef
    GraphLinkView
    projected per-frame topology
```

这说明“共享 topology”不是为了抽象而抽象，而是已经有三个真实消费者/重复实现。

---

## 2. 分层目标

```text
L0 Shared Graph Source Foundation
    GraphTopology
    GraphLayout
    stable NodeId / PinId / Link
    structural invariants

L0 Function Material
    MaterialGraph = shared topology + Material typed payload/config

L0 Function FlowForge
    FlowGraph = shared topology + Flow typed payload/vars/exports

L4 Material Tool
    MaterialGraph -> MaterialIR -> ShaderIR -> MaterialDescription

L4 FlowForge Tool
    FlowGraph -> FlowForgeIR -> MLIR/AOT -> ScriptArtifact

L5 Graph Editing
    Graph interaction/undo/render protocol

L5 MaterialEditor / FlowForgeEditor
    domain presentation + rules + compiler tool access
```

---


## 3. 物理 owner：v1 冻结为 `modules/function/graph`

首轮 Shared Graph Source foundation MUST 位于：

```text
modules/function/graph/
```

理由：

- 被 Material/FlowForge reusable source model 直接消费；
- 不属于 Editor；
- 不属于 Toolchain；
- 它描述可编辑 source graph 的 reusable function capability，而不是 core language/runtime primitive。

Wave F 禁止重新评估到：

```text
modules/core/graph  # explicitly forbidden for Wave F
engine/editor/graph
engine/toolchain/graph
```

如果未来出现多个完全非-source-graph consumer，是否继续下沉到 Core 必须经过新的独立架构 review；本轮实现不得自行移动 owner。

## 4. GraphTopology 必须是纯数据/结构

建议核心 vocabulary：

```cpp
using NodeId = uint64_t;
using PinId = uint64_t;

struct NodeRecord
{
    NodeId id;
    NodeTypeId type;
};

struct PinRecord
{
    PinId id;
    NodeId owner;
    EPinDirection direction;
    uint8_t fan_cap;
    PinSemanticId semantic;
};

struct LinkRecord
{
    PinId from;
    PinId to;
};
```

`NodeTypeId`、`PinSemanticId` 只作为 opaque stable identity，不承载 Material/Flow 类型对象。

### 4.1 GraphTopology 提供

```text
add/insert/remove node
add/insert/remove pin
connect/disconnect
lookup by stable id
iteration
referential integrity
fan-cap structural enforcement
clone/copy if source use case requires
```

### 4.2 GraphTopology 不提供

```text
Material EValueType conversion
FlowForge RefType
exec/data legality
Material shader type coercion
Flow control-flow legality
palette
node color
UI backend / Dear ImGui dependency
compiler lowering
serialization format policy
```

---

## 5. PinId 直接成为 stable identity

当前 Editor GraphKit 用 `(node id, side, pin index)`；FlowForge 已经有 derived stable pin id。

新 foundation 建议：

```text
NodeId stable
PinId stable
Link only references PinId
```

不要以 vector ordinal 作为 persistent link identity。

收益：

- dynamic pins；
- Sequence output 增删；
- undo/redo；
- serialization；
- renderer selection；
- diagnostics；
- node reconstruction。

Pin ordinal 只用于展示顺序，可作为 node-local ordering metadata，不作为 identity。

---

## 6. Layout 与 Topology 分离

```cpp
struct GraphNodeLayout
{
    float x;
    float y;
    bool placed;
};

class GraphLayout
{
    // NodeId -> GraphNodeLayout
};
```

Compiler：

```text
consumes topology + domain payload
ignores layout
```

Renderer：

```text
consumes topology + layout + domain presentation
```

这允许从 Material `Node` 中移除当前的 `ui_pos/ui_placed` Editor metadata，避免 semantic node base 污染。

---

## 7. 不创建 Universal Graph Payload

明确禁止：

```cpp
GraphNode {
    map<string, GraphValue> properties;
}
```

以及：

```text
GraphValue = variant of every engine type
GenericGraphIR
UniversalGraphAST
```

原因：

- 丢失 domain 类型安全；
- compiler 变成字符串查属性；
- codegen 优势消失；
- plugin ABI 被迫永久支持万能 value type；
- Material/Flow semantics 被揉在一起。

---

## 8. MaterialGraph：共享 topology + typed payload

建议结构：

```cpp
struct MaterialGraphData
{
    ELightingTechnique shading_model;
    vector<TextureSlotDecl> texture_slots;
    vector<ParamSlotDecl> param_slots;
    RenderState render_state;
};

using MaterialNodePayload = variant<
    ConstantData,
    InputData,
    SampleTextureData,
    MathData,
    SwizzleData,
    ConstructData,
    DecodeNormalData,
    TbnTransformData,
    ParamData,
    OutputSurfaceData
>;

class MaterialGraph
{
    graph::GraphTopology topology_;
    graph::GraphLayout layout_;
    MaterialGraphData data_;
    MaterialPayloadStore payloads_;
};
```

第一阶段可以继续保留现有 polymorphic Material nodes，并先让它们引用/组合 shared topology；不要强迫 topology migration 与 payload variant migration 同一 commit 完成。

---

## 9. FlowGraph：共享 topology + typed payload

FlowGraph domain data：

```text
Graph variables
Exports/functions
Flow node operation payloads
RefType/RuntimeObject facts where still semantically valid
```

建议：

```cpp
class FlowGraph
{
    graph::GraphTopology topology_;
    graph::GraphLayout layout_;
    FlowGraphData data_;
    FlowPayloadStore payloads_;
};
```

FlowGraph 的 exec/data semantics 仍属于 FlowForge。

---

## 10. Structural vs Semantic Connect

共享 topology 只验证结构：

```text
from pin exists
from is OUTPUT
to pin exists
to is INPUT
fan cap
duplicate link
```

Material semantic rules：

```text
EValueType compatibility
surface output rules
slot/type rules
cycle policy
```

FlowForge semantic rules：

```text
EXEC_OUT -> EXEC_IN
DATA type compatibility
pure/control rules
function/variable semantics
```

推荐 mutation sequence：

```text
Domain rule preflight
    ↓ allowed
GraphTopology atomic connect
```

Compiler 仍必须独立 authoritative validate；Editor preflight 只是 UX。

---

## 11. Graph Source 不是 IR

严格术语：

```text
GraphTopology / MaterialGraph / FlowGraph = editable source representation
MaterialIR / FlowForgeIR / MLIR / ShaderIR = compiler IR
```

不要把 shared topology 命名为：

```text
GraphIR
UniversalIR
NodeIR
```

否则容易错误地把 compiler-only normalization/SSA/control-flow 概念反推到 source model。

---

## 12. Compiler 路径

### Material

```text
MaterialGraph
├─ GraphTopology
├─ MaterialPayload
└─ MaterialGraphData
      ↓
MaterialLowering
      ↓
MaterialIR
      ↓
ShaderIR/backend
      ↓
MaterialDescription
      ↓ cooker
MaterialAsset
```

### FlowForge

```text
FlowGraph
├─ GraphTopology
├─ FlowPayload
└─ vars/exports
      ↓
FlowForge lowering
      ↓
FlowForge IR
      ↓
MLIR/AOT/link
      ↓
ScriptArtifact
```

---

## 13. GraphKit 重定义

“GraphKit”不再意味着一个巨大 `GraphEditor` backend-specific widget。

L5 共享部分拆成：

```text
GraphEditingSession
├─ topology mutation orchestration
├─ selection
├─ topology lock
├─ undo/redo transaction
└─ layout edit

GraphRenderProtocol
├─ read source topology/layout
└─ emit interaction intents

DefaultNodeGraphRenderer
└─ concrete L5 renderer using Lux `ui::NodeCanvas`

ui::NodeCanvas
└─ L0 Lux UI presentation primitive; private backend may use imgui-node-editor
```

MaterialEditor / FlowForgeEditor 直接是 UI window，不嵌一个概念上重复的 `GraphEditor` 对象。

---

## 14. Render Protocol

Renderer 消费 source，不拥有第二份 graph model。

```text
GraphSource
    ↓ read
Renderer
    ↓ user action
GraphIntent
    ↓
GraphEditingSession
    ↓ canonical mutation
GraphSource
```

Intent 示例：

```text
Connect(PinId, PinId)
Disconnect(LinkId or PinId pair)
AddNode(NodeTypeId, position)
RemoveNode(NodeId)
MoveNode(NodeId, position)
SelectNode(NodeId)
InvokeNodeAction(NodeId, ActionId)
```

Renderer 不能直接修改 domain payload/topology。

---

## 15. Default Renderer vs Custom Renderer

共享默认：

```text
canvas
node chrome
pin/link drawing
selection
zoom/pan
connection gesture
context gesture
```

domain 可定制：

```text
node title/style
pin style
node body
special overlay
custom link style
```

甚至整个 Renderer 可被 Material/FlowForge/plugin 替换。

Renderer replacement is a Lux-level renderer replacement, not permission to expose backend APIs。The default renderer consumes `ui::NodeCanvas`; custom/domain renderers consume the same Lux UI surface unless a separately approved UI semantic is added。

---

## 16. Node Body 与 Domain Rules 分离

旧 `IGraphSchema::drawNodeBody` 把 semantic schema 与 backend-specific drawing 混合。

新设计：

```text
MaterialGraphRules
    canConnect / canDelete / node structural actions

MaterialGraphPresentation
    node title/style/pin presentation
    generated node body drawer

FlowGraphRules
    flow semantics

FlowGraphPresentation
    flow-specific rendering
```

不再让 “schema” 直接调用 Dear ImGui 或其他 backend API；presentation 只调用 Lux UI/NodeCanvas。

---

## 17. Graph UI Codegen

Graph node payload 同样可以使用公共 generator 生成 UI：

```text
Node payload annotations
    ↓
Generated GraphNodePresentationBinding
```

例如 SampleTexture：

```text
texture_slot -> generated asset picker
uv input pin -> topology/schema generated pin description
RGBA output -> generated pin description
```

第一阶段不需要设计一个完整 Graph annotation DSL；优先复用已有类型/member annotation，并只为 graph structural pins 增必要标记。

---

## 18. Plugin Graph Nodes

插件如果要扩展开放 graph language，需要：

- stable NodeTypeId；
- generated structural schema；
- typed payload lifetime/serialization contract；
- generated Editor presentation；
- compiler lowering contribution。

当前 Material 是 closed-enum source language，FlowForge 是否允许 runtime plugin node 需另外决定。

不要仅因为插件理论可能存在就立刻将 payload 变成 universal type-erased store。

---


## 19. Payload 策略：Wave F 明确禁止重写

Wave F 的唯一目标是消除 topology/layout/stable identity 重复，不是同时重写两种语言的 node payload model。

因此 Wave F MUST preserve：

```text
Material existing typed/polymorphic node payload semantics
FlowForge existing typed/polymorphic node payload semantics
Material graph-level config
FlowForge variables/exports/control semantics
```

Wave F MUST NOT：

```text
convert Material nodes to std::variant
convert FlowForge nodes to std::variant
introduce RuntimeObject/Any universal node payload
introduce map<string, GraphValue>
introduce LUX_GRAPH_NODE annotation DSL
introduce generic compiler dispatch registry
```

未来如果第三种 graph language 证明 payload storage 也真实重复，再单独设计 Phase 2。该决定不由 Wave F coding agent 推断。

## 20. Undo/Redo

Undo 属于 L5，不下沉到 L0 source foundation。

GraphTopology 必须提供可组合的原子 mutation，L5 `GraphEditingSession` 记录 inverse intent。

严格要求 transaction atomicity：

```text
compound connect replacement
node delete + incident links
undo/redo replay
```

任何中间失败必须恢复到操作前状态，history/revision 不前进。

这也修复当前 GraphCommandStack 已发现的 partial mutation 风险。

---


## 21. 迁移路径（normative scope）

### Wave F0 — Freeze structural contract

只冻结：

```text
NodeId
PinId
NodeTypeId (opaque)
PinSemanticId (opaque)
GraphTopology
GraphLayout
atomic structural mutations
```

### Wave F1 — Introduce `modules/function/graph`

新增 package 与 standalone tests；不得依赖 Material/FlowForge/Editor/Toolchain。

### Wave F2 — MaterialGraph composition

只把以下结构迁移到 shared foundation：

```text
stable node identity
stable pin identity
links
node/pin structural ownership
layout
structural connect/disconnect primitives
```

Material concrete node payload、slot declarations、render state、compiler IR 均保持原 owner/representation。

### Wave F3 — FlowGraph composition

同理，只迁 topology/layout/identity；variables、exports、RefType/RuntimeObject、control-flow node semantics 保持 FlowForge owner。

### Wave G — Editor GraphKit refactor

旧 `GraphEditor` 的语义职责拆成：

```text
GraphEditingSession / intents / undo          L5 semantic editing
GraphRenderProtocol                           L5 protocol
concrete renderer boundary                    L5 presentation implementation
Material/Flow domain presentation/rules       respective L5 editor integration
```

Wave G freezes the source/edit/render separation；it does not require the final UI backend abstraction itself。Shared Graph Source MUST remain Dear-ImGui-free。

### Wave U2 — NodeCanvas backend isolation

在已经完成的 G separation 上 hard-cut concrete backend dependency：

```text
GraphRenderProtocol
    -> DefaultNodeGraphRenderer
    -> ui::NodeCanvas
    -> private ImGui/node-editor backend in modules/function/ui
```

After U2，GraphRenderProtocol 与 domain presentation MUST also remain Dear-ImGui-free；only `modules/function/ui` private backend may directly depend on imgui-node-editor。U2 MUST NOT reintroduce a second graph model or alter `GraphEditingSession` semantics。

### H/I — MaterialEditor / FlowForgeEditor

在 F/G/U2 完成后直接消费 shared source 与 Lux UI renderer path；不得恢复 IGraphView 作为第二份 topology projection，除非某个尚未迁移的外部 graph consumer 有真实兼容需求且单独获批。

## 22. 测试

### Shared topology

- stable NodeId/PinId；
- add/remove/restore；
- pin dynamic insert/remove；
- invalid references fail-closed；
- fan cap；
- deterministic iteration/serialization requirements if specified；
- clone preserves ids/layout。

### Material

- all existing positive/negative compiler tests；
- artifact byte equivalence；
- layout ignored by compiler；
- topology mutation compiler behavior。

### FlowForge

- compile/execute tests；
- variables/exports unaffected；
- control/data links exact；
- stable id decode/roundtrip。

### Renderer

- renderer cannot mutate source except intents；
- custom renderer；
- default renderer through `ui::NodeCanvas`；
- dynamic pin stability；
- undo failure rollback；
- fault injection: Nth canonical mutation failure restores exact pre-transaction source/layout state；
- inverse rollback failure poisons/fails closed the editing session and rejects further mutation until explicit recovery；
- revision/history advances only after complete successful transaction；
- graph/editor targets compile without Dear ImGui/imgui-node-editor include paths after U2；
- only private `modules/function/ui` backend links node-editor。

---

## 23. 禁止项

```text
No GenericGraphIR.
No UniversalGraph language.
No string-keyed property bag.
No Material/Flow semantic enums in shared topology.
No Dear ImGui in shared source package, GraphRenderProtocol or domain presentation.
No compiler thunk stored in GraphTopology descriptor.
No renderer-owned duplicate graph model.
No index-based persistent pin identity.
No L0 undo history.
No forced payload unification in the first topology refactor.
No direct imgui-node-editor dependency outside private Lux UI backend after U2.
No NodeCanvas ownership of topology/edit/undo semantics.
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md` and `10-lux-ui-foundation-and-legacy-visual-parity.md` for renderer/backend work.
