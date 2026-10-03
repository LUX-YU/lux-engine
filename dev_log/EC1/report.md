# EC1 交付与证据说明

EC1 S0–S7 的本轮 Windows 与安装 SDK 范围已完成，停在 EC1 复审。实现提交为 `137b8f441faa1dbb7dfa792b6f01b513327e6248`；精确状态、命令退出值与 XEC-01～24 映射见 receipt.json、coverage.json 和 commands.json。

最终资格包括：独立 clean tracked 全量构建和二次无工作；222 项完整回归加单独原生输入；PLAYER 全量构建、二次无工作、13 项回归及无 Editor 编译闭包；45 个安装公共头的独立 C++20 编译；24 组安装消费者及原八项 operation 编译负例；三个实际骨骼 DLL 模式；安装版原生输入。真实双视口、原应用场景与两种既有 GPU 模式均实际运行。数量用于核对清单，具体行为见各命令的详细输出。

## 工作区、范围与保护

- 实施检出：`E:/SyncForder/CodeRepos/lux-engine-p12`；独立干净资格检出：`E:/SyncForder/CodeRepos/lux-engine-ec1`。
- 原检出 `E:/SyncForder/CodeRepos/lux-engine` 及其 ProjectBuilder.cpp 用户差异保持原样。补丁**未应用**到实施检出；对应新路径为 `editor/authoring/project/src/ProjectBuilder.cpp`。原字节与补丁继续保留在既有 `dev_log/P12/protected/`。
- 不修改 main、不合并、不发布；结束后停在 EC1 复审。原阶段快照未改写。
- P12 `PARTIAL_USER_WAIVER`、Linux／系统 IME `NOT_RUN`、旧 50k 性能样本 `PARTIAL` 继续保留。EC1 的自动化真实窗口测试不等于完整人工编辑链或输入法实测。

## 完成的责任迁移

| 范围 | 唯一 owner 与本轮变化 |
|---|---|
| History | EditHistoryData 保留一份身份、记录和 gate；EditExecutor 实际执行原 prepare/apply/publish/replay/reclaim 算法。原 EditHistory 变异成员和调用已删除，两个执行者仍共用同一日志准入。 |
| 会话准备 | SessionPreparation 明确拥有可执行准备责任，替代 PreparedSessionData；SessionStore、SessionState、三领域 gate 和保存基线不复制。 |
| 资产观察 | ProjectCatalogSnapshot 私有拥有关系、只读目录与同快照 by-id 索引。ModelCreation 使用固定目录索引；外部 codec 前后的版本检查保留。 |
| 激活与能力 | Editor 扩展 ABI V8 按声明提供 SessionActivities、ProjectActivities、WorkbenchAccess。它们组合原 provider 引用，不做服务查找；CPU 使用者不被迫实例化渲染或编译器。 |
| 开放路由 | SourceAuthoring 用稳定 AssetTypeId 和格式版本；不可变工厂目录负责发现和歧义。Manifest 新写 v3，v1/v2 只读迁移，未知类型保留。 |
| 视图装配 | 各领域工厂产生完整 DetachedView；拥有自己的交互、意图连接和呈现。ViewHost 独占挂载窗口，应用仅使用通用 ViewContent 关系和 exact ViewType，不保存具体 Interaction 指针。 |
| 派生产物 | 冻结产物经 IArtifactSource / SharedBytes 交接到既有发布 owner，删除 VCompiledSource 产品分发表；不改变作者 checkpoint。 |
| 适用性 | AuthoringFacts / queryApplicability 使用实际声明、已安装 provider 和分区契约。Inspector、创建与 Feature 选择共用事实，最终领域依赖与冲突校验仍在原位置。 |
| 运行与异步 | 原 SceneRuntime、RunStore、ExecutionRuntime、WriteCoordinator 和资源退休 owner 保持唯一；视图消失不丢已准入完成。 |

## 外部骨骼 DLL

`cmake/installed-consumers/editor-ec1-skeleton` 是独立安装 SDK 消费者，三个模式使用同一实际 DLL。

- HEADLESS：原 SkeletonAsset codec、VFS 读取、真实工作副本、历史、冻结、保存与重读。
- WINDOW：原 Pane/Element、DetachedView、真实 Root/Host、焦点、关闭与最后代码 lease 析构。
- APP：实际 Desktop/Application 的资产打开意图、两个视图共用内容、Save/SaveAs/reload/recovery、关闭及缺 provider 后保留数据。没有宿主 Skeleton 分支。
- 可编辑 root 名称与 global X，保留骨骼顺序、parent_index、bind/inverse-bind 和 mesh 索引约定；不声明重定向、IK、重排或完整动画工作台。
- APP 的 witness DLL 仅作验收观测，显式声明 Project/Workbench 能力，不贡献模型，不扩大 Skeleton DLL 的能力声明。CPU 路线不依赖它。

## 删除及文件变化

`file-actions.json` 是 Git 的实际增改删／重命名清单；`files.json` 固定最终内容哈希。本实现为 31 个新增、157 个修改、2 个迁移，共 190 项。旧责任多数位于原文件内部，因此不能用删除文件数量代替删除算法证明。

已扫描并删除的生产符号包括 `PreparedSessionData`、`EProjectAssetKind`、`VCompiledSource`、`Bone_t` 和 `EditHistory::execute/undo/redo/clear/close`。旧格式 wire 标签的只读转换不是旧产品回落，继续保留。AssetTypeId 迁入 identity 的真实 provider，逻辑 include 不变，不留转发别名。

## 检查有效期与规范核对

- 原断言迁移单独核对：History 调用换为 EditExecutor，日志与值断言保留；Workspace 动作换为带载荷的 variant；恢复错误由松散 failure/result 换为一个 expected，仍核对同一成功／失败与来源；具体 content_views 观察改为 Host 的公开关系查询；catalog 字段改只读方法。Outliner 原对象数量断言增加了身份折叠检查，没有用更弱的条件替代。
- SceneSource 同一个稳定读取作用域中，实体及 schema 成员关系已建立后直接捕获，删除重复查询。查询与使用之间没有回调或 owner 替换。模型写入、异步采用、文件版本、gate 和 codec 后的核对继续保留。
- 内部 opaque 保留只接受已准入 SceneSource 的真正不可变拥有块；外部可变别名仍复制冻结。共享物理字节仍按全逻辑字节计入容量。
- public header 的重依赖按真实 provider 收拢；45 个修改公共头逐个通过安装前缀的 C++20 编译检查，实际结果见 final-headers。
- 新 owner 明确特殊成员；连接和代码引用销毁顺序不交给偶然字段顺序推断。原 operation 的八项编译负例保持。
- 修改闭包逐项检查短路前置、复杂判断命名、异常边界及 observer；没有新增热路径异常或 OOM 恢复，没有在 observer 内直接改 Registry。
- 格式核对不是全仓 AST 证明；已有第三方弃用和未修改文件警告不伪装成已清零。

## 性能裁定

详见 `S5-notes.md` 及真实短样本日志。开发期前后样本带有当时源码 diff 指纹，属于开发测量，不冒充最终 clean SHA 的新运行成绩。最终回归再次检查行为和成本计数。

| 路径 | 结论与测量边界 |
|---|---|
| Outliner | full author/Run 身份作为折叠 key，一次 N 插入和 C 查询替代 C×N 标签搜索；不将源码操作上界叫作分配计数。 |
| 目录核验 | 同一不可变快照按 AssetId 查询，避免每个 primitive 扫全目录；版本仍在实际失效边界核对。 |
| 捕获 | 六个登记 schema、两个实际非空池、1k/10k 对象；跳过空池并删除重复 lookup。10k 短样本约 4.8–5 ms；保留 N×非空池扫描，不新增全局组件索引。 |
| opaque | 内部已冻结 8 MiB 块每次复制从 8 MiB 减为 0，逻辑预算不变；元信息和编码结果仍有其正常分配。 |
| 投影 | 32/512 对象；无变化 0 重建，字段/结构/配置各 1。512 对象字段约 2 ms、配置约 2.46 ms；保留完整重建，不引入另一套增量退休协议。 |

## 失败证据与支持边界

所有首次失败保留原日志。开发期失败及对应修正写在唯一迁移账本的 ec1 节点。最终资格中，缺 Lua package 路径、缺原有 packed-render/Physics2D 产品配置和中途停止的 bf55 构建均保留；不将其改记为成功，也不声称任意关闭 builtin 功能的 Editor 配置都能构建。

最终 Windows 配置是 `EC1 + STRICT`、RelWithDebInfo、实际 LLVM/MLIR 与既有 builtin 功能；从最终 clean tracked commit 独立构建并安装到全新 EC1 前缀。依赖种子只有逐文件哈希核对的第三方产物，没有旧 engine 头或 DLL。SDK 模式分别检查公开 include/link 与运行依赖，不借旧构建树。

原生输入是自动化真实窗口路径；系统 IME 继续 NOT_RUN。旧人工链免验、Linux 和旧慢测不会因为 EC1 自动化通过而改变状态。

本次原生输入在源码构建与安装 SDK 两条路径都实际运行。两条输出均记录 foreground=1、hit_test=1、TextEdit=`ab`、validation_errors=0；检查拖动生成 Inspector 的 Preview/Commit/Undo、跨区域捕获、持有捕获时关闭及 DIRECT 关闭后的焦点释放。它不是系统 IME 候选／提交测试。

归档已实际搬到中文／空格路径验证；完整证据通过，删除 final-build.log 或篡改其字节均被真实验证器拒绝。此结果只属于 EC1，不改写 P12 免验结论。最终供复审的检出为 `E:/SyncForder/CodeRepos/lux-engine-ec1`。
