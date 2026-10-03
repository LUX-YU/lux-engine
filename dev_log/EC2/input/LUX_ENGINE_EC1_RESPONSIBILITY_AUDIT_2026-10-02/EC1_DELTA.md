# EC1 补充清单：本次职责调查的处置边界

固定审阅输入：`54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8`。本文件是调查建议，不自动宣告任何实现已完成，不新增主阶段，不改写原 EC1 总册。

## 1. 需要加入 S0 的真实检查项

| ID | 当前类型／位置 | 处置等级 | 推荐并入 |
|---|---|---|---|
| RA01 | ProjectPublication / ProjectStorage / ProjectPublicationOperation | 高：计划与占用权明确分离，收紧 prepared 输入 | S1、S2 |
| RA02 | SceneConfigurationElement::SystemElement / Impl | 高：UI、草稿与领域准备分离 | S4 |
| RA03 | ArtifactPresentation / EditorArtifacts | 高：纯打包及发布流程归活动，复用现有 publication operation | S2、S3 |
| RA04 | WorkspacePane / ResultsPane | 高：去除 Impl&，展示和请求改用正式能力 | S2、S3 |
| RA05 | MaterialPreviewStore / MaterialCompileKey | 高：活 owner、配方、完成数据与采用目标分清 | S2、S3 |
| RA06 | WorkspaceStore / LegacyMigration / LayoutCommitReceipt | 中高：存储与政策／纯迁移分开，查询不隐式捆绑目录 IO | S2、S5 |
| RA07 | AssetImporter 内部 Load/Encode / model-source recipe | 中：提取配方 codec，承认模型领域，不自动建设通用 importer | S2 |
| RA08 | ProjectOpenData | 中：准确命名拥有型准备，保留 lease | S1 |
| RA09 | CompiledMaterial / CompiledFlow | 中高：来源、产物与字节的一致性封装 | S1、S3 |
| RA10 | EC1 已有 Snapshot / Intent / SessionPreparation | 继承：不再重复新建替代类型 | 原 S1 |

## 2. 明确不要做的事

- 不为 SessionState 增加 BindingManager、CheckpointManager 和 GateManager。
- 不把每一个 compile operation 改成数据＋另一个全局任务执行器。
- 不把 ProjectStorage 整体拆成大量逐方法转发接口；只让真实目录、读取和发布责任有清楚入口。
- 不把所有 UI 胶水都判为 Application 违规。跨层产品操作仍由 Application 组合。
- 不新增第二个项目发布算法。先研究已有 ProjectPublicationOperation 的能力和两种路径的语义差异。
- 不要求 History、publication plan 或 compiled output 为了“纯数据”变成任意可写 POD。
- 不删除 LegacyWorkspaceImporter 数据兼容。迁出纯转换后旧文件继续只读保留。
- 不把 `SceneProjection` 的活实例直接变成可复制 DTO，也不新增第二 Runtime。
- 不因为用到 `std::any`、function pointer、virtual 或模板就统一替换；按真实边界决定。

## 3. 接口前后对照必须记录的字段

```text
current_type / method
actual_data_owned
actual_actions_performed
external_side_effects
owner_thread_and_callback_boundaries
new_data_owner
new_algorithm_owner
new_resource_owner
consumers_to_migrate
old_definitions_to_delete
required_semantics_to_preserve
focused_test_and_measurement
```

这张表进入已有唯一施工账本，不能建立另一套 competing current 状态。

## 4. 重点行为证明

**RA01：** plan 不可被 worker 修改；reservation 唯一移动与释放；没有 operation 启动也能正确放弃；旧文件发布事实不因取消消失。

**RA02：** 无 Root 构造配置；GUI 与纯输入输出一致；当前 partitioner 而非固定 single 决定系统候选；未知 payload/版本/依赖诊断不退化。

**RA03：** 非 Material/Flow 的 owning 产物也可发布；无窗口执行；文件成功与项目登记失败分别查询；复用同一协调器，不重复写入。

**RA04：** 单独安装消费者只链接公开 provider 即可创建面板；不 include AppImpl；面板只获得所需观察与请求；原实际操作仍走安全点。

**RA05：** 不必交出编译 operation 即能提供合法完成结果；产物可供两个目标分别采用；旧结果拒绝、上一次成功、背压与退休全部保持。

**RA06：** 状态查询和目录刷新各自观察；旧输入变化仍冲突；多布局来源与 marker 幂等保持；当前终态后刷新行为不增加成每帧刷新。

**RA07：** recipe codec 可以只对 bytes 工作；读取与 CPU cooking 分离；missing 与 IO failure 不合并；原导入关闭／重试仍有效。

## 5. 验收口径

这是类型责任调查，不是新运行证明。对未经实际重现的风险不写 FAIL，对未执行的测试不写 PASS。原 P12 waiver、Linux/IME NOT_RUN、性能 PARTIAL 保留准确含义。采用本次某个建议时，针对其修改执行实际回归，不默认扩大到全产品重测。
