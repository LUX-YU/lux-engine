# P09：独立工作区值、纯计划和可靠存储

状态：**PASS（P09 范围）**。完整执行记录及归档门禁见 `receipt.json` 和 `check_receipt.py`。

已验收前置：`41167a9bdff8c192fe990d53aa8dfa132d57b081`。实现提交：
`38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1`，分支 `codex/editor-redesign-v4`。
本阶段只交付 P09，停止等待复审；未接入 P10 桌面宿主或 P12 产品 ApplyLayout。

## 输入和实现边界

原 V4 P09、本次启动补充和既有施工账本一起作为依据。本次输入包的 40 项 SHA256 核验通过，
其中 `reference/v4` 与当前施工材料中的 `spec-v4` 逐文件一致。原包和提取内容保存在 `spec/`。
`.internal/editor-redesign/` 仍是唯一可变施工材料；本目录只冻结本阶段实现及运行证据。
旧阶段快照未修改，旧源码证据按各自 implementation_sha 核验。

`ProjectBuilder.cpp` 原有差异没有进入实现提交，其 SHA256 保持
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
没有 reset，没有修改 main。

## 数据、计划与所有权

`layout_model` 是一个 STATIC 库，包含 layout 和 recovery 两个职责目录；`workspace_store`
是另一个 STATIC 库。没有新增 DLL、通用管理器或任务队列。

| 数据或责任 | 唯一 owner / 边界 |
|---|---|
| 布局 | DockLayout 拥有稳定 LayoutId、label、槽位、DockTree、视图配置和 opaque 数据 |
| 恢复 | RecoveryManifest 仅拥有内容 locator 和恢复关联，不拥有作者源或 live SessionId |
| 偏好 | UserPreferences 独立保存 selected LayoutId，不兼任布局或请求状态 |
| 目录 | LayoutCatalog 是值快照，明确完整性及逐条诊断 |
| 验证 | ValidatedLayout 只能经完整验证构造，内部数据只读；无 unchecked 默认构造 |
| 计划 | LayoutPlan 拥有输入快照和结果；不保留活动 Root、Pane、Session 或 provider 回调 |
| 发布 | 应用既有发布 owner 向 WorkspaceStore 提供同一个 P05 WriteCoordinator 和 IArtifactStore |
| 票据/FIFO/Unknown | 原 WriteCoordinator；Store 不建立另一个队列，也不代替调用方取走 ready 项 |
| 物理文件操作 | 原 ProjectArtifactStore 可靠发布边界；Store 读取、编码和提交原 WriteTicket |

P08 的 ViewInfo 值迁入已有 `editor/contracts`，公开 include 路径、ViewTypeId 身份不变。
原 `editor/views/api` 定义删除，没有转发头。纯 planner 因而无需链接 UI。

按 parse → validate → resolve 处理全树：schema、重复身份、引用、环、共享节点、不可达节点、
深度、有限 split 比例、根几何及预算均先验证。精确按 ViewRestoreKey + ViewType 匹配；
同类型其他窗口不会被误用，额外活动视图保留。输出中的 ViewId 只是未来采用时需要重验的快照。
缺失 provider 或不支持的视图 schema 是明确计划结果，不触发创建、打开、绑定或回调。

## 存储契约与迁移

新格式均为 schema 1 TOML：

- `.lux/workspace/layouts/<32 位小写十六进制 LayoutId>.layout`
- `.lux/workspace/preferences.toml`
- `.lux/workspace/recovery.toml`
- `.lux/workspace/migration-v1.toml`

Rename 只发布同一个布局文件中的 label。布局发布结果、偏好结果、目录刷新结果分开；
后处理失败不能擦掉已提交的磁盘事实。只返回 WriteTicket 表示请求已接受，不表示已写入。
Store 不修改作者 checkpoint。BUSY、权限、版本变化和读取失败原样传播，只有确认不存在才是
NOT_FOUND。单条目录记录损坏返回不完整目录及诊断，目录访问失败返回错误，不成功写回默认数据。

沿原 publicationTargetKey 规范化受控根内目标；不同逻辑地址不能绕开同一物理目标的 lane。
为实际删除新增窄的 REMOVE 动作和 provideRemoval，仍使用原票据、FIFO、容量、Unknown 及 writer
退休协议。物理删除只在 ProjectArtifactStore 发布入口发生。已经验证的前序发布边可推进在途
删除的版本前提，外部修改仍冲突；Unknown writer 未退休时删除不能越过它。删除回执为 missing
版本及未确认目录持久性，不声称 unlink 已完成文件刷新。之后显式提交的新保存可以重建文件，
之前排队的保存不能在同 lane 删除之后晚到并复活它。

未知合法 payload 按 type/schema、精确字节长度和 hex 保存；支持的文件 schema 中未知字段保留
原文件字节。未知整个文件 schema 拒绝改写。默认文件上限 16 MiB、opaque 8 MiB、条目 4096、
树及 TOML 深度 64，超限明确拒绝。

私有 LegacyWorkspaceImporter 仅读旧 `.lux/editor` schema 1 TOML 及可识别的 ImGui INI 数据。
稳定 ID 来自旧相对文件名，来源记录含原输入摘要；旧文件原字节保存在 opaque 中。可识别的
`v1:<asset UUID>` 转成独立恢复 locator，不打开资产。没有记录的未保存内容无法恢复，此限制
作为诊断返回。未知子窗口或 INI 细节保留在原字节中，不伪造新产品已经应用它们。

continueMigration 每次重验输入及目标，最多准入一项缺失写入；调用方结清后再继续。已有且来源
一致的新记录保留，包括迁移后用户编辑；来源冲突明确拒绝。全部新记录重新读取验证后才接受
marker 发布。中断后重建 Store 再继续，不随机生成身份，不修改旧文件，不覆盖已迁移的用户值。
这是非热路径的有界重验，没有新增增量迁移缓存或性能保证。

## 逐项验收映射

以下日志均对应本次实现 SHA，native 与安装 SDK 的同场景分别记录，不能互相替代。

| 原编号 | 实际证据 |
|---|---|
| X09-01 | validation/effects：坏 dock、重复和环被拒绝；真实 Root、已挂载 Pane、dirty MaterialSession 的内容、current、observed、绑定和历史不变 |
| X09-02 | rename：真实 label 发布成功，文件路径/ID 不变；Windows 只读偏好目标真实返回 ACCESS_DENIED，native_code=5 |
| X09-03 | remove：真实删除、偏好仍指向原 ID，重建 Store 后有诊断回退；排序、Unknown、外部冲突另有深化场景 |
| X09-04 | opaque/budgets：未知类型/未来 payload 原字节往返、未知字段保留、文件/条目/深度/载荷超限拒绝 |
| X09-05 | migration/collision：新文件发布、marker 前中断，重建 Store 幂等继续；用户改名保留、原文件不变、目标来源冲突拒绝 |
| X09-06 | effects：真实 dirty 内容与挂载视图；同类型不同 key、不同类型相同 key、额外视图精确处理；零打开/重绑定，Undo/Redo 仍可用 |

X09 深化还覆盖同一物理目标的不同 namespace、已确认前序、删除 Unknown 的退休与核对、读取失败
分类、损坏目录记录及布局发布后的目录刷新失败。`evidence/workspace-files` 和 SDK 对应目录保留
真实 IO 产物，测试日志保留实际观察。

| Q 场景 | P09 交付范围与后续责任 |
|---|---|
| Q04 | 纯计划不修改真实作者/UI 已通过；完整产品应用仍归 P12 |
| Q31 | 新纯验证完整拒绝已通过；旧 C01 完整应用仍 FAIL，归 P09/P12 |
| Q32 | 缺 provider 和未知载荷按预算保留，PASS |
| Q33 | 精确身份与额外视图保留已通过；桌面 ApplyLayout 仍归 P12 |
| Q35 | 稳定身份及改名/偏好分离，PASS |
| Q36 | 发布、偏好、目录事实分离已通过；完整产品应用仍归 P12 |
| Q37 | 实际删除及重建读取入口回退已通过；桌面启动应用仍归 P12 |
| Q52 | 新格式 schema 拒绝和 opaque 保留已通过；完整扩展 SDK 装载仍归 P11 |

## 工程验收

所有最终命令、退出码、归档路径和 SHA256 位于 `receipt.json`；文件清单位于 `FILES.md`。
资格来自准确实现提交的独立干净检出，构建树复用，不宣称首次冷构建通过。

| 验证 | 本次结果 |
|---|---|
| 显式阶段 | 主配置、PLAYER 配置及 architecture/package audit 均为 P09，零新门禁发现 |
| 全量构建 | target all / -j 4 / -k 0 通过；第二轮 ninja: no work to do |
| 完整 CTest | 174/174；原 160 项全部保留，新增 13 项行为及 1 项依赖测试 |
| 原模型和补正 | P01 至 P08/R1/R2、三模型、保存、Run、交互、真实 UI 维护及历史归档检查均通过 |
| 实际编译/IO/后端 | 固定产物编译、发布、视口绑定及 P09 原生/安装后的真实文件场景通过 |
| 依赖 | 原各组负例保留；P09 18 个实际 CMake 夹具含合法对照，各禁边失败在预期规则，修复后通过 |
| PLAYER | 独立 PLAYER 配置 11/11；全量及二次无工作；实际编译单元/target 无 Editor |
| 安装消费者 | 重装 SDK，原 12 组 74 项加新 workspace 组 13 项，共 13 组 87/87；均重建及二次无工作 |
| Operation 特殊成员 | 原 8 项复制/移动编译负例按 C2280 拒绝，非缺头文件造成的失败 |
| 显式 GPU_UI | 1/1，实际 feature.exe 三帧及资源退休；未声明像素读回 |
| 显式 EDITOR_SCENE_PANE | 1/1，实际 scene_panes.exe --scene-panes，12 帧、resize、资源及 view 退休 |
| 归档门禁 | P09 收据验证及生产路径不可用/缺失归档/损坏归档三种检查 |

没有修改 modules 公共头，不触发三前缀头同步；没有 Android 构建。

GPU 结果单独统计。两个历史上以 GPU/ScenePane 命名的原消费者目录仍运行原默认 CPU_UI；
不能用目录名计为 GPU。本次另显式配置 CONSUMER_MODE=GPU_UI 和 EDITOR_SCENE_PANE，保留实际
test command。它们只证明原功能回归，不证明 P10/P13 完整新产品像素、IME 或布局应用资格。

开发期间首次编译诊断以及 effects 夹具错误使用非正式 Material 会话 type 导致的失败，均保留
在 `development/`。修正后的夹具使用真实 `lux.editor.material` 类型；没有缩减原断言。
最终门禁同时检查原 160 项集合及原 C++ 行为测试体未改，不能仅靠新总数证明回归。
复用投影测量脚本的原输出仍写着 P08-R1 归档前缀；原输出在 development 中保留，冻结时仅将
这五个日志引用修正为本次实际 P09 路径，不改变测量值或历史文件。测量仍仅是含进程启动的
原 CPU 投影回归，不宣称 P09 布局热路径性能资格。

归档检查只读取可取得的 dev_log 文件及版本化 Git blob；生产机器路径只是命令记录。
另外验证生产路径不可用仍能通过，缺少或损坏归档结果必须失败。

## 删除、暂留与未完成项

- ViewInfo 旧定义位置删除，新位置为唯一实现，公开 include 不变，无兼容壳。
- 新 workspace 不依赖 WorkspaceRequest、EWorkspaceAction、WorkspaceData、PaneManager 或旧 Context。
- 原 EditorWorkspace.cpp、EditorWorkspaceStorage.cpp、私有 EditorWorkspace.hpp 及 WorkspaceRequest
  只服务原 app/menu/SettingPane 消费者，最迟 P12 删除；未把它们重新接进新存储。
- 没有新增桥、平行 History、Session、Runtime、执行器、写队列或 author checkpoint。
- **C01、C03、C04 均保留原 FAIL**，本次按原探针复现；C01 完整应用归 P09/P12，C03 归 P11，C04 归 P12。
- 冷构建失败和 P06 窄修复的原证据继续保留；干净源码不等于冷构建。
- 硬链接和任意外部多进程发布协议不作支持声明。内容恢复、ApplyLayout、新桌面宿主和新产品 GPU
  资格均不在本阶段完成范围。

实现与本验收分别提交，正常推送后停止在 P09。
