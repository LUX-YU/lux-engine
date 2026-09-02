# L5 EditorContext、Toolset、插件贡献与生命周期设计

Status: **Normative L5 Foundation Design (v3)**  
Parent document: `00-L5-architecture-overview.md`

---

## 1. 设计问题

Editor 是一个长生命周期程序。多个窗口会重复打开/关闭，但以下能力应继续存在：

- 对 product-wide VFS/asset-loading capability 的稳定访问；
- Material/FlowForge compiler facilities；
- 后台 job；
- UI session / commands；
- plugin contributions；
- shared scene metadata；
- shared file monitor / asset-facing control state；
- process execution runtime。

因此不能把这些能力的生命周期绑到 `MaterialEditor`、`AssetBrowser` 等窗口。

同时，也不希望每个窗口构造函数出现十几个独立参数。解决方案是一个**显式传递的 `EditorContext`**。

---

## 2. 层级位置与 ownership

```text
L0
├─ LuxObject / Signal
├─ AssetVfs / AssetVfsView
├─ Lux UI public API / UISession / CommandRouter / Theme
└─ source/meta/codegen

L2
├─ ExecutionRuntime
└─ asset_loading Sender workflow

L3
└─ SceneMetaManager / optional RenderRuntime integration

L4
├─ MaterialGraphCompiler
├─ FlowForgeCompiler
└─ asset cookers

L5
├─ EditorApplication      # physical owner / composition leaf
├─ EditorContext          # non-owning capability aggregate
├─ Toolset
└─ Editor windows
```

不再定义 Product/Application Host。`EditorApplication` 直接产出 Editor executable，并作为 process/application composition root。

## 3. v3 精确 ownership 与 API 形态

v3 ownership 冻结为：

```text
EditorApplication value-owns:
    process::ExecutionRuntime
    process::TaskScope root_tasks
    asset::AssetVfs                 # mutable control plane
    Toolset
    EditorSelection
    UISession
    SceneMetaManager
    RenderRuntime/platform state when configured
    concrete AssetRead endpoint/port

EditorContext owns:
    NO process/application service lifetime

EditorContext references/carries capabilities:
    Toolset&
    AssetVfsView
    AssetReadPort
    ExecutionRuntime&
    TaskScope&
    EditorSelection&
    UISession&
    const SceneMetaManager&
```

建议 create info：

```cpp
struct EditorContextCreateInfo final
{
    Toolset& toolset;
    asset::AssetVfsView vfs;
    process::asset_loading::AssetReadPort asset_read;
    process::ExecutionRuntime& execution;
    process::TaskScope& tasks;
    EditorSelection& selection;
    ui::UISession& ui;
    const scene::SceneMetaManager& scene_meta;
};

class EditorContext final
{
public:
    explicit EditorContext(EditorContextCreateInfo info) noexcept;

    [[nodiscard]] Toolset& toolchain() noexcept;
    [[nodiscard]] asset::AssetVfsView vfs() const noexcept;
    [[nodiscard]] process::asset_loading::AssetReadPort assetRead() const noexcept;
    [[nodiscard]] process::ExecutionRuntime& execution() noexcept;
    [[nodiscard]] process::TaskScope& tasks() noexcept;
    [[nodiscard]] EditorSelection& selection() noexcept;
    [[nodiscard]] ui::UISession& ui() noexcept;
    [[nodiscard]] const scene::SceneMetaManager& sceneMeta() const noexcept;
};
```

### 3.1 禁止的替代实现

```text
EditorContext value-owns AssetVfs
EditorContext owns ExecutionRuntime threads
EditorContext owns root TaskScope lifetime
EditorContext::instance()
EditorServices / ServiceRegistry
unordered_map<string, void*>
getAnything<T>()
per-window VFS
AssetVfs static-method façade with hidden state
AssetVfs::Get() lazy singleton
```

Context 的统一访问路径是显式依赖便利，不是隐藏依赖机制。

## 4. v3 shutdown / 析构顺序

关闭单个 window：

```text
window disconnects signals/commands
window destroys local Lux UI/canvas state
NO Toolset shutdown
NO VFS shutdown
NO root TaskScope stop
NO ExecutionRuntime stop
```

关闭 EditorApplication：

```text
1. stop admitting new Editor work / close project mutation entry points
2. root TaskScope.requestStop()
3. close/await root TaskScope until no owned operation remains
4. destroy Editor windows/listeners that may use EditorContext
5. destroy/reset EditorContext and the already-closed root TaskScope owner
6. requestStop/destroy Toolset
7. destroy EditorSelection / UISession
8. close AssetRead endpoint admission and wait/cancel outstanding reads
9. unmount/destroy mutable AssetVfs
10. requestStop ExecutionRuntime
11. drain required main completions under shutdown contract
12. ExecutionRuntime.join()
13. destroy SceneMetaManager / RenderRuntime / platform-window state in dependency-safe order
```

The exact physical member destruction order may differ if a type internally owns another listed capability, but the following invariants are mandatory：

```text
No live EditorContext may reference a destroyed Toolset/Selection/UISession.
No live TaskScope-owned operation may outlive the capabilities it can call.
No AssetRead operation may outlive provider/VFS/runtime dependencies.
No Runtime worker may outlive ExecutionRuntime.join().
```

不得先销毁 scheduler/VFS/provider，再让 operation state 持有悬空 capability。


## 5. Toolset 的精确 v3 语义

`Toolset` 是 Editor 可使用的长期 L4 tool capability container，不是通用 DI framework。

建议 surface：

```cpp
enum class EToolInstallError
{
    DUPLICATE_TYPE,
    FROZEN,
    CONSTRUCTION_FAILED
};

class Toolset final
{
public:
    template<class T, class... Args>
    expected<T&, EToolInstallError> install(Args&&... args);

    template<class T>
    [[nodiscard]] T& get() noexcept;       // required capability

    template<class T>
    [[nodiscard]] T* find() noexcept;      // optional capability probe

    void freeze() noexcept;
    [[nodiscard]] bool frozen() const noexcept;
};
```

`get<T>()` 的 missing 情况属于 application composition/programmer error：MUST fail closed，不允许自动构造，不允许返回 dummy/no-op tool。具体 fail mechanism 可用 engine invariant/terminate，但不得静默降级。

### 5.1 Key

MUST 使用 typed stable identity，例如 compile-time type token；禁止字符串 service name。

### 5.2 Mutation window

```text
composition phase:
    install built-in tools
    install plugin-contributed tools
    validate required capabilities
    freeze

runtime/editor phase:
    get/find only
```

冻结后不得 install/remove/replace tool，以避免 function pointer/tool reference 在 window 生命周期中失效。

### 5.3 `EditorApplication::installTool` lifecycle

如果 `EditorApplication` 提供 convenience `installTool<T>()`，它必须先检查 application state，不能假设内部 `Toolset` owner 始终 engaged。

```text
COMPOSING
    -> install allowed, returns Toolset install result

RUNNING
    -> explicit FROZEN/invalid-phase failure

STOPPING / JOINED
    -> explicit STOPPING/invalid-state failure
    -> MUST NOT dereference/reset optional Toolset storage
```

Required regression tests：

```text
install before start succeeds
install after start fails explicitly as frozen
install after shutdown fails explicitly and never crashes/UB
```


## 6. Tool 设计：长期 capability，不是运行中 job 的容器

以 Material 为例：

```text
compileMaterial(snapshot, environment)
    = deterministic synchronous algorithm

MaterialGraphCompiler
    = immutable/reentrant Sender factory
```

允许 Compiler 长期保存：

- immutable compiler environment；
- target/toolchain paths；
- immutable include/search configuration；
- copyable `CpuScheduler` capability；
- `shared_ptr<const Shared>` 形式的只读配置。

禁止保存：

- current source graph；
- current IR；
- current diagnostics；
- current temporary directory；
- `bool compiling`；
- in-flight job list；
- mutable cancellation source；
- 为“线程安全”而给整个 compile 加全局 mutex。

同一个实例必须满足：

```text
compile(A) || compile(B) || compile(C)
```

并发安全。若底层第三方库需要独占 context，则该 context 在每个 invocation 内独立创建，或由明确的独立 thread-safe facility 提供；不得把“第三方库不方便”转化成 Compiler 整体串行锁。

### 6.1 Invocation ownership

异步 API 必须 by-value/owned snapshot：

```cpp
compiler.compile(material_graph.clone(), options);
```

而不是：

```cpp
compiler.compile(const MaterialGraph& mutable_document_graph); // async forbidden
```

一次任务的 snapshot、IR、MLIRContext、shaderc objects、temporary files、receiver/stop token 归 Sender operation state。

## 7. Editor window 构造

```cpp
class MaterialEditor final : public ui::Pane
{
public:
    MaterialEditor(EditorContext& context, MaterialDocumentId document);

private:
    EditorContext& context_;
    MaterialGraphCompiler& compiler_;
};
```

构造函数可缓存 Tool 引用：

```cpp
compiler_(context.toolchain().get<MaterialGraphCompiler>())
```

Tool 引用的 lifetime 安全由 Context contract 保证。

### 7.1 窗口关闭

关闭 `MaterialEditor` 只销毁：

- Lux UI/window local state；
- graph canvas interaction state；
- current node selection；
- local popup state；
- editor command registrations owned by that window。

不会销毁：

- MaterialGraphCompiler；
- `AssetVfsView` / `AssetReadPort`；
- ExecutionRuntime；
- 已提交、由 EditorApplication root `TaskScope` 持有 operation lifetime 的后台 job。

---

## 8. Feature-local object vs Context

判断规则：

```text
只有一个 Editor feature 需要
    -> 由该 Editor 自己拥有

多个 Editor feature 需要同一份事实/能力
    -> EditorContext 或其中的 shared capability
```

### 8.1 应进入 Context 的候选

- Toolset；
- `AssetVfsView` / `AssetReadPort`；
- execution capability；
- UISession / CommandRouter access；
- SceneMetaManager；
- plugin contribution catalog；
- active/global Selection（若 multi-window UX 证明需要）；
- 不包含 AssetIndex；Wave D/v1 明确 VFS-first。若未来需要索引，必须单独架构 review。

### 8.2 不应进入 Context

- MaterialGraph / FlowGraph；
- graph node selection；
- canvas zoom/pan；
- current asset picker popup；
- Scene camera orbit state；
- per-document undo stack；
- individual compilation revision input。

---

### 8.3 Product-wide capability 与 EditorContext 的边界

`AssetVfs`、`ExecutionRuntime`、`RenderRuntime`、`SceneMetaManager` 等不是因为 Editor 会使用就变成 Editor-owned service。

- mutable VFS mount/unmount 由 EditorApplication/project composition 管理；window 只拿 `AssetVfsView`；
- async asset read 通过 `AssetReadPort` / L2 asset_loading，不让 window 或脚本同步读取磁盘；
- `ExecutionRuntime` 有显式 owner/shutdown；
- `RenderRuntime` 继续使用 capability injection，不做 global singleton；
- process-wide logging/assert/crash facade 可单独存在，但不得把这种例外推广成通用 service-locator 模式。

---

## 9. Context 不是 Service Locator

必须有 source-level/code-review 规则防止 Context 无限膨胀。

加入新 member/accessor 前必须满足：

1. 至少两个独立 L5 feature 真实消费；
2. 生命周期确实与 EditorContext 相当；
3. 不能更合理地由现有 Tool/Resource owner 提供；
4. accessor 表达明确能力，而不是 generic registry；
5. 不形成低层到 L5 的反向依赖。

禁止引入：

```text
EditorServices
ServiceRegistry
EditorServiceProvider
EditorContext::getAnything<T>()
EditorContext::resolve(string)
```

Toolset 是唯一特例，因为它有明确的“L4 tool capability set”语义边界。

---

## 10. VFS in EditorContext

VFS 对所有资源相关 UI 提供统一 view：

```text
/Engine
/Game
/plugin mounts
/patch mounts
```

Context 可以提供：

```cpp
asset::AssetVfsView EditorContext::vfs() const noexcept;
```

不同窗口不应各自创建/扫描一套资源空间。

项目切换：

```text
unmount old /Game
mount new /Game provider
notify interested editors / republish project-visible resource state
```

`/Engine` 等 process/editor installation mount 可以继续存在。

---

## 11. UISession / CommandRouter / Lux UI boundary

EditorContext 是所有 Editor window 获取 UISession/CommandRouter 的统一路径：

```cpp
context.ui().commandRouter()
```

`UISession` belongs to `modules/function/ui` and is the owner/bridge for Lux UI session state. Editor windows, generated bindings and plugins MUST draw through Lux UI public API (`ui::Frame`/scope/leaf operations or the exact equivalent frozen in Wave U). They MUST NOT include or expose Dear ImGui types.

Command 定义应尽量在 composition/startup 注册；具体 Editor 在构造时绑定自己的 context/activation scope。

例如：

```text
editor.material.compile
editor.graph.undo
editor.graph.redo
editor.asset.delete
editor.scene.delete_entity
```

`CommandRouter` 负责 focus/context route；窗口不互相直接调用。

Dear ImGui context、docking IDs、backend textures 等只能存在于 `modules/function/ui` private implementation。详细 contract 见 `10-lux-ui-foundation-and-legacy-visual-parity.md`。


## 12. 插件贡献

第三方插件应与 first-party 使用同一个公开 codegen SDK。

插件可贡献：

```text
Tool factories / Tool descriptors
Editor window factories
generated Component Inspector bindings targeting Lux UI
generated Graph node presentation bindings targeting Lux UI/NodeCanvas
commands
asset/editor integrations
```

示意：

```cpp
struct EditorContribution
{
    span<const ToolContribution> tools;
    span<const WindowContribution> windows;
    span<const ComponentEditorBinding> component_editors;
    span<const GraphPresentationBinding> graph_presentations;
};
```

EditorApplication/plugin composition phase 安装这些 contribution，然后 freeze；EditorContext 只引用冻结后的 Toolset。

---

## 13. 插件 UI 不要求 runtime reflection fallback

插件开发 SDK 必须公开：

- annotation headers；
- parser/generator CMake functions；
- generated binding ABI/public C++ contracts that do not expose ImGui；
- stable NodeTypeId/Component identity helpers；
- Editor contribution ABI。

标准 UI 路径直接加载生成好的 function pointer/binding table，并调用 Lux UI public API；不在每帧通过 `RefClass/RefField` 动态解释，也不要求 plugin/generated source include ImGui。

Runtime reflection 仍可存在于引擎其他用途，但不是 L5 插件 UI 必需条件。

---

## 14. Plugin unload / hot reload

当前 Plugin hot reload 仍是 held work，因此第一版 L5 不要为 hot unload 设计复杂 lease graph。

第一版约束建议：

```text
Editor plugin contributions install during startup/project extension activation.
While EditorContext is live, contributed code must remain loaded.
Toolset/Pane binding function pointers are valid for contribution lifetime.
```

未来若做 hot reload，再引入 module lease/code lifetime，不要提前加复杂 indirect dispatch。

---

## 15. 测试

### EditorContext

- 所有 child Editor 析构后 Toolset/VFS 仍可用；
- Context shutdown 顺序不会让 Tool job callback 访问已销毁 UI；
- Context 不允许 copy/move；
- Context lifetime probe。

### Toolset

- duplicate install fail-closed；
- get missing tool contract failure；
- find missing returns null；
- freeze 后 install 被拒；
- requestStop 会传播到支持 stop 的 tools；
- tool destructor 在 Execution/VFS 之前。

### Plugin

- generated contribution 安装；
- duplicate type identity rejection；
- plugin Editor 可以从 Context typed get 自己的 tool；
- plugin UI binding 不需要 runtime RefClass；
- plugin/generated binding target 不需要 ImGui include path/link dependency。

---

## 16. 禁止项

```text
No EditorContext singleton.
No AssetVfs singleton/static global state.
No EditorContext ownership of product-wide runtime infrastructure.
No arbitrary service locator.
No Manager/Services bag.
No Pane-owned VFS/executor/compiler lifetime.
No universal ICompiler base.
No runtime-reflection fallback requirement for plugin UI.
No ImGui/imgui-node-editor types in Editor/plugin/generated public UI contracts.
No plugin unload complexity before a real hot-reload requirement.
```

---

> Coding implementation MUST also comply with `08-normative-execution-contract.md`.
