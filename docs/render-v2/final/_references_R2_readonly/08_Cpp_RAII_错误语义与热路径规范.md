# 08 — C++、RAII、错误语义与热路径规范

> 这份文档是全 Render V2 的强制编码规范。实现 LLM 不得以“更安全”为理由违反它。

## 1. Complete Object Principle

Public semantic object 一旦存在，必须满足它承诺的不变量。

禁止：

```cpp
Foo foo;
if (!foo.init()) ...;
if (foo.isInitialized()) ...;
```

改为：

```cpp
auto foo = Foo::create(...); // expected<unique_ptr<Foo>, Error>
```

或 value 可安全 move 时：

```cpp
expected<Foo, Error>
```

## 2. RAII ownership

拥有 native/resource lifetime 的类型：

```text
move-only
析构释放
ownership 从类型和成员可读
不要求 caller shutdown()
```

禁止裸 owning handle 四处传递。

Borrow 明确：

```text
T& / T* / span / view
```

owner 必须明确比 borrow 长寿。

## 3. Candidate / Transaction

Fallible aggregate mutation：

```text
prepare candidate
validate complete candidate
commit no-fail-ish structural adoption
old owner retired/destructed
```

禁止：

```text
边创建边写 published object
失败后靠一堆 bool 修半初始化状态
```

## 4. Error model

Expected failure：

```text
expected<T,E>
```

Structural impossible state：

```text
construction-time prevention
或明确 fatal/assert contract（按仓库 release 策略）
```

不要把 invariant violation 偷偷改成：

```cpp
if (!ptr) return;
```

这会把 bug 变成静默渲染缺失。

## 5. “过分免责式判断”禁止

LLM 常见坏模式：

```cpp
if (!scene) return;
if (!feature) return;
if (!resource) return;
if (!initialized_) return;
if (index >= vec.size()) return;
```

如果调用点 contract 已保证条件成立，这类 guard 是错误设计。

必须先判断：

```text
这是合法可缺失状态？
还是结构不变量？
```

合法可缺失：用 `optional/expected` 明确表达。

结构不变量：构造阶段验证；执行阶段直接使用或 fatal contract，不静默跳过。

## 6. bool + payload

禁止平行事实：

```cpp
bool has_x;
X x;
```

若“无 X”是合法状态：

```cpp
optional<X>
```

若是多状态 protocol：

```cpp
variant / enum state + state-specific storage
```

但不要为了生命周期补偿引入语义 zombie state。

## 7. 不要发明 Manager/Controller

新增类型必须拥有至少一个明确的：

```text
ownership
invariant
runtime protocol
stable value abstraction
```

仅为“把函数分组”不新建 Manager。

不使用：

```text
RenderManager
RenderDataManager
FeatureController
ProjectionManager
ContextManager
```

除非职责无法用真实领域名表达，并经用户批准。

## 8. Context 规则

`Context` 仅表示：

```text
一次 operation / scope 可访问的非拥有环境
```

正确：

```text
RenderFeatureContext
RenderSceneContext
PassRecordContext
```

错误：

```text
DeviceContext (owner)
ResourceContext (owner)
PresentContext (owner)
```

## 9. Runtime 规则

Runtime 必须是 active execution domain：

```text
queue / admission / progress / in-flight / stop / lifecycle
```

单帧 struct 不叫 Runtime。

## 10. no-exception public style

Render public/runtime boundary 不用 exception 表达普通失败。

OOM/fatal policy遵循仓库统一规则，不在局部随意 catch `std::bad_alloc` 再返回模糊错误。

不要：

```cpp
try { ... } catch (...) { return false; }
```

## 11. Hot path：禁止项

每帧 / per-entity / per-draw 热路径默认禁止：

```text
heap allocation
std::function
std::string construction/comparison
RTTI / dynamic_cast
runtime reflection
unordered_map lookup if route can bind once
shared_ptr atomic refcount churn
mutex on owner-thread-only state
virtual dispatch per entity/draw
```

## 12. Hot path：推荐

```text
span
fixed/retained vectors
dense/sparse sets
SoA where data volume warrants
strong numeric IDs
prebound handles/function pointers
per-FIF scratch
clear_keep_capacity
compile-time traits
```

## 13. Type erasure 只允许真实边界

主要允许：

```text
heterogeneous FeatureInstance collection
heterogeneous ProjectionInstance collection
cross-thread transport TypeId+bytes
```

不要在内部到处传 erased value。

## 14. shared_ptr

只用于真实 shared lifetime/code pin/跨线程 passive ownership。

禁止把 `shared_ptr` 当默认 pointer。

Backend owner graph优先：

```text
unique_ptr/value owner
references/non-owning pointer downward
```

## 15. `std::function`

cold configuration 可以慎用。

热路径和每帧 hook：默认禁止；优先：

```text
concrete call
function pointer + state
prebound small ops table
```

## 16. Threading

线程模型是 contract，不是实现建议：

```text
PROGRAM  owner-thread producer → backend consumer
CONTROL  owner-thread producer → backend consumer
UPLOAD   multi-producer → bounded upload transport
Backend Scene/Frame state  backend owner thread only
```

不要“为了性能/灵活”擅自把 PROGRAM/CONTROL 放宽为任意线程 MPMC。

其它线程需要提交 Scene state/control 时，应先进入对应 owner execution domain；大型资源 producer 才直接使用 UploadPort。

新增并行必须有：

```text
measured bottleneck
ownership model
stop/lifetime model
benchmark
```

## 17. destructor

Destructor：

```text
noexcept
不等待无界异步工作
不执行复杂业务 callback
不申请新资源
```

需要等待的 barrier 必须是显式 API/host protocol。

## 18. Naming

名称必须表达职责：

```text
Owner: Device, Target, ResourceDomain, Infrastructure
State: RenderFrameState
Runtime: RenderRuntime
Context: operation environment
Registry: authoritative registration/lookup
Catalog: read-mostly projection/declaration set
Lease: ownership/use responsibility
Receipt: completion/status observation
```

## 19. 注释

注释说明：

```text
为什么有这个 invariant
为什么不能更简单
lifetime/thread contract
```

不要写防御性叙事来为丑陋 API 辩护。

如果注释需要 50 行解释一个补偿性类为什么存在，优先重新设计这个类。

## 20. 性能验收

必须从第一 vertical slice 就记录：

```text
CPU frame shell p50/p95/max
heap allocation count/bytes
queue/wake count
graph compile count
pipeline creation count
upload copied bytes
```

### 0-allocation benchmark 的精确定义

固定 topology：

```text
1+ fixed Scene
fixed Views
fixed Feature topology
fixed resource capacities
no asset upload/growth
graph already compiled
pipelines/layouts already created
```

经过 warm-up 后，统计 first-party owner-thread：

```text
RenderRuntime
VulkanBackend
SceneRenderer
FrameExecutor
RenderGraph record/execute infrastructure
```

要求：

```text
CPU heap allocations/frame == 0
```

不把下列混入同一指标：

```text
Vulkan driver internal allocation
OS allocation
第三方库隐藏 allocation（单独报告）
明确的 topology/resource growth
asset upload/streaming growth path
```

允许 test-only global allocation instrumentation；**禁止为统计分配在 production 引入 `IRenderAllocator` / GenericAllocator framework。**


## 21. Hot record path 不允许“懒惰补救”

record/draw 过程中发现结构资源缺失：

```text
pipeline
descriptor layout
required capability
compiled graph plan
bound route
```

不得现场 create/find-and-create 作为容错。

这些必须在 cold composition / attach / compile transaction 中完成。

缺失表示 prior contract 失败；应 fail-fast/报告明确错误，而不是把构建成本和失败路径塞进 hot path。

## 22. R2：功能正确性的性能优先级

热路径门禁不授权绕过 correctness：CPU-side 吞吐优化不得牺牲跨 Lane 的 resource lifetime、revision/backpressure、不变式或 GPU fence 证明。结构拓扑增长、首次编译、首次 asset upload 与故障恢复是冷/增长路径，应分别测量和报告，不得混为“稳定帧每次都 0 allocation”的同一口径。

`noexcept` 的承诺意味着普通 C++ exception 不能外逃；任何可能分配的 `noexcept` helper 必须依照仓库统一的 OOM fatal 策略，而不是在局部 catch 后返回半初始化结果。
