# R3-A — Attachment RAII / ScopeResources / Context 所有权闭环

**状态：设计工作单，未经用户授权不得施工。** Base 查当前 HEAD；输入 `00/02/04/09/10/14/19/20/21/22/26/28`。R3-A 是 P2-A 已实施代码的有界语义收敛，不重写 Feature/Graph compiler 的全部 API，也不恢复 Legacy Mesh 业务。

## 目标

1. 将 `InstanceContext` 的 Native RAII owner 从 P2-A DeviceSession 移入 Native bootstrap，Session 借用它；保证 Session teardown 的真实 Native GPU 完成在 VulkanInstance/Debug 消毁前。保留 DeviceContext 的 VMA/Queue 及 ResourceContext 的已有 Native borrowed queue/staging 关系；不造 ContextManager。
2. 用**唯一 EffectId→(SceneId,ViewId,TargetId)** 表中的 move-only Attachment 持 resource-usage Lease/Native prepared context 引用；消除独立长期 `ExecutionJob` 的可编辑重复事实。若为热帧需要派生有序数组，必须冷态建立且仅含非拥有引用。
3. 证明 ScopeKey `Global/Scene/View/Attachment` 的区分，先用一个 Native Buffer/1–2 typed Storage 或现有 P2-A BufferFactory。Session 内采用唯一 `ScopedResources`/现有 Domain 的有界重构，不能新旧 Domain+ScopedStore 双轨长期并存。Scene/View 析构前撤销全部相关 Attachment；Global resource 被其他 Scene 使用时不释放；同 View 不同 Target Attachment-private history 不串。
4. 资源 acquire/release 与真实 accepted Queue Ticket 分离：Attachment CPU 逻辑退场后 GPU 正在使用的 backing/Descriptor 安全保留；Native 提交失败部分成功路径不制造假的 completion。Oom 策略 Fatal；普通错误 last-good。
5. Client/Engine 中立，禁止 `RenderRuntime`/Scene/View 头 include Vulkan 或暴露 Session 私有 Native handles。

## 最小可运行用例

- E1/S1/V1/T1 与 E2/S1/V2/T2 共用 Scene LightBuffer；detach E1 后 E2 数据仍正确，卸载 E2 后资源按 retention 进入可回收；Native submit 时机可控并由 ticket 证明。
- S1 与 S2 共用 Global Mesh-like buffer，删 Scene1 后 Scene2 输出不变；没有 GPU backing 双重销毁。
- ViewRef 完整双生成式身份；同一个 View 在 E1/T1 和 E2/T2 有两个不同 attachment-private buffers；不会错误复用。
- 2 Scenes×4 Views、FIF2/3、shared compiled NativeGraph 的旧 P2-A GPU oracle 继续有效；in-flight detach 后重建 ID 不复活旧引用；终止时 Native owner count 归零。
- InstanceContext 提前销毁被静态/运行时 owner 关系禁止；真正合法 teardown 验证 GPU+WSI 借用没有悬挂。

## 本阶段不做

完整 P2-B Feature 作者 SDK/ResourcePlan 编译、engine ECS DataProjection、P3 mixed Effect Target、P4 全 Mesh/Texture/Light Storage、D3D12 实现与 H1–H6；不为了设计文件目录齐全拆 Native R4 全部底层。阶段 I/clean V/STOP，报告说明旧 P2-A TypeOwnership 中仍被替代的条目及删掉的重复缓存表。
