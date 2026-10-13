# 26 — Attachment RAII、资源使用权和 GPU Ticket 的反例矩阵

## 26.1 问题：把 Scene 析构当作 GPU Storage 析构

错误实现：`~Scene() { delete scene.lightStorage; }`。真实 GPU 资源可能同时被 E1/E2 多个 Attachment 使用或还有未完成的 GPU submit；并且 Scope 存储的 Owner 本就位于 DeviceSession。正确行为：Scene 删除请求由 Backend owner-thread 确认/撤销关联，Attachment release CPU leases，GPU 资源等真实 Ticket 完成退休，最后使 Scene 的 SlotKey 失效。若选择 Busy 契约，则需先 detach；不能在 Scene destructor 里直接调用 Native `waitIdle` 或手动 destroy。

## 26.2 问题：同 View 不同 Target 历史复用

E1/ViewA/T1 的 TSR 与 E2/ViewA/T2 的 TSR 即使 ViewRef 相同也可能采样不同 width/format/jitter/Shader history ABI。`ResourceScope::Attachment` 必须把 EffectId、TargetId、具体兼容签名视为 part of key；View-scoped camera 可共享，但 History 不能默认按 ViewId 唯一。

## 26.3 问题：编译器以 shader 类型猜 Scope

只有 `buffer<LightRecord>` GPU read 并不证明其数据来自 scene；同型 Preview Lights 和 Main Lights 可能两个不同 DataKey。Compiler 必须看到 Feature/DataSchema 的 explicit requirement，验证 writer/access/schema，再生成 BindingSlot；缺失/冲突 cold-fail，不按注册顺序/字符串首匹配 fallback。

## 26.4 问题：热态查 Registry

编译计划 `[bindingSlot4]` + attach 准备的 native descriptor/Buffer/Ready 允许 executor 直接绑定。不允许每次 pass 查询 `session.domain(sceneId).find<T>()`、重新创建 Shader layout 或在没有结构变化时 rebuild Graph。资源更新通过 typed dirty/version 驱动必要 native descriptor patch，而非高频全局数据重建。

## 26.5 问题：多 Effect 同 Target 多次绘制

Target T 的 E1/E2 都输出中间片段；不可各自以 `execute(Ei,view,T)` 宣称已生成完整终图，再在 WSI 额外开启隐藏 Composite。Compiler 必须派生完整 Target Graph/Pass/Output，并由 Backend 在 Effect 更新时一次整体切换；若不同目标有显式依赖，遵守 Ready Ticket。

## 26.6 问题：Plugin 卸载和代码回调

插件提供 `GridProjection` 的 callback/Feature Ops/Native Resource destructor；Host 可能在 GPU 提交后继续使用其 Shader/native callbacks。CodePin 必须覆盖最后的 CPU callback 和 GPU/Native owner teardown。跨 DLL 只传版本化 Descriptor/opaque token，不能依赖 C++ RTTI/owning STL。

## 26.7 非 OOM 失败与 fatal OOM

普通参数/身份/DeviceCaps/ABI/Writer 冲突可恢复，在候选事务下完整拒绝且 last-good。OOM 按项目 Fatal 策略终止；不要把历史 P2-A “vk OOM 注入后可重试”作为新协议未来必须保留的能力。Native partial submit/device lost 必须保持已被接受 work 的 ticket 与 teardown 安全，而不假设总提交成功。

## 26.8 精确阶段反例见 appendix

`appendix/ExecutionScenarios.csv` 给出至少三十个 scenario 的阶段、Output Oracle、失效条件；必须区分已有 P2-A 曾通过的 B01–B21 历史事实与新增 RD3-R3 设计门禁。脚本不可因为被测功能未实现就将其全部标为 PASS。
