# 10 — Scene/View/Target 与 Effect Attachment 执行关系

## 10.1 中立对象

Scene 真正拥有 View（每 Scene 局部生成式 ViewId）；View 保存 Camera 与 view-only logical facts，不存 EffectId、GraphId 或 TargetId。Target 保存 offscreen/surface、颜色深度接口、format/samples/extent/placement rules，但**不保存第二张可编辑的 EffectBinding 列表**。Backend 唯一持 `EffectId → [(SceneId,ViewId,TargetId,placement,...)]`，并以生成式 ID 校验三元组与 Runtime owner。

## 10.2 attach 与派生计划

`createView(scene)` 不隐式创建特定 Effect、Native Graph 或针对某个 View 的全量 GPU 资源。`attachEffect(effect,view,target,placement)` 在本帧合法 owner-thread 完成编译需求检查、ScopeKey 资源租约和 PreparedBindings，最后发布关联。`detachEffect()` 只移除指定关系；删除 Scene/View/Target 时按引用关系撤销/拒绝、GPU 退休，不扫描/写入反向字段修补状态。

**不引入长期 ExecutionJob/GraphViewGroup 两套可编辑类型。** 如果实际 Record 性能需要有序数组，Backend 可冷态构造值型 `PreparedSchedule` 或直接按 Attachments 组织有序记录，唯一可编辑使用关系仍是 Effect Binding。相同 GraphId 可以在合法序列中的多个非连续位置出现，不能为减少 PSO binds 改变 Layer/Target 的 producer-consumer 顺序。

## 10.3 多 Effect → 一个 Target

两个 Effect 渲染同一 Target：Compiler 生成本 Target 的完整合成候选，Effect 的子逻辑图先经过 namespace remap，按 explicit layer/blend/dependency 合并 Pass 与最后 Composite/Resolve。这个组合 IR 不成为另一份用户可编辑 RenderEffect/逻辑 Graph；Native 编译产物由 Session 内统一 code 容器持有。Effect 更新影响该 Target 的全体组合消费者，必须按闭包一次安全发布而非半切换。

## 10.4 视图与历史状态

View Camera 可以是 View scope 数据，供不同 Effect 读取同一视角。但 Temporal/TSR/HZB 等历史若依赖 Effect/Target/Render Scale/Shader History ABI，必须用 Attachment-private 作用域或显式相容性证明，不仅通过 ViewId 推断可复用。Resize、camera cut、jitter/DSR/Effect ABI changes 必须改变 epoch 或正确 fallback。Full final output 以及 imported another Target completed image 必须声明 GPU ready/access state。

## 10.5 本期限定

一个 RenderBackend/一个 DeviceSession/一个 GPU；支持多 Scene、多 View、跨 Effect/Target 关联，但 P2-A 当前真实 GPU 业务只覆盖固定 offscreen readback；WSI、resize、complete multi-effect Target、temporal histories 与 Legacy UI 仍待后续 P3/L stage，不能宣称已具备。独立帧节奏 10Hz ECS + 144Hz Render 的 R2 合同继续保留。
