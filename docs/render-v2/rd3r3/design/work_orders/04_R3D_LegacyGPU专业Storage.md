# R3-D — 高价值 Legacy GPU Storage 按真实业务纵向迁移

**状态：未授权，依赖已稳定 ResourcePlan/Attachment/Native Final Target。** 输入 04/05/08/12/16/17/22/23/24 以及 `LegacyGpuAlgorithms.csv`。本阶段先做几个有业务 Consumer 的 Native Storage，后续 L 阶段再按剩余功能扩展。

1. 每种 Storage 先读 Legacy 源文件及 Shader ABI，记录算法和 layout 核心；至少 Mesh(Global)+Instance(Scene)、Texture(Global)+Material(Global)、Light(Scene) 跑通。严格保留 VMA virtual TLSF/chained Arena、4096-record stable pages、stable InstanceSlot/dense workset、Dirty stream coalescing、Texture bindless mip region/residency、transfer staging ring 的价值；若替换必须给真实对比证据。
2. 构建新的 cold Factory/ScopedResources/Lease/BindingProvider 接口；Storage 独占自己内部 GPU Buffers 与 arena，Session 不为每 Mesh/Texture record 增加公共 Scope 条目，Effect 也不拥有 Storage。
3. Engine/Cpu data authority 与 GPU Storage 不复制完整第二 ECS；必要 HostMirror 按原性能需求保留，GPU-only 不生成假 CPU records。真 Queue ticket 与 ABI ready/updating 同时验证。
4. 以两个 Scene 同一 Mesh 根资源+不同 Instance transform、多个 View GPU cull、Light Dirty 0.1%、bindless in-flight delete/mip feedback、PointCloud Chunk 顺序做有限但真实 GPU/VRAM/Dirty/CPU tests。热态 stable topology 无额外 per pass Heap。

禁止用 `GenericGpuStorage<T>` 代替全部专业缓存、任何地方恢复旧 RenderContext service hub、拿 FrameSerial 假冒 GPU Ticket、为每种 Storage 起一个 Manager；按本轮选择的 2–4 类切割 I/V，交付资源迁移证据卡，其余后续 L。


## 必须交付的 Bulk Upload + MaterialGraph/Transient Pool 纵向片段

除已有 Mesh/Instance/Texture/Material/Light 原有指标外，本阶段至少实现一条 `R2 UPLOAD → Server owned Mesh/Texture payload → R4 SubmissionQueue (dedicated 或 aliased) →真实 Ready Ticket → Graph Pass sampling/draw → safe retire` 的端到端用例；两种 queue/family 状态必须按第 31 章测试，不能只通过 `PROGRAM` 提交小值冒充 Mesh/Texture 上传。使用现有 material::MaterialGraph/Toolchain cooker 的真实 cooked shader 构造两个仅参数不同的材质实例，确认 Material GPU 表分别更新、Shader/PSO 共享和纹理 slot 正确，不建立新 material Node 系统。测第 29/32 章 transient pool exact-key+recompile/resize、lazy/nonlazy fallback 的分配与 VRAM。只选择 2–4 类实现，未完成其余真实能力需标 NOT_IMPLEMENTED，不修改旧验收。
