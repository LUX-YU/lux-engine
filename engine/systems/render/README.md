# Engine RenderSystem

`system_render` 是可选的静态 SceneSystem 组件，公开路径为 `lux/engine/systems/render`。
Engine 调度类只推进 SceneSystem。启用渲染时，由 EngineContext 拥有 Render 模块的 RenderClient、
RenderFrameSubmission 和服务线程；它们必须晚于 Scene、上传器和资产仓库销毁。

## 装配与所有权

RenderSystem 借用 World、EngineContext 和能力元目录，直接拥有远端 Scene/Effect 身份、Reaction 和 Collector。
EngineContext 拥有基础设施，RenderSystem 通过 execution()/assets() 借用通用服务。
Context 不公开 UI 或专业上传器；MeshUpload/MaterialUpload/TextureUpload 由专业装配方持有，必须覆盖仓库用途清偿。

RenderSystem::create只构造本地对象。Engine接管Scene后，SceneSession拥有其TaskScope，
RenderSystem::initialize在该范围内消费完整能力选择、创建Effect/Scene并装配行为，不再查询整个能力目录。失败时所有已返回句柄仍归System，
由Engine统一释放；不需要外部RenderScene候选或swap。一个Effect仍可供多个View共享。

```cpp
RenderCapabilityMetaRegistry metadata;
metadata.addSceneReaction<ViewReaction>();
// 在已有异步装配流程中解析；数组存活到这次 co_await 完成。
const std::array names{
    lux::render::CommandName{lux::render::Mesh3dCommands::CreateInstances::name},
    lux::render::CommandName{lux::render::Mesh3dCommands::ReleaseInstance::name}
};
auto ids = co_await context.renderClient().request<lux::render::QueryCommandIds>(names);
if (ids.error.type) co_return lux::cxx::unexpected(ids.error);
metadata.addReaction<MeshInstanceReaction>(
    [&meshes, &materials, create = ids.ids[0], release = ids.ids[1]](
        RenderSystem& system, lux::render::CapabilityImplementationId implementation
    ) noexcept
    {
        return MeshInstanceReaction::create(system, implementation, meshes, materials, create, release);
    }
);
metadata.addCollector<MeshTransformCollector>();
metadata.addCollector<ViewCollector<lux::render::Mesh3dCapability>>();
// 实际装配边界逐个检查登记结果。上面仅展示接口。
// composition 中每项都包含能力 type_info、实现 ID 和配置；output 不含宽高。
auto system = RenderSystem::create(std::move(composition), output, world, context, metadata);
```

元信息工厂绑定实际专业依赖的稳定引用。登记不创建对象、不启动 IO；构造仅建立订阅并保存传入的命令 ID，不查询通信目录。
capability occurrence 的实际实现 ID 和帧入口按值传给对应工厂。Scene 通用 Reaction 不接收无效 ID 占位。
没有 Dependencies 类型扫描或 RenderContextArgument。Editor UI 可绑定给自己的 Collector，Engine 不知道 UI。

System地址先稳定再启动远端初始化。元目录及其捕获依赖必须覆盖System寿命；不能把局部metadata
传给create后立即销毁。正常execute不查询元目录。普通业务对象不拥有进程基础设施。

## 业务协程与 ECS 安全点

RenderReaction 拥有 EnTT scoped_connection 和实体输入协调。
每个实体至多一个根任务；在途集合只记录执行身份，pending/processing 保存两个轮次的脏身份。
完整 Entity（含代际）作为键，已销毁对象的清理不会被同槽位的新实体覆盖。
同帧中间值可以合并，但已接纳的远端请求必须接收并处理真实结果。业务逐次事件不走这个脏集合。
process通过所属System借用的TaskScope准入；reconcile完成准备、等待、采用或清理。
没有AsyncMixin/RetirementMixin、局部任务范围或完成列表；新变化留到后续轮次，不在回调中循环追赶。

Engine 执行 TaskGraph 时不派发全局 Main 回调；全局完成在图结束和 System/Collector 遍历之外派发。
Observer 只记录意图，或移出即将销毁影子的资源，不修改 Registry。根通过 starts_on Main 启动。
Worker 阶段只使用私有输入；采用前回 Main；不得重入消息泵。停止期间仍交付已接受请求。

Mesh/Material 更新触发 MeshInstanceReaction，Transform 的普通更新只触发 Collector。
ViewReaction 处理 View/Transform 的结构变化和 Target 意图；相机参数变化由 ViewCollector 处理。
连接会折入存量组件，组件必须通过 patch/replace 发布需要被观察的更新。

## Mesh 与资产用途

MeshInstanceReaction 消费 Mesh3D、MaterialComponent、Transform3D。资产读取、同用途准备合并与引用查询
由 AssetRepository 提供；纯 RenderUpload 只发送具体命令，缓存两个 CommandId，不持有资产仓库。
MaterialUploadDefinition 组合纹理用途；材质结果持有这些依赖，Material 释放成功后再归还纹理。

候选资源保存在协程帧。准备失败保留旧影子并报告 preparationFailed；完整成功才替换。
真实 Mesh/Material 用途与 Instance 身份由私有 MeshRenderResourceComponent 拥有。
createModelEntities 消费 ModelDescription 并展开静态节点变换，不新增父子 Transform System。
当前 Mesh 资产按导入 primitive 组织，不能把它当成已经具备任意多 Section 数据源。

## View 与 Target

RenderTargetComponent 只描述期望输出，不包含 LuxWindow/Pane。ViewReaction 每 Scene 装配一次，
创建 Target/View/Attachment 并将完整结果采用到纯数据 RenderViewComponent。
组件只声明字段；Reaction 在采用和移出点显式 exchange 清空来源，不能依赖默认移动清空标量身份。

零尺寸暂停现有目标。正尺寸走 Resize、采用真实描述和 epoch、更新参数、恢复目标。
旧 Resize 回复描述已经发生的事实；输入又变时仍更新同一 Target 的实际状态，但不恢复过时输出。
替换 Surface 与普通 Resize 区分，正常 Resize 不重新创建整个 Effect。

影子在异步操作期间被删除时，实际资源移到对应 Reaction 的 removed_resources_。
它只拥有真实资源，不存输入/任务状态；同实体协程结束借用之后再释放。释放失败原 Owner 继续保留。

## 帧与退休

Collector 同步收集脏数据到共用 FrameStorage，不读取文件、不创建 View。
RenderFrameSubmission 在帧开始控制领先帧数，帧结束统一提交；FrameStorage 复用，真实 GPU FIF 仍归 Render。
CPU 回复不等于 GPU 完成，物理回收仍走唯一 cleanup_scope_。

关闭顺序：停止 Scene 调度和新输入 → 清偿全部对象根 → 已提交帧输入消费 → 专业 Reaction 资源释放
→ Scene/Effect 移除 → 资产仓库释放缓存用途 → 上传器/进程 Render 服务 → Execution。
Engine先等待SceneSession唯一任务范围，再撤销System/Reaction的LuxObject回调身份并调用专业资源释放。
专业释放在已关闭的业务范围外被直接await；成功后逆序析构，失败保留剩余Owner。
System/Reaction析构不泵事件；Engine和独立仓库的最终Owner析构在Main外层执行既有清偿屏障。

RenderClient 保留 IPC 请求关联和通用 CommandId，专业请求直接使用 request<Command>(id, request)。
QueryCommandIds 只用于冷态装配，回复不重复携带输入名称；预先解析的 ID 在工厂中按值捕获，不能借用局部结果数组。
预备工厂必须为 noexcept，并返回 RenderResult<unique_ptr<T>>；注册入口在类型擦除前检查该合同，
不接收已擦除的 std::function。空函数指针在注册时返回错误，不进入场景初始化。
EngineContext 只用 CompletionWork 接入 Main 安全交付，不保存第二张请求表。RenderSystem/Reaction
从已有 Context 借用 Client；RenderUpload 保存创建时收到的 Client 和两个专业 CommandId，不使用全局单例。
这些关联并不拥有 stdexec OperationState。Source 的真实 Response/Server 清偿边界保持。


渲染影子与 RenderTargetComponent 具有字段反射，并标记为运行时派生、REBUILD；没有作者创建、
持久化或复制快照入口。反射生成头只位于构建期 component_info，不安装私有实现。AssetDataRef
字段保持引用 RAII，组件本身不定义生命周期函数或发送 IPC。通用“禁止所有成员函数”的生成校验
仍受解析器缺少成员函数模板信息限制，不能把字段生成成功当作该校验已完整实现。

可见性生产者只写 VisibilityComponent::global_requirement，用户开关 is_visible 由业务写入。
Reaction/Collector 合成有效需求；RETAIN 不启动首次上传，隐藏驻留由 RenderableMeshComponent 控制。
ViewCollector 仅记录完整 View/Transform/影子组合；MeshTransformCollector 仅记录已有渲染影子的 Transform。
资产通知只标记本地待处理集合，安全点采用/失效影子后由 EnTT 通知 Collector，不伪造作者组件更新。
