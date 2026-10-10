# 10 — 验收门禁与 LLM 实施合同 R2

> 本文件对所有 Render V2 实施 LLM 具有最高执行优先级。任何“更安全 / 更通用 / 为未来兼容”的理由都不能绕过这些规则。

## 1. 每阶段开始

必须：

```text
resolve exact remote HEAD
verify previous implementation/verification SHA
read current-stage docs + immediate lower-layer docs
audit current consumers
record ALLOWED / READ-ONLY / FORBIDDEN paths
```

不得从聊天记忆猜代码状态。

## 2. 禁止跨阶段

当前 R3 时不得顺手：

```text
改 Feature
改 Scene
改 Editor
优化 Mesh
重命名所有 Context
```

非阻塞问题只记录。

## 3. Allowed paths

每次实施指令必须显式：

```text
ALLOWED:
READ-ONLY REFERENCE:
FORBIDDEN:
```

改 forbidden path 默认阶段 FAIL。

## 4. 禁止补偿性架构

未经文档/用户批准，严禁新增：

```text
*Manager
*Controller
*Coordinator
*ApplicationHost
*CompatibilityLayer
*LegacyAdapter
Universal*Registry
ServiceLocator
IRenderBackend
```

特别禁止重新创建独立 `render_feature` execution component。

如果设计难实现，STOP 报冲突，不造中间层。

## 5. 禁止过度防御免责

invariant path 不得：

```cpp
if (!x) return;
if (!initialized_) return;
if (bad) continue;
```

先分类：

```text
合法可缺失 → optional/expected
boundary input error → typed expected error
结构不变量 → construction validation / fatal contract
```

静默不渲染不是“安全”。

## 6. RAII hard gate

新增 owner：

```text
complete construction
move-only/value ownership
fallible create → expected
no public init/shutdown
destructor releases own native backing
```

新增：

```text
initialized_
initError_
isInitialized()
```

默认 FAIL。

## 7. Semantic zombie 禁止

public semantic owner 不进入：

```text
CLOSING but unusable
valid=false but still semantically present
closed-but-existing
```

native backing 延迟释放 → private retirement owner。

## 8. 三类 Dependency 不得混用

必须区分：

```text
RenderDataTypeId       Scene→Backend input
SceneCapabilityId      persistent Scene provider/consumer
RenderGraph resource   current graph read/write
```

禁止 generic `DependencyId` / one `requires[]` 混装三者。

## 9. Stable ID / Route ID contract

稳定：

```text
RenderDataTypeId
FeatureTypeId
SceneCapabilityId
```

runtime-local：

```text
RenderRouteId / OperationId
```

禁止 hot path canonical hash/map lookup。

route composition 时 bind once。

禁止 Render 自建第二套 stable hash primitive。

## 10. Thread model hard gate

```text
PROGRAM  owner-thread producer → backend
CONTROL  owner-thread producer → backend
UPLOAD   MPMC
Backend Scene/Frame state = backend owner thread
```

未经明确设计批准不得把 PROGRAM/CONTROL genericize 成 MPMC。

## 11. Transport mechanism 不能为“简化”破坏

禁止：

```text
bounded → unbounded
three lanes → EventBus
expected → bool
code pin → raw function pointer
reply ownership → caller borrow
```

## 12. SceneCapability 只做 non-owning projection

Capability table 不拥有第二份 semantic state。

必须能回答：

```text
真实 owner 是谁？
capability index 指向谁？
consumer attach 后缓存什么 typed pointer/handle？
```

执行 hot path 不查 capability registry。

## 13. RenderResourceDomain scope hard gate

默认：

```text
one RenderResourceDomain per RenderRuntime integration domain
multiple RenderSystems/Scenes borrow it
```

不得默认每 Scene 新建 domain 导致 Mesh/Texture 重复上传。

必须区分：

```text
runtime-shared asset resource
Scene-local semantic resource
Feature/backend-local resource
```

## 14. Device Lost policy

`VK_ERROR_DEVICE_LOST`：

```text
terminal RenderRuntime failure
stop new semantic work
host recreates a NEW RenderRuntime
```

禁止自行增加：

```text
RECREATING
RESTORING_SCENES
RESTORING_FEATURES
```

自动恢复 framework。

## 15. Graph compile / execute boundary

steady record path 必须使用：

```text
CompiledGraphPlan + FrameGraphBindings
```

禁止每帧重新 compile。

record hot path 不允许：

```text
lazy VkPipeline creation
lazy descriptor-layout creation
shader compile
```

## 16. 热路径 hard gate

未经 profile + 用户批准，稳定 frame/entity/draw path 禁止新增：

```text
new/delete
vector capacity growth
std::function
std::string lookup
runtime reflection
RTTI/dynamic_cast
shared_ptr refcount churn
mutex
unordered_map dynamic route lookup
generic kind/lane switch per message
```

## 17. Projection complexity gate

正常增量 Projection：

```text
~O(changed entities)
```

禁止 stable frame 全量扫描所有 entities/published state 侦测变化。

只有 explicit full-sync/rebuild 允许 O(N)。

## 18. 0-allocation benchmark contract

固定：

```text
fixed Scenes/Views/Feature topology
fixed capacities
no asset upload/growth
graph compiled
pipelines/layouts created
warm-up complete
```

统计 first-party owner-thread：

```text
RenderRuntime
VulkanBackend
SceneRenderer
FrameExecutor
RenderGraph execute/record
```

要求：

```text
CPU heap allocation/frame == 0
```

不把 Vulkan driver/OS/明确 growth path 混入。

允许 test-only allocation instrumentation。

禁止为此在 production 新增 generic allocator abstraction。

## 19. 性能不得靠猜

所有“优化”：

```text
before
change
same-machine after
```

没有数据只能称结构简化。

>5% p50/p95 regression 且原因不明确 → STOP。

## 20. 不引入第二 authority

任何 cache/projection/index：

```text
authority = ?
invalidation = ?
cache can mutate semantic truth? NO
```

两个容器都能修改同一事实 → FAIL。

## 21. 静态注册魔法禁止

禁止：

```text
static constructor registrar
DLL-load side effect registration
hidden global registrar
```

显式 registration data + composition。

## 22. Public C++

默认：

```text
no exceptions
no RTTI
expected for fallible
span/string_view for borrow
unique_ptr/value for ownership
strong IDs
```

## 23. Commit / verification

```text
Implementation commit
→ independent qualification
→ verification/docs commit
→ STOP
```

verification 不修 production code。

发现 bug → 新 implementation commit 后从头重跑 qualification。

## 24. Test discipline

禁止：

```text
删失败测试
降低断言
扩大 tolerance 过测试
忽略 validation error
改成未测
```

Test-only seam 必须 internal。

## 25. Final static zero gates

至少：

```text
0 RenderSyncStage
0 RenderFeatureSceneBinding
0 abstract RenderFeature
0 independent render_feature execution component
0 GeneralRenderServer public
0 RuntimeServer
0 engine::RenderContext
0 render::RenderContext owner
0 old RenderContextView / RenderSceneView names
0 RendererThread / FrameRuntime
0 old public include prefix
0 production render_legacy dependency
0 runtime dual-renderer switch
```

## 26. Dependency gates

执行 `02` 全部 negative tests。

此外：

```text
render_vulkan → render_features MUST FAIL
render_runtime → render_features MUST FAIL
scene_render → Vulkan internal MUST FAIL
scene_render → built-in Feature MUST FAIL
```

## 27. GPU gates

受影响阶段：

```text
validation = 0 error
native GPU smoke
resource create/failure/rebuild
shutdown/retirement
device-lost terminal failure
```

最终：

```text
Mesh only
PointCloud only
Mesh + PointCloud same target
multiple views
offscreen
presentation
upload pressure
```

## 28. Installed SDK gates

真实 installed consumers：

```text
standalone RenderRuntime
external RenderFeature
external RenderProjection
headless/offscreen
Scene Render consumer
```

不能用源码私有 include。

## 29. Legacy transplant

每次迁算法报告：

```text
legacy path
terminal SHA
algorithm/invariant
old structures NOT copied
V2 test
```

`render_legacy` read-only。

## 30. 用户修改保护

开工前记录用户工作区差异；不覆盖无关改动。

## 31. STOP 条件

任一：

```text
需要跨 forbidden layer
需要新增未批准 framework
设计与真实代码冲突
需要改变成熟 GPU algorithm 才能满足架构
核心 invariant 失败
性能 >5% 回退且原因不明
发现必须让 capability/resource/data 三种 authority 混用
```

立即停止自动推进并报告。

## 32. 最终代码外观验收

Reviewer 仅从目录/类型名应能回答：

```text
谁拥有 Vulkan device？
谁拥有 RenderScene？
谁拥有 Scene→Render publication？
谁拥有 shared asset resource？
大资源从哪里上传？
实时数据从哪里发布？
Feature 如何扩展？
Scene capability 谁拥有？
多个 Feature 如何同画面？
Graph 何时编译、何时仅 record？
哪个层依赖哪个层？
```

如果必须读大量补偿性注释才能回答，架构未收敛。

## 33. R2 用户开工决定与旧门禁的替代

2026-10-10 用户明确批准以 `a669409a289a6fa4092f21176397795b1cdb7f3e` 作为有已知问题的 V1 frozen reference base **立即启动**。R0 不再需要 MA 旧队列全部 PASS；不过既有失败、NOT_RUN、用户延期不允许改写为 PASS，必须进 `V1_KNOWN_ISSUES.md`。具体 SHA 由 R0 工作单约束。

## 34. R2 Build Qualification 按阶段隔离

R0 完成旧 Render source relocation 后 V2 branch 的 EDITOR/PLAYER/TOOLCHAIN 在 product cutover 前是 `EXPECTED_UNAVAILABLE`；不可为“绿色 CI”添加空目标、compatibility facade、V1 runtime fallback 或双 Renderer 配置。V1 full test 仅在 frozen oracle worktree 记录；R0–R16 用 `cmake/render-v2-bootstrap/` 专用入口及新 Render 子集测试。R17 才要求恢复产品的真实全量构建和 installed SDK。缺失环境保留 NOT_RUN，禁止伪造 PASS。

## 35. R2 Typed Data 与跨 Lane 约束

`render_core` 无 Mesh/Light 等 domain payload；typed schema 在无 Vulkan 的 `render_data`；`render_content` 保存 cooked content；`scene_render` 不依赖 `render_features`。跨 Lane 没有默认全序，资源 readiness、scene/view existence、handle generation、in-flight retirement 必须各有因果门禁；不得以全局 EventBus 代替原三 Lane。Backpressure retained packet 期间 source 的后续 dirty/revision 不得丢失。

## 36. R2 R0 工作单的实施权威

启动期仅执行 `11_R0_实施工作单.md`。当前架构文档为约束，**不是允许自动执行 R1–R18 的授权**。遇到需要新编译兼容层、跨阶段生产修改、未许可删除或缺少 freeze 输入等问题，停止并报告，不推测、不自行扩展 Scope。
