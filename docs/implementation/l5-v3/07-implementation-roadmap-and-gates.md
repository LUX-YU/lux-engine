# L5 Editor 实施路线、Dependency DAG、Architecture Gates 与 Stop Conditions

Status: **Normative Implementation Roadmap (v3 — current repository continuation)**

Reviewed checkpoint: `main@4593ce9b02ddbe35d81de2ded309666ede0bb8da`.

> 本文定义唯一实际执行顺序。文件编号仅用于阅读组织，不代表 implementation order。Coding agent 必须同时遵守 `08-normative-execution-contract.md`；任何 UI work 必须同时遵守 `10-lux-ui-foundation-and-legacy-visual-parity.md`。

---

## 1. Current checkpoint 与唯一实施 DAG

当前 reviewed repository 已经把以下方向实现到 foundation level：

```text
B   L2 Execution
V1  AssetVfs read/control split
V2  VFS-backed AssetRead + blocking IO isolation
A   EditorApplication + non-owning EditorContext + Toolset
F   Shared Graph Source
G   Graph editing/render protocol separation
```

这些 foundation **不得因为进入 v3 而重新设计**。当前问题是 exact committed HEAD 的 qualification/re-entrancy/lifecycle 仍有阻断，因此下一步是 R0，而不是回到 B/F。

唯一继续 DAG：

```text
                  reviewed current foundation
              B + V1 + V2 + A + F + G
                         │
                         ▼
          R0 Foundation Requalification / Hotfix
             ├─ committed-source reproducibility
             ├─ EditorApplication installTool lifecycle
             └─ TaskScope re-entrant admission/close
                         │
             ┌───────────┴─────────────┐
             ▼                         ▼
       U0/U1 Lux UI Core          S0 Async Script
       + Editor primitives        contract/design gate
             │                         │
      ┌──────┼──────┐                  ▼
      ▼      ▼      ▼              S1/S2 implementation
      C      D      E
 Inspector Asset  Scene
      │      │      │
      └──────┴──────┘
             │
             ├─────────────┐
             ▼             ▼
       U2 NodeCanvas   feature stabilization
       backend isolation
             │
       ┌─────┴─────┐
       ▼           ▼
 H0 Material    I0 FlowForge
 in-memory UI   in-memory UI
       │           │
 persistence    persistence + ScriptSymbol
 gate            identity gate
       ▼           ▼
 H1 durable     I1 durable/package

P Product Target Generation
    = separate product track
    = STOP until project-manifest / target-generation spec approved
```

允许并行：

```text
R0 完成后：U0/U1 与 S0 design 可并行
U1 完成后：C、D、E 可并行；推荐 C 先作为 first UI vertical slice
U2 在 U0/U1 + existing G 上进行，可与 C/D/E 后半段并行
H0/I0 在 U2 后可做 in-memory edit/compile integration
H1/I1 durable workflow 必须等待各自 persistence/identity gate
```

禁止：

```text
在 R0 未通过时继续堆新 feature 并把 qualification debt 留到最后
因为 U wave 而重新改写 Graph source/edit semantics
因为 H/I UI 需要而自行发明 source codec/document identity
因为 S 需要 async 而自行决定 continuation ABI
因为 P 需要 executable 而自行发明 project manifest
```

---

## 2. Historical Pre-L5 Closure — 保持已关闭，不重新展开

Current review shows the previous Material/Graph blockers have substantive fixes in the reviewed direction. They remain regression gates：

### 2.1 Material fail-closed

必须持续覆盖：

- malformed enum/payload；
- invalid pin identity/direction/type/arity；
- non-finite values where invalid；
- missing output/cycles/slot errors；
- no UB；
- no silent fallback miscompile。

### 2.2 Installed Material relocatability

Material compiler installed consumer MUST NOT depend on original source/build absolute paths. Generated/embedded shader support and compiler environment must remain relocatable.

### 2.3 L4 package private boundary

Material compiler MUST NOT cross sibling private source/include boundaries. Shared shader/SPIR-V support keeps a narrow correct owner.

### 2.4 Graph transaction atomicity

Must preserve：

```text
replace-connect rollback
node remove/detach rollback
undo/redo rollback
history/revision advances only on full success
poison/fail-closed when inverse rollback itself fails
```

Before H/I, add fault-injection coverage for Nth canonical mutation failure and inverse rollback failure.

### 2.5 Classification/docs

- node graph target remains EDITOR；
- retired Authoring/legacy normative docs remain superseded；
- no compatibility aliases that resurrect the old architecture。

---

## 3. R0 — Foundation Requalification / Hotfix Gate

R0 is the immediate blocker before new UI feature implementation.

### R0.1 Committed-source reproducibility — P0 blocker

Reviewed HEAD has CMake declaring：

```text
engine/editor/application/test/editor_application_test.cpp
```

but the reviewed Git tree does not contain/track that source. Therefore the recorded test evidence cannot prove a clean clone of that exact HEAD is self-contained.

MUST：

```text
recover the actual test source used by the qualified worktree
commit it to the repository
keep the test target unless coverage is intentionally removed by a reviewed decision
```

MUST NOT simply delete the CMake test target to manufacture a green configure if the test was part of the claimed foundation qualification.

### R0.2 `EditorApplication::installTool` lifecycle — P1 high priority

Current public convenience API must not dereference disengaged Toolset storage after application shutdown.

Required behavior：

```text
COMPOSING     install allowed
RUNNING       explicit FROZEN / invalid-phase result
STOPPING      explicit STOPPING / invalid-state result
JOINED        explicit STOPPING/INVALID_STATE result
```

MUST NOT crash/UB after shutdown.

Tests：

```text
install before start succeeds
install after start fails explicitly
install after shutdown fails explicitly
```

### R0.3 `TaskScope` re-entrant admission — P1 high priority / concurrency correctness

`start()` MUST assume eager Sender start/spawn can invoke user/completion callbacks synchronously before returning.

MUST implement the contract in `06`/`08`：

```text
no eager start while TaskScope owns its state/admission mutex
no re-entrant stop callback while that mutex is held
admitted-but-not-yet-registered start reservation
close waits both reservations and owned operations
```

MUST NOT apply a naïve “unlock immediately before spawn” fix that allows `close()` to finish before the admitted operation registers.

Tests：

```text
inline callback -> requestStop()
inline callback -> nested start()
start vs close
start vs requestStop
close vs admitted starter registration
```

### R0.4 Clean qualification — mandatory

After R0.1–R0.3, qualify from a **brand-new clean checkout**, not the dirty/local worktree that produced previous evidence.

Evidence MUST record at least：

```text
git rev-parse HEAD
git status --porcelain          # empty
git diff --exit-code
git ls-files --error-unmatch engine/editor/application/test/editor_application_test.cpp
```

Then execute the repository-supported matrix：

```text
Default Developer
PLAYER/runtime-clean profile
EDITOR
TOOLCHAIN
Full Render / configured render closure
install + relocated consumers where applicable
second/no-op build after configuration changes
```

R0 is not complete until the evidence names the exact committed HEAD used for all results.

### R0.5 Recommended hardening, not feature blockers unless target requires them

- Linux ThreadSanitizer or equivalent race instrumentation for Execution/VFS/TaskScope where supported；
- Graph fault-injection rollback tests before H/I；
- actual Android configure/build before claiming Android target closure；header-prefix synchronization alone is not an Android build qualification。

### Gate R0

```text
clean clone configures
all referenced test sources tracked
installTool lifecycle explicit/no UB
TaskScope re-entry/races pass
qualification evidence belongs to exact clean HEAD
no foundation architecture redesign introduced
```

---

## 4. Foundation Waves B / V / A / F / G — implemented direction, preserve contracts

These sections remain normative because later work consumes them, but v3 coding agents should normally **not reimplement them**.

### 4.1 Wave B — L2 Execution

Current foundation vocabulary：

```text
ExecutionRuntime
CpuScheduler
MainScheduler
TaskScope
TimerQueue/TimerSender
PortSender
```

Preserve：

```text
bounded admission
real CPU concurrency
owner-thread Main drain
structured lifetime
stdexec stop vocabulary
domain-blind process/execution
```

Do not add `JobSystem/JobManager` or Material/Flow vocabulary to L2.

### 4.2 Wave V1 — Product-wide VFS

Preserve：

```text
explicit mutable AssetVfs control plane
copyable/read AssetVfsView
immutable snapshot mount publication
provider lifetime retained by snapshots
read/read concurrency
```

Do not restore `AssetVfs::Get()`/lazy singleton/coarse unsafe mutable reader state.

### 4.3 Wave V2 — production AssetRead / IO isolation

Preserve VFS-backed `AssetReadPort` and shared bounded Blocking/IO isolation for synchronous storage. No feature-private IO pool.

### 4.4 Wave A — EditorApplication + EditorContext + Toolset

Preserve ownership：

```text
EditorApplication owns:
    ExecutionRuntime
    root TaskScope
    mutable AssetVfs
    AssetRead endpoint/port
    Toolset
    EditorSelection
    UISession
    SceneMetaManager
    configured RenderRuntime/platform state

EditorContext carries/references:
    Toolset&
    AssetVfsView
    AssetReadPort
    ExecutionRuntime& / TaskScope&
    EditorSelection& / UISession& / SceneMetaManager&
```

### 4.5 Wave F — Shared Graph Source

Preserve exact owner：

```text
modules/function/graph
GraphTopology
GraphLayout
NodeId / PinId
opaque NodeTypeId / PinSemanticId
```

No Material/Flow payload unification.

### 4.6 Wave G — Graph editing/render protocol

Preserve separation：

```text
GraphEditingSession
GraphIntent
GraphRenderProtocol
renderer/presentation
```

U2 changes only the concrete UI backend path; it MUST NOT alter the source/editing SSOT.

---

## 5. Wave U — Lux UI Foundation / Dear ImGui isolation

Detailed normative design: `10-lux-ui-foundation-and-legacy-visual-parity.md`.

### U0 — Public/private boundary + Frame + Theme

Prerequisite: R0.

MUST：

```text
modules/function/ui remains the public UI owner
Dear ImGui private to UI backend source/target
no ImGui types in public Lux UI headers
ui::Frame or exact equivalent explicit per-frame capability
Pane draws through Lux UI
Theme/design token ownership
Lux geometry/color/id/options types
```

MUST NOT：

```text
retained widget tree
WidgetManager
IUiBackend virtual leaf interface
public ImGui escape hatch
one-to-one public clone of every ImGui flag/type
```

### U1 — Editor primitives required by C/D/E

Implement only proven primitives：

```text
window/child
menu/context menu/popup where required
toolbar
table/property rows
tree rows
text/button/input/checkbox
scalar/vector/enum editors
search/filter helpers where shared
tooltip/focus/hover facts
drag/drop stable payload
edit gesture begin/change/commit facts
ViewportElement integration
opaque UI texture/presentation handle if required
```

Central Theme must carry shared spacing/palette/row/tool/asset/tree/graph metrics. Feature code should not copy Legacy magic constants.

### U2 — Graph NodeCanvas backend isolation

Prerequisites: U0/U1 + existing G.

Target：

```text
GraphRenderProtocol
    -> DefaultNodeGraphRenderer
    -> ui::NodeCanvas
    -> private imgui-node-editor / Dear ImGui backend
```

MUST remove direct node-editor/ImGui dependency from L5 graph renderer/domain presentation code.

### Gate U

```text
public Lux UI package compiles without ImGui include path
engine/editor/** compiles without direct ImGui/node-editor headers after migration
plugin/generated UI code compiles without ImGui dependency
only approved private UI backend links Dear ImGui/node-editor
Theme centralizes shared visual language
no retained-mode framework / speculative backend abstraction
```

---

## 6. Wave C — Generated EntityInspector

Prerequisites: `R0 + A + U1`.

C is the recommended first real UI vertical slice because it validates Lux UI, codegen, Selection, typed ECS mutation, undo gesture and asset picker together.

### C0 Codegen projection

- annotation -> typed Lux UI editor binding；
- no runtime `RefField` hot path；
- no runtime fallback renderer；
- generated source has no Dear ImGui include/type/call。

### C1 Value binding

First set：

```text
bool
integer
float/double
Eigen vector
quaternion
enum
AssetId
readonly
nested explicitly supported value type
```

`double` remains double through Lux UI; backend precision handling is private.

### C2 EntityInspector

- component enumeration by schema/editor_visible；
- generated binding lookup；
- direct typed mutation；
- `registry.patch<T>()`；
- local search/expand state；
- property row/theme follows Lux UI and may visually reference Legacy Inspector。

### C3 Semantic fields / undo

- Parent relation never raw write；
- AssetId picker keeps stable identity, re-resolves on commit；
- edit gesture gives live patch but creates one undo operation on commit。

### Gate C

```text
No runtime reflection fallback
No direct Dear ImGui dependency
No per-component hand wiring
Transform update reaches reactive system
Unknown binding fails visibly/read-only; never silently generic-write
Parent invariant preserved
undo gesture requires no backend-specific item-state calls
```

---

## 7. Wave D — AssetBrowser + FileMonitor

Prerequisites: `R0 + A + U1`; background import uses existing B/V2.

### D0 VFS-first

MUST directly consume `context.vfs()` (`AssetVfsView`).

MUST NOT create：

```text
AssetIndex
AssetCatalog framework
ContentBrowser source DB
AssetManager singleton
```

### D1 AssetBrowser

- virtual path/folder browse；
- local search/filter over enumerate snapshot；
- grid/list；
- breadcrumb；
- stable `AssetId` drag payload；
- thumbnail through Lux opaque UI presentation handle；
- local window state only；
- visual/interaction language may intentionally match Legacy AssetBrowser through Theme。

### D2 FileMonitor

- Platform FileWatcher remains raw mechanism；
- L5 wrapper normalize/debounce/stabilize；
- absolute normalized path contract；
- project/root generation filtering is mandatory correctness mechanism；
- `clearPending()` is never a substitute for generation filtering。

### D3 Toolchain integration

- import/cook through concrete Toolset capability；
- root TaskScope owns operation lifetime；
- V2 shared Blocking/IO isolation for blocking stages；
- completion re-resolves stable asset/project generation；
- no UI pointer captured across worker completion。

### Gate D

```text
closing AssetBrowser leaves VFS alive
no recursive filesystem scan per frame
stale project events rejected by generation
no AssetIndex introduced
no UI-owned worker thread
no direct Dear ImGui dependency
```

---

## 8. Wave E — SceneEditor / SceneOutliner / Viewport

Prerequisites: `R0 + A + U1`.

### E0 Selection

v1 MUST use one EditorApplication-owned `EditorSelection`, referenced via `context.selection()`.

Identity：

```text
EditorSceneHandle{slot,generation} + Entity
```

No Scene AssetId as live identity; no L3 SceneId created solely for Editor.

### E1 Outliner

- Flat projection is first-class；
- hierarchy only when hierarchy semantics exist；
- hierarchy mutation via canonical `reparent/detach`；
- Inspector independent from projection；
- Legacy row/highlight/context interaction may be a visual reference without reintroducing legacy ownership。

### E2 Viewport

- application composition supplies render presentation capability；
- Lux `ViewportElement` exposes UI interaction facts only；
- picking updates shared Selection；
- gizmo direct typed Transform mutation/patch；
- no RenderRuntime/device ownership in window；
- no ImGui texture/ID types in viewport public seam。

### E3 Scene Settings

- generated Lux UI where static schema supports it；
- lower-layer Scene/System builder remains authoritative validator。

### Gate E

```text
Scene works without Parent
Flat projection works
Hierarchy optional
No giant EditorScene
No RenderRuntime/device creation in L5 window
No direct Dear ImGui dependency
```

---

## 9. Wave H — MaterialEditor

Base prerequisites for in-memory integration: `R0 + A + B + F + G + U2`.

### H0 In-memory edit / compile integration — allowed

`Toolset` installs long-lived `MaterialGraphCompiler`：

```text
immutable environment
shared scheduler capabilities
compile(owned snapshot) -> domain Sender
```

`MaterialEditor` is the window; no `MaterialEditorPane` + nested graph owner duplication.

Async path：

```text
clone/freeze source
compiler.compile(snapshot)
root TaskScope owns operation
CPU/blocking stages as appropriate
Main completion
stable document/revision check
apply diagnostics/preview
```

Window close does not cancel root-scope work.

### H1 Durable source workflow — STOP gate

If MaterialGraph source codec/document identity is not approved：

```text
STOP durable open/save/reopen/project-source integration.
```

H0 may prototype in-memory editing/compilation, but MUST NOT invent a file format or claim complete MaterialEditor persistence.

### Gate H

- same compiler instance parallel compiles；
- owned snapshot isolation；
- relocated compiler environment；
- stale revision discard；
- window-close survival；
- graph UI reaches backend only through Lux UI/NodeCanvas；
- durable path only after source identity/codec approval。

---

## 10. Wave I — FlowForgeEditor

Base prerequisites for in-memory integration: `R0 + A + B + F + G + U2`.

Compiler remains immutable/reentrant; per invocation owns MLIRContext/temp state.

### I0 In-memory edit / compile integration — allowed

- reuse V2 BlockingScheduler for blocking linker/file stages；
- no FlowForge-private blocking pool；
- `ProcessSender` only when real subprocess lifecycle requirements justify it；
- graph UI uses Lux UI/NodeCanvas only。

### I1 Durable/package workflow — STOP gate

If FlowGraph codec or stable `ScriptSymbol` source identity is not approved：

```text
STOP durable open/save/packaging integration.
```

MUST NOT restore retired Authoring to bypass this gate.

---

## 11. Wave S — Runtime Async Script Foundation

Mechanical prerequisites B+V2 already exist, but the continuation ABI/state model is not yet sufficiently frozen for implementation. Therefore S is split into a design gate and implementation waves.

### S0 — mandatory contract freeze

Before code implementation, approve at minimum：

```text
script instance stable identity/generation
continuation/state record
resume token / program point representation
locals/value storage across suspension
result/error channel into resumed script
cancellation / scene shutdown semantics
explicit Simulation resume queue/stable point
nested/repeated async operation behavior
ordering/reentrancy rules
```

Sender operation state is not the script continuation ABI.

### S1 — core continuation/resume runtime

After S0：

```text
persist continuation/state
return control to Simulation on suspend
bounded resume queue
validate stable instance/generation on resume
resume only at explicit Simulation point
```

### S2 — first bridges

```text
Delay -> TimerSender
AssetLoad -> AssetReadPort/loadAsset<T>()
domain async completion -> resume record
```

MUST NOT：

```text
sleep game thread
sync_wait async Sender on game/main thread
busy wait GPU fence
resume directly on worker/render callback
retain native C++ stack frame across frames
```

### Gate S

Delay and asset load samples suspend across frames while frame loop continues; shutdown invalidates/cancels outstanding continuations safely without use-after-free.

---

## 12. Product Track P — Project-specific executable generation

Target outcome remains frozen：final shipping product is a project-specific executable, not a generic fixed Player binary.

Implementation remains STOP until an approved project manifest / target-generation specification freezes exact inputs：

```text
module/system selection
project native/generated code inputs
plugin/static linkage policy
platform/backend selection
cooked/pak inputs
CMake/generator output contract
```

Current `PLAYER` is only a runtime-clean qualification profile.

---

## 13. Suggested Source Topology (v3)

```text
modules/
  function/
    ui/                    # Lux public UI + private Dear ImGui backend
    graph/                 # shared structural graph source
    material/
    flowforge/

engine/
  process/
    execution/             # Runtime, CPU/Main/Blocking where authorized, TaskScope, Timer
    asset_loading/         # AssetReadPort / VFS-backed endpoint / typed load sender

  editor/
    application/           # EditorApplication composition leaf
    context/               # EditorContext / Toolset / Selection contracts
    inspector/
    asset/
    scene/
    graph/                 # editing/render protocol; no backend source ownership
    material/
    flowforge/

  toolchain/
    material/
    flowforge/
    asset/
```

Exact leaf names may follow repository conventions; semantic owner/dependency direction may not change without review.

---

## 14. Product / Dependency Guards

MUST maintain：

```text
PLAYER/runtime-clean qualification: no Editor, no L4 compiler closure
EDITOR: L5 + explicitly required L4/L3/L2/L1/L0
TOOLCHAIN: L4 tools, no Editor UI
FUNCTION: no Toolchain/Editor dependency
PROCESS execution: no Material/FlowForge/Editor vocabulary
modules/function/ui public: no Dear ImGui types
engine/editor after Wave U: no direct Dear ImGui/imgui-node-editor dependency
```

Editor linking Toolchain must be explicit, not through an “all tools” aggregate.

---

## 15. Codegen Guards

- first-party/plugin Inspector binding = generated typed Lux UI code；
- no runtime-reflection fallback；
- plugin SDK uses the same public generator；
- generated source does not include Dear ImGui；
- Wave U does not invent a declarative UI DSL；
- Graph F/G/U2 do not invent a universal graph annotation/payload system。

---

## 16. Coding Agent Execution Protocol

At the beginning of every wave：

1. Read current HEAD and canonical architecture source.
2. Verify the worktree/source snapshot is self-contained; for qualification work start from a clean clone.
3. Inventory current package/target/public headers/tests.
4. List only this wave's MUST changes.
5. List MUST NOT touch / STOP gates.
6. Add/update architecture guard/tests before or with production changes.
7. Hard-cut migration; no compatibility shim/parallel architecture.
8. One semantic commit per topic where practical.
9. Run relevant product closure + installed/relocated consumer tests.
10. Record exact HEAD and clean status in qualification evidence.
11. If a STOP condition appears, report the smallest missing design decision; do not infer a framework/format/owner.

---

## 17. Global Stop Conditions

STOP and return to architecture review if implementation appears to require：

```text
new upward dependency
new global Manager/Registry/Services root
change of EditorApplication/EditorContext ownership
change of Toolset typed lookup/freeze semantics
shared graph owner move away from modules/function/graph
Material/Flow payload rewrite merely to finish topology/UI work
new source persistence/file format/document identity
new project manifest/target format
script continuation ABI invented during coding
engine/authoring resurrection
compatibility shim/parallel old-new architecture
Dear ImGui/public backend type leaked outside private Lux UI backend
retained-mode widget tree/framework
IUiBackend or generic backend abstraction without a second real backend/approved need
```

---

## 18. Anti-over-abstraction Rules

Do not proactively create：

```text
EditorManager
EditorServices
ServiceRegistry
ToolRegistry (Toolset is the bounded exception)
JobSystem / JobManager
CompilerManager / GenericCompiler
GenericGraphIR / UniversalGraph
GraphValue property bag
AssetManager replacement
AssetIndex during Wave D
SelectionRegistry during v1
WidgetManager / retained visual tree
IUiBackend virtual leaf abstraction
persistent object for every Button/Label/Input widget
```

A new abstraction needs at least two real consumers or an independently proven contract gap; theoretical future flexibility is not sufficient.

---

## 19. Recommended Commit Sequence from current checkpoint

```text
R0.1 repo: recover/commit editor_application_test source; prove clean configure
R0.2 editor/application: make installTool lifecycle fail closed after start/shutdown
R0.3 process: make TaskScope admission/close re-entrant-safe + race tests
R0.4 qualification: rerun clean-clone profile/install matrix and update evidence

U0 ui: freeze public/private Lux UI boundary + Frame + Theme
U1 ui: add property/table/tree/drag-drop/edit-gesture/viewport primitives needed by C/D/E

C1 editor/codegen: generate Lux UI component editor bindings
C2 editor: EntityInspector typed value/property UI
C3 editor: semantic fields + undo gesture + AssetId picker

D1 editor: VFS-first AssetBrowser using Lux UI
D2 editor/platform: FileMonitor normalization + generation filtering
D3 editor/toolchain: root-TaskScope import/cook integration

E1 editor: Selection + flat SceneOutliner
E2 editor: optional hierarchy projection + canonical reparent/detach
E3 editor: Lux Viewport/picking/gizmo

U2 ui/graph: NodeCanvas + move imgui-node-editor behind UI backend

H0 material: in-memory MaterialEditor edit/compile after U2
H1 material: durable source only after codec/document identity approval
I0 flowforge: in-memory FlowForgeEditor after U2
I1 flowforge: durable/package only after codec + ScriptSymbol identity approval

S0 design: freeze continuation/resume ABI
S1/S2 scripting: continuation runtime + Delay/AssetLoad bridges

P* product: only after project-manifest / target-generation spec approval
```

---

## 20. Qualification Matrix

Every cross-layer wave at minimum：

```text
clean source / exact HEAD evidence
Default Developer
PLAYER/runtime-clean profile
EDITOR
TOOLCHAIN
Full Render where applicable
install + relocated consumer where applicable
no-op second build after build-system changes
architecture dependency probe
```

R0/Execution additionally：

```text
stress/race/shutdown/queue-bound tests
TaskScope synchronous re-entry tests
ThreadSanitizer/race instrumentation where environment supports it
```

UI additionally：

```text
modules/function/ui public headers compile without ImGui include paths
engine/editor migrated targets compile without direct ImGui/node-editor headers
generated/plugin binding target compiles without ImGui include paths
only private UI backend links Dear ImGui/node-editor
Legacy visual/interaction checklist for major migrated surfaces
```

Graph additionally：

```text
Material artifact equivalence
FlowForge compile/execute equivalence
Nth mutation fault-injection rollback
inverse rollback failure -> poisoned/fail-closed session
```

Platform closure：if Android is claimed as supported/qualified for the wave, run an actual Android configure/build rather than only synchronizing install include prefixes.

---

## 21. L5 Foundation / UI Ready Definition

Before calling the new Editor foundation ready for H/I feature expansion, all must hold：

```text
R0 clean-checkout qualification passes
EditorApplication ownership + non-owning EditorContext stable
installTool lifecycle fail-closed in every application state
Toolset frozen typed capability lookup stable
TaskScope re-entrant-safe admission/close stable
AssetVfsView + immutable mount publication stable
production AssetRead does not block owner thread
Lux UI public/private backend boundary stable
Dear ImGui hidden from Editor/plugin/generated code
Theme and UI primitives sufficient for C/D/E
generated EntityInspector hot path stable
VFS-first AssetBrowser + FileMonitor generation filtering stable
Scene flat/optional hierarchy + shared Selection stable
Shared Graph Source + Graph editing/render protocol stable
NodeCanvas hides imgui-node-editor before H/I graph UI expansion
no known architecture STOP condition outstanding
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md`.
