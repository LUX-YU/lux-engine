# 04 — RenderGraph 与 Vulkan Foundation R2

## 1. 两类基础机制分开

RenderGraph：

```text
logical scheduling / dependency
```

Vulkan Foundation：

```text
native GPU mechanism
```

禁止合成“VulkanRenderGraphCore”。

## 2. Logical RenderGraph

只描述：

```text
logical resource
pass
read/write
usage
dependency
logical target semantic
logical lifetime
validation
```

Public graph 不出现：

```text
VkImage
VkBuffer
VkCommandBuffer
VkImageLayout
VkPipelineStageFlags*
queue family index
```

## 3. Graph 三阶段模型

必须明确：

### `RenderGraphDefinition`

```text
logical topology
passes
logical resources
semantic target/resource declarations
```

只在 topology 变化时重建。

### `CompiledGraphPlan`

```text
dependency order
queue assignment
barrier plan
physical resource plan
record plan
```

由 backend compiler 生成并缓存。

### `FrameGraphBindings`

当前帧 concrete facts：

```text
frame slot
current imported target images
View/Scene frame data
external waits/signals
dynamic offsets/handles
```

steady frame：

```text
CompiledGraphPlan + FrameGraphBindings → record
```

禁止：

```text
每帧 builder → analyze → compile → record
```

## 4. Graph cache

cache key 只包含真实影响 topology/plan 的事实。

当前可包括：

```text
feature topology
target layout
device capability variant
```

未来可加入：

```text
view profile
```

但没有真实 consumer 时不实现 ViewProfile framework。

稳定 Scene：

```text
compile_count ≈ 0/frame
```

## 5. Graph telemetry

cheap counters：

```text
compile_count
cache_hit
cache_miss
invalidations_by_reason
```

profiling build：

```text
compile CPU duration
record CPU duration
```

## 6. Pipeline/Descriptor creation 不是 record hot-path 行为

Graph record / draw hot path 禁止：

```text
lazy create VkPipeline
lazy create VkDescriptorSetLayout
compile shader
grow global cache
```

这些必须在：

```text
Feature attach
resource rebuild transaction
graph/topology invalidation cold path
explicit asynchronous/cold build path（未来真实需求时）
```

完成。

record 时缺失结构 invariant：

```text
fail-fast / prior validation failure
```

不是现场补建。

## 7. 迁移 V1 Graph 算法

高价值算法：

```text
dependency analysis
execution plan
barrier synthesis
layout/resource planning
multi-queue scheduling
recorder
```

流程：

```text
先写 V2 input/output contract
保留 V1 test vectors
迁 algorithm/invariant
不复制 V1 owner hierarchy
```

## 8. Vulkan Foundation

### `device/`

```text
VulkanInstance
VulkanDevice
physical selection
queues
VMA allocator owner
device caps
```

### `memory/`

```text
buffer/image owners
staging primitives
arena/ring primitives
```

### `descriptor/`

```text
descriptor layout/pool/set mechanisms
```

### `pipeline/`

```text
shader-module owner
pipeline layout
pipeline templates
pipeline cache/manager
```

### `transfer/`

```text
staging → GPU scheduling
```

### `retirement/`

```text
fence-proven serial
DeferredDestroyQueue
FrameRetireScheduler
private native-backing retirement
```

Foundation 0 knowledge of RenderScene/built-in Feature/ECS。

## 9. RAII leaf rule

native owner：

```text
move-only
complete on construction
no public init/shutdown
fallible create = expected factory
destructor releases native owner
```

禁止 `initialized_` 补偿状态。

## 10. Candidate-before-publication

创建/重建：

```text
prepare complete candidate
failure → local candidate destroys
success → no-fail-ish adopt/swap
old backing remains valid until successful replacement
```

## 11. `vkDeviceWaitIdle`

正常资源生命周期禁止用它简化 ownership。

只允许：

```text
explicit global barrier
device teardown
极少数有独立测试证明无更细 mechanism 的 cold reconfiguration
```

## 12. ResourceRegistry 定位

registry 仅用于：

```text
cold composition
stable owner publication
```

Feature attach 时 resolve typed pointer/handle once。

每帧不得反复：

```cpp
registry.find<T>()
```

结构 invariant 构建阶段验证，hot path 不加 silent null guard。

## 13. `std::function`

cold configuration 可慎用。

每帧 hook：

```text
concrete call
function pointer + state
small prebound ops
```

默认禁止 capturing `std::function`。

## 14. 完成门禁

```text
logical graph target 0 Vulkan include/link
Definition/CompiledPlan/FrameBindings 分界有测试
stable graph 0 redundant compile/frame
record hot path 0 pipeline/layout lazy creation
native fault injection
candidate rollback
ASan CPU ownership path
GPU validation smoke
0 Feature include in Vulkan Foundation
```

## 15. R2：结构变化与帧值变化

`RenderGraphDefinition` 的拓扑变化（pass/resource 集合、resource usage、target format/layout、feature topology）才使 `CompiledGraphPlan` 失效。Frame slot、View camera/transform、dynamic offsets、imported backing revision、临时运行参数等不改变拓扑的值，必须通过 `FrameGraphBindings` / record context 注入，不触发编译。

不提前抽“任意图形脚本调度器”；必要的条件执行可使用编译期固定 pass + 本帧 binding/skip 条件，只有真正拓扑变化才重建。V2 性能回归应分别量测 `graph_compile_count` 和 `record_cpu`，防止以“所有 Pass 每帧重建”悄悄回归。
