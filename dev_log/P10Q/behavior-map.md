# P10Q 原行为与 API 迁移对应

本表补充原 195 项测试名称包含关系；名称／数量本身不证明语义。实现差异可由 files.json 的固定 Git 对象核验。以下只迁移调用或增强断言，没有删除原失败判据。

| 原行为 | 最终测试／消费者 | 本轮调用变化与继续保留的断言 |
|---|---|---|
| P10 R1 草稿与排队输入来源 | editor.draft_source_literal、signature、flow-canvas、material-canvas、flow-queue、material-queue、material-lifetime、lifecycle、positive、history | 类型化编译服务和共同输入交付；仍检查 S0 草稿遇 S1、BUSY 不移除、STALE 拒绝、明确 Revert、History 及作者值，绝不执行时补 current。 |
| 三模型的字段／结构原子性 | editor.scene_model.*、material_model.*、flowforge_model.* | 公开纯别名改 TSessionAccess；保留冻结源、current、observed、dirty、绑定与 undo/redo 检查。 |
| 混合替换和全程读取准入 | mixed-*、read-*、reading、reload-*、input-* | 没有重写模型或 gate；原 clone／code 析构回调和所有提前返回分支保留。 |
| Flow 身份高水位 | ids-undo-branch、candidate-seed、signature、restore、failure、exhaustion | NodeId、全部 PinId、变量身份与哨兵继续验证；未改为大小判断。 |
| P05 保存／发布 | editor.persistence.*、真实 files、asset_save_target | SaveExecution 改命名空间和 target；FileArtifactStore 更名；SharedBytes 替换 payload vector。原 FIFO、Unknown lane、Save As 历史／高水位、ExportCopy、容量、递归与迟到完成判据保留。 |
| P05 R1/R2 完成可靠性 | persistence.r1-*、r2-* | 同一 SaveService／WriteCoordinator；已接纳编码完成在外层 dispatch 内仍可吸收，禁止重新放开递归确认。 |
| 运行单步和退休 | scene_execution.*、r1-queued、mixed、failed、callback、capacity | Runtime 和结果寿命未替换；实例回收后原票据结果、单项／Run 确认和容量仍检查。 |
| 编译 owner 与发布 | compilation.actual_publish、ownership_material、ownership_flow；projection-compilation SDK 的八项 reject | Operation 四种特殊成员禁用仍锁定；Material 新服务转移 unique_ptr；Flow 服务 ID／借用；产物可独立拥有；编译失败不伪装发布失败。 |
| P08/R1 临时访问失败 | interaction_reclaim.{scene,material,flow}.{sync,cancel,selection,stale,gate,thread,reload} | 原真实 Store 回收回调保留；BUSY 不等于 STALE，不清除手势／选择／来源。 |
| 实测后的 Flow 选择复用 | editor.flowforge_interaction；实际 SDK candidate-paths | 新增稳定 1000 次同步、嵌套 gate BUSY 时全状态不变、删除／Undo／Redo／关闭；缓存仅取代重复冻结，不跳过 Store 或原 gate。 |
| P09/R1 文件和迁移来源 | workspace.*；实际 workspace SDK | IO／BUSY 不解释为不存在；同物理目标、稳定 LayoutId、selected 来源、非选 opaque、marker、冲突和幂等保留。夹具只改为独立子目录，不重试失败。 |
| Detached／Host 生命周期 | editor.detached_views、view_host、interaction-views SDK | 原挂载／卸载／单 owner／code 顺序保留；新增可重试与永久关闭失败断言。 |
| 后端／真正双视口 | render.features.view_binding、projection.highlight_backend、scene_views_gpu、desktop-views SDK | ViewportElement 搬迁；仍经真实 RenderRuntime readback 检查独立相机、高亮、资源重试、关闭重开及 validation_errors=0。 |
| 实际原生输入 | editor.desktop_native_input、desktop-views SDK | 唯一输入路径、焦点、capture、Undo 断言不变；夹具临时置顶且确认实际命中窗口，退出还原；没有自动重试或模拟入口替代。系统 IME 未测。 |
| 旧产品／跨 DLL | unified_core_*、context_lifecycle、component_elements、editor-d2、external-feature | 新目录和任务提供者由旧壳借用；旧业务回归及独立 domain/GUI DLL 析构和生成控件继续执行。 |
| 包与真实依赖负例 | 全部 *_boundaries、architecture_negative/current、quality.boundaries | 删除过时包；精确 render_client 依赖补齐并补直接／传递 render_runtime 拒绝夹具，未通过放宽层规则获得通过。 |
| 历史已知失败 | C01、C03、C04 独立探针 | 预期退出 1；旧历史快照不修改，新结果单独存档，后续责任不变。 |

新增质量行为包括目录/任务共享与失败保持、关闭分类、线性层级输入、容器资格和真实画布 churn。历史输入证据仍引用原实施 SHA；before 中的异常退出和未完成长测不能改写成通过。
