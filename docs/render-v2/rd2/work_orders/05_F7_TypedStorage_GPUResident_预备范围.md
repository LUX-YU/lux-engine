# RD2-F7｜真正的 Scope DataSet、Typed Storage、GPU Residency、脏增量（预备范围，未授权）

**仅 F6 PASS 且用户授权后启用。** 不允许一次性实现所有未来 Geometry/Asset/Texture/AccelerationStructure 存储；只按真实跨 Feature 和 GPU-driven 需求实现经验证的最小组合。

## 最小真实交付

1. Scope-owned 多实例 DataSet identity：Runtime/Scene/View + TypeID/Schema + Key + Generation，不能将 Feature TypeId 当 StoreId。
2. **扩展 F6 已实测的最小固定容量 DataSet backing**，不另造第二份 store owner；实现 `template<DescribedData T>` 的 `RecordHandle<T>`, `DataSetReader<T>`, `DataSetWriter<T>`；Stable Slot/Generation, sparse_to_dense, dense active, dirty revision；CPU-only T 仅受真实存储生命周期约束，GPU-facing view 另受同源 GPU Layout/Representation 验证；小而可复用的 slot/dirty算法，不再给每 T 一个动态 Manager。
3. CPU-authoritative record pool (`LightGpu`/`SpriteGpu`)：O(changed) record processing；Upload Merge/Batch/Scatter/Full thresholds由工作负载测量；失败不清 dirty；不同 Scene 同结构 shader plan 共用、实际 GPU Buffer 分离。
4. GPU-authoritative (`ParticleGpu`): only GPU resident versions、受控 GPU Update/Readback/跨帧 Sync；CPU writer `.patch` 编译负例。
5. Chunk-mode (`GaussianChunkGpu` 仅 Metadata，非完整 3DGS)：DataSchema 描述可变范围/页面需求；大型数据 Buffer/Arena 与 Chunk metadata 分离，按预算/ready/last-use 索引。
6. **GPU backing 是 Backend Native RAII owner，不是 Feature 本体/CPU RenderResourceDomain**；DataSet 逻辑有效期、slot reuse、GPU last-use 和 cached capacity 四者分别核实。
7. 一个 Scene RenderSystem 将 ECS/Editor/Procedural 源变化发布到 PROGRAM/UPLOAD，真实 end-to-end CPU delta→GPU readback；没有 GPU 问题时不因为 Entity 删除全量销毁资源。
8. 一份 DataSet 由两个 Feature 并行 consume，删除/卸载 Producer 不错误释放；另一个 Scene 可共享同 Asset backing 但保持各自 Light/Visibility/History。

## 开闭原则、GPU 性能、易用性测量

新增 SpriteGpu/ParticleGpu 类型**不得修改** RenderGraph hazard/core runtime/VMA/Feature SDK 的业务类型 switch；新 Shader 只写 PassParams，不手写 `set/binding`。GPU Cull/Compact 只处理专用 typed Workset，而不是统一 GeometryKind switch；比较 stable slot gather vs compact vs chunk 按 CPU/GPU真实测量。跑 10k–1M records, D=0/0.1/1/10/100%, create/erase churn, 1/4 View, Device Queue in-flight 的原始数据。F7 不允许因为 GPU 某布局的理论好处提前引入 SoA/Bindless/RT GenericPolicy 巨大模板层。

## 限制与 I/V

本阶段允许真实 `render_data` typed CPU storage/生成契约和 `render_vulkan` native storage adapter、`render_runtime` 最小 Dataset binding、`scene_render` 实际 typed projection 与 tests；具体精确 ALLOWED 文件须 F7 preflight 审批。Read-only Frozen Legacy/历史报表/F2 Graph算法、其它 unrelated Engine/Editor/产品。遇到 lower-layer 真实无法支持多 Owner/Indirect/Storage ABI 应先提供影响报告，不能自行重写。I/V 正常完整 Build、Full ASan/Real GPU/SDK/Perf、原测试保留与 719 保护；PASS 后 STOP，F8 需用户授权。
