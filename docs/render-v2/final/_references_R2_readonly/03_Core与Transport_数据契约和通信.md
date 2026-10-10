# 03 — Render Core 与 Transport：数据契约和通信 R2

## 1. 核心原则

> **业务边界 typed；跨线程 transport erased；backend dispatch 后立即 typed。**

业务 API 不暴露 `void* + TypeId`。

## 2. RenderData

RenderData 是普通、明确语义的 value type。

```cpp
struct MeshInstanceUpdate final
{
    RenderSceneId scene;
    RenderEntityId entity;
    RMeshHandle mesh;
    RMaterialHandle material;
    RenderSpatialTransform3D transform;
    std::uint32_t flags;
};
```

禁止：

```text
IRenderData
variant<AllRenderData...>
GenericPropertyBag
```

## 3. Stable semantic identity

跨 runtime/plugin composition 的 stable types：

```text
RenderDataTypeId
FeatureTypeId
SceneCapabilityId
```

使用：

```text
canonical semantic name
+
全仓统一 stable identity primitive
```

例：

```text
lux.render.data.mesh.instance.upsert.v1
lux.render.data.mesh.transform.v1
lux.render.data.view.camera.v1
```

禁止：

```text
registration index
DLL load order
std::type_info address
process-random hash
Render 自建第二套 hash 算法
```

注册时必须验证：

```text
stable id collision
wire/layout version
operation kind/lane
reply contract
```

## 4. Runtime-local route identity

transport hot path 不直接使用 stable semantic ID 做 map lookup。

注册：

```text
RenderDataTypeId
    ↓
RenderRouteId   // runtime-local compact numeric id
```

producer composition：

```text
RenderDataTypeId
    ↓ resolve once
BoundRenderRoute<T>
```

后续热路径：

```text
BoundRenderRoute<T>
    → known RenderRouteId
    → packet encode
```

`RenderRouteId` 不持久化、不跨 runtime 使用。

## 5. 复用现有 typed operation codegen

V1 `LUX_OP` 已经表达：

```text
Payload
lane
kind
reply
blob
opcode
```

V2 不创建第二套协议 generator。

author declaration 生成：

```text
stable semantic descriptor
compile-time lane/kind traits
typed encode thunk
backend typed decode/handler thunk
reply traits
blob packing metadata
registration record
```

## 6. 三条 Lane 与线程模型

### PROGRAM

用途：

```text
transform
camera
light
visibility
animation/bones
dynamic material state
instance state
```

**并发 contract：owner-thread single producer → backend single consumer。**

目标底层：SPSC semantics。

不得因为“未来灵活”默认改成 MPMC。

### CONTROL

用途：

```text
Scene/View/Target lifecycle
Feature lifecycle
query/stats
resource destroy
cold configuration
```

**并发 contract：owner-thread single producer → backend single consumer。**

目标底层：SPSC semantics。

需要其它线程发 control 时，先进入 owner execution domain。

### UPLOAD

用途：

```text
mesh vertex/index
texture pixels
point-cloud chunks
large material graph
resource pages/chunks
```

**并发 contract：multiple producers → bounded upload admission。**

目标底层：MPMC semantics。

这三种 contract 是架构事实，不由实现 LLM自行放宽。

## 7. Lane 由类型静态决定

调用方：

```cpp
writer.write(transform_update);  // PROGRAM
uploads.submit(mesh_upload);     // UPLOAD
control.request(create_view);    // CONTROL
```

禁止：

```cpp
send(EOperationLane::UPLOAD, transform);
```

错误 lane 应在编译期不可表达。

## 8. Compile-time dispatch，不在每条消息 runtime switch

`write<T>()` 不得：

```cpp
switch(RenderOpTraits<T>::kind) { ... }
```

让每条消息执行 generic 分支。

generator/traits 应生成专门的：

```text
writePOD<T>
writeBulk<T>
writeBlob<T>
request<T>
upload<T>
```

或等价 compile-time specialization。

runtime erasure 只发生在 transport/dispatch 边界。

## 9. 大资源与实例状态分离

例：

```text
MeshAssetUpload       UPLOAD
MeshInstanceUpdate    PROGRAM
MeshTransformUpdate   PROGRAM
DestroyMesh           CONTROL
```

不要万能 `MeshRenderData{operation,...}`。

## 10. RenderDataWriter

`RenderDataWriter` 只把强类型 PROGRAM data 写进当前 owning program candidate。

它不：

```text
查 Feature
管理 asset/resource
提交线程
等待 IO
做 dynamic route lookup
```

Projection/producer 持有 prebound route。

## 11. Packet ownership

accepted packet 不借 caller stack/vector/string 生命周期。

必须：

```text
inline payload copied
blob packet-owned or pinned by explicit shared owner
attachments own/pin source data
reply state owns completion data
```

禁止：

```text
submit() 返回后仍借 caller 临时 span 指向的数据
```

## 12. Backpressure

Backpressure 是正常 protocol result。

PROGRAM：

```text
RenderSystem retains complete owning candidate and retries same packet
```

UPLOAD：

```text
typed capacity/backpressure result
domain retains data and chooses retry
```

CONTROL：

```text
bounded admission + explicit result
```

禁止：

```text
spin
hot-path blocking wait
unbounded fallback
silent drop
```

## 13. 保留 V1 的成熟机制

迁移，不重写算法：

```text
bounded queue core
packet/blob storage
Upload byte accounting
reply routing
pending reply retry
BlobRef
owned ExternalDataRef attachment
stop/wake
generation-safe runtime-local route slot
```

V2 重写的是 owner/interface，不是成熟 queue/packet 算法。

## 14. PROGRAM/CONTROL 热路径目标

正常 steady state：

```text
0 heap allocation
0 std::function
0 string comparison
0 runtime reflection
0 RTTI
0 mutex
0 shared_ptr refcount churn
0 dynamic route map lookup
```

允许：

```text
known local RouteId
span
memcpy
retained vector capacity
direct/concrete call
function pointer at erased dispatch boundary
```

## 15. Errors

普通失败：

```text
lux::cxx::expected<T,E>
```

保留精确协议错误：

```text
payload size/alignment mismatch
unknown/stale route
reply mismatch
capacity exhausted
stopping
```

禁止：

```text
bool + out-error
invalid handle as failure
catch(...) and continue
```

## 16. Micro benchmarks

从 Transport 完成阶段开始固定：

```text
1M empty/minimal PROGRAM encode
1M POD write
bulk write
blob write
request/reply
MPMC upload contention
queue full/backpressure
```

同时记录：

```text
alloc count/bytes
ns/op p50/p95
branch/CPU profile as available
```

性能结论必须同机 before/after。

## 17. 完成门禁

```text
PROGRAM owner-thread SPSC contract tested
CONTROL owner-thread SPSC contract tested
UPLOAD MPMC contract tested
stable semantic ID 与 local RouteId 分离
hot producer holds bound route
0 Vulkan/Scene/ECS dependency
accepted payload lifetime safe
backpressure deterministic
typed writer has no per-message generic lane/kind switch
```

## 18. R2：跨 Lane 因果关系与资源安全

PROGRAM、CONTROL 各自 FIFO，UPLOAD 有自己独立的多生产者准入；**三条 Lane 之间默认不存在全序**。不得因为使用同一 `RenderRuntime` 就假设 `createScene`、`uploadMesh`、`upsertInstance`、`destroyMesh` 会按调用墙钟顺序到达 backend。

必须明确每一种依赖的协议门槛：

```text
Scene/View：create accepted/ready → dependent program admitted/executed
Resource：UPLOAD completion/receipt → publish resolved handle → PROGRAM references
Destroy：先停止新的引用发布；旧 packet/in-flight GPU references 清偿后释放 native backing
```

可以使用当前已有 receipt、generation-safe handle、per-FIF fence/retire serial 与拥有型 packet。**不增设全局 EventBus、通用事务总线或跨 Lane 全序队列**。跨 Lane 需排序的仅那些真实有因果依赖的操作。

具体提交/dispatch 边界在 R2/R5/R7/R9 的真实 vertical slice 固定；R0 只审计现有因果关系并列为测试向量，不能擅自发明第二条 transport。

必须有负例：尚未 ready 的资源不能被解引用；已请求销毁但仍被 in-flight frame 引用时不能提前销毁；跨 Lane 交错不导致 stale handle 误用。正式 V2 侧 GPU 行为测试只能在相应阶段报告 PASS。

## 19. R2：线程模型不是对现有消费者的未经核实断言

PROGRAM/CONTROL 的 owner-thread SPSC 是 V2 设计合同，但 R0 必须先清点 V1 的实际 producer/call sites；若有独立线程直接发 control/state，迁移时需在真正 owner domain 汇合。不得暗中把 SPSC 放宽成 MPMC，也不得把原多线程生产者直接塞入 SPSC 造成数据竞争。与合同矛盾时按第 10 文档 STOP 上报。
