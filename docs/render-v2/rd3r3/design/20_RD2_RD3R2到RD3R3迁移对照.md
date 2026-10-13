# 20 — 已实现 P2-A 与 Legacy → RD3-R3 职责迁移表

## 20.1 实际仓库状态（2026-10-13 检查）

`codex/render-v2` 已前进到 `960e755395be2c9f78a86339a062c507826c4eb4`（P2-A V），父 I `483d20ed900b8c226a740704f3c888498b1dae11`。真实 P2-A 已做 DeviceSession、三层 Context 结构（但 InstanceContext 在 Session 内）、SlotMap<unique_ptr<ResourceDomain>>、Scene/View DomainId、EffectBinding 三元组、派生 ExecutionJob、GPU native graph sharing；文档报告 Windows x64 43/43 scoped tests PASS、2 Scene/4 View/FIF2/3 validation 0。**我未在本环境重跑这些测试；状态属于报告证据。**

## 20.2 必须调整的真实 C++ 状态

| P2-A 真实源码形态 | 新归属/做法 | 阶段 |
|---|---|---|
| `runtime/include/.../DeviceSession.hpp::InstanceContext` 成员 | native bootstrap owns stable InstanceContext; DeviceSession borrows; actual owner lifetime verified | R3-A |
| `DeviceSession::{domains_, domain_order_}` | 保留 native store 算法为内部 ScopeStorage candidate；把动态需求/RAII leases 绑定到 attachments，最终不再公开 DomainId 做资源权威 | R3-A / R3-B |
| `runtime/ResourceDomain.hpp` typed virtual entries and local `getOrCreate` | 复用 local type ownership 与 factory compatibility，重构成 ScopeKey+ResourceKey、保持 one Native owner/cold API，不追加第二 ResourceRegistry | R3-A / R3-D |
| `Scene.domain`, `View.domain` | 只保留 Scene/View logical facts；GPU ScopeKey 在 attach 根据验证的 SceneId/ViewRef 构造；过渡期按计划删除直接 Domain 依赖 | R3-A |
| `runtime/Objects.hpp::ExecutionJob` | 压缩为唯一 EffectAttachment authority 中的已准备数据 + cold-derived order，不保留可编辑双表 | R3-A |
| `RenderEffect{definition,initial_values,GraphId}` | RenderEffect 统一 own Features + one logical Graph，Native compiled Graph only Session; no second logical cache | R3-B |
| `NativeAssembly/Backend.hpp` 公开 VulkanOptions/EffectSource | 把 Native Factory/Session bootstrap 移到可选择实现的 Native Port，Engine 仅看中立 Client/Wire/Schema | R3-E（最小 API seam 可先做） |
| P2-A `getOrCreate` native Buffer/Factory OOM recovery | 保留 P2-A historical test; RD3-R3 对新业务 API 不承诺 OOM retry，非 OOM conflict/ABI last-good | R3-A |
| Legacy GPU storage & Transfer | 各专业 Storage 迁移保留成熟算法；Use leases + Native last use 替代纯 FIF serial | R3-D / L |
| Legacy `RenderSyncStage` + plugin `RenderFeatureSceneBinding` | 保留 change tracking/prepare-commit-discard/registration/codepin 的算法，变为可注册中立 DataProjection，不让每 Feature 生成 Proxy | R3-E |
| 当前 `vulkan::GraphExecutionContext` | 保留当前 Native storage/descriptor/tickets/slot 管理，绑定真实 ResourcePlan 和 Attachment-private history | R3-B/R3-C |

## 20.3 避免“文档内容变当前事实”

P2-A 不实现：完整 Effect-owned Feature/Graph composition、Compiler ResourcePlan/attach RAII resource sharing、Engine DataProjection plugin、mixed Effect final Target、WSI/history/Legacy 专业 Storage、DirectX、H1–H6。Native Graph sharing 已实测，不等于其它功能完成。下阶段先审核已有 P2-A I/V 的真实 diff 与 Native C++ include/link DAG，再冻结允许改动文件，避免在既有代码上再造一套中间层。

## 20.4 历史来源边界

原 RD3-R2 ZIP `RenderV2_RD3R2_DeviceSession_EffectGraph_Architecture_2026-10-12(3).zip` 的全部 86 F/I/H、25 W、51 REQ 与 16 Legacy GPU 算法追踪作为输入保留并更新；旧 ZIP 不包含本轮讨论最终 Scope lifetime/Projection/Instance ownership 决策。Legacy freeze `a669409a289a6fa4092f21176397795b1cdb7f3e`。原有 I/V 和历史性能 PARTIAL 不可在设计改写中悄悄改判。
