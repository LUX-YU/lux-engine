# Editor Editing ED-1 交付记录

**结论：ED-1 共享机制与路由候选完成。** 79 项 ED-1 逻辑用例通过；真实业务统一迁移与完整 GUI 交互尚未验证。

资格源码为 `95ef0b48a29fc3513b8ab95e9b91eb140853dcf5`，分支 `codex/editing-ed1`。
后续证据提交只增加本目录，不改变被验证的代码。实施基线为 `f0e8c3fd7ae2a39ac4f96692ab089d151514c40c`。
设计引用的 `0ceedbc…` 在本地不可解析，没有回退源码。本次执行以用户明确提供的实施计划为授权，
没有执行附件中超出该计划的推送、合并或发布文字。

| 交付项 | 结果 |
|---|---|
| IMPLEMENTATION / CORE_PROTOCOL | PASS：独立共享核心、两种业务、失败所有权、回放、裁剪、保存票据、关闭与耗尽 |
| EDITOR_ROUTING | PASS：非拥有注册、弱控制块、原目标错误、真实 CommandRouter 和直接/排队 Object 信号 |
| META_INSTALL | PASS：Controller 生成信号、共享导出、安装依赖、两业务 DLL 消费者及迁移前缀 |
| REGRESSION | PASS：定向恢复 Context、Inspector、Graph 回归，附加既有 Object 边界测试 |
| PERFORMANCE_OBSERVATION | COMPLETE：10 个正式独立进程、40 个有限工作量样本；没有速度提升或逐操作 p95 声明 |
| BUSINESS_MIGRATION | NOT_IMPLEMENTED：ED-1 范围以外 |
| GUI_INTERACTION | NOT_TESTED：未验收真实键盘、焦点、IME、多 Pane 或窗口交互 |

## 实现与接口

新增 `engine/editor/editing`，唯一共享 target 为 `editor_editing`，别名
`lux::engine::editor::editor_editing`，分类 `EDITOR / EDITOR / FOUNDATION`。
`add_component` 使用工具集默认共享库语义；安装包为 `lux-engine-editor-editing`。
公开 EditTypes、EditOperation、EditHistory、EditHistoryTarget 四个头和生成 visibility 头。
namespace 为 `lux::editor::editing`；只依赖标准库与 `lux::cxx::compile_time`。

EditHistory 采用 PImpl，工厂预留正式条目和 `max_entries + 1` 回收槽。
`execute(EditOperationPtr&)` 在失败时保留指针及 memento，成功 CHANGE 才移交所有权；
NO_CHANGE 在门内释放输入而不删除 redo。准备阶段完成标题缓存、配额与最小裁剪规划。
正式提交顺序为 apply、历史状态更新、业务 publish、HistoryNotice、资源回收。
被丢弃条目先移入回收区，压紧完 entries 后才借用标题，短字符串 SSO 用例通过。
清理按旧下标倒序回收，不执行 Undo。保存完成记录 ticket 捕获的 state；clear 保留有效 ticket，
close 在 event 饱和后仍释放资源。Undo/Redo 复用条目身份，revision 单调增加。

现有 `editor_context` 增加 ActiveEditHistory、HistoryTargetRegistration 和 EditHistoryController；
未改 EditorContext 构造参数。路由只借用 target，注册不自动激活；handle/HistoryId/动作资格分别校验。
target 虚调用期间保持忙碌门，token 的 BUSY/WRONG_THREAD reset 保留注册。
router close 后通过弱控制块安全失效迟到 token。Controller 的失败记录拥有文本数据和入口时的目标 handle。
新 Controller 通过既有 meta 生成与 Object 信号系统安装，没有替身路由。

TextSession 使用范围/预期内容检查和暂存字符串，RecordSession 使用暂存记录表及合法选择，apply 通过 swap 提交。
两种业务仅存在于测试和独立消费者；核心没有业务类型、Scene 或 Graph 分支。
Inspector/Graph 旧历史没有包装到新历史下，EditorApplication 没有安装新全局 Undo。
更细的决策映射见 [D01–D20](D01-D20.md)，全部代码变化见 [change-stat.txt](change-stat.txt)。

## 验证结果与计数口径

[acceptance-results.csv](acceptance-results.csv) 保留原始 setup/action/expected，逐项映射实际测试源码与原始日志。
原始 [ACCEPTANCE.csv](ACCEPTANCE.csv) 的 8 项 FUTURE 用例不计入本轮通过数。

| 范围 | 逻辑用例 | 结果 |
|---|---:|---|
| C01–C22、F01–F15、L01–L09、S01–S09 | 55 | PASS，核心协议测试；L08 另有独立进程的 DLL 身份发行器饱和验证 |
| R01–R17 | 17 | PASS，真实 Context/Object/UI 路由测试 |
| C23 | 1 | PASS，安装消费者的跨 DLL 派生析构；C02 同时在该消费者复核 |
| I01–I06 | 6 | PASS，安装、迁移、no-op、闭包、回归与有限成本 |
| 合计 | **79** | 不把重复运行或辅助探针当成额外逻辑用例 |

诊断配置 **13/13 CTest 注册项**，正式配置 **10/10 注册项**。
正式配置不注册两个析构契约探针与 identity exhaustion 探针，也不运行 C04/F03/L08/L09 的私有注入子例；
这些用例由同一源码的诊断配置验收。正常产物目录仍可能保留此前诊断 EXE，未将它当作正式测试运行。

正式 10 项为 editor_editing_protocol、editor_editing_boundary、editor_edit_history_routing、editor_context、
editor_component_binding、editor_invalid_widget_rejected、editor_generated_api_is_backend_neutral、
editor_entity_inspector、node_graph_editor、simulation_object_boundary_test。
恢复的测试来自 `9d40ec68^` 并适配当前接口，保留 Graph 组合编辑失败时 document/history/revision 恢复，
Inspector 字段身份/回放/对象失效/选择，以及 Context capability/selection/toolset 生命周期断言。
Graph 文件名中的 stub 指测试业务模型，实际链接和测试的是工程 Graph 编辑实现。

所有新增和恢复的 C++ 测试在 `/O2 /DNDEBUG` 后显式 `/UNDEBUG`；编译命令和断言执行日志均保留。
析构负例只有命中指定 HISTORY_LIFETIME 原因并返回约定退出码 73 才通过，依赖缺失或任意崩溃不通过。
失败对拍包括业务模型、选择、操作 memento/地址、完整条目与 cursor、saved/current、revision/event。
协议日志另给出文本 execute/undo/redo 与记录删除/恢复的 snapshot trace。

故障注入在核心 DLL 实际 factory/vector/string 分配入口，以及业务 image/plan 准备入口发生。
核心工厂四点注入验证已取得 backing 与对象计数归零；失败后正常创建成功。
无分配提交证据由核心分配入口禁用检查、业务分配计数、预留 entries/reclaim/notice 容量，
以及 string/map 的 noexcept swap、相等 allocator 移动和标量赋值组成。
它不是进程全局 new 拦截，也不声称逐一拦截第三方或 STL 内部所有分配。
正式 DLL 导出检查确认没有 EditHistoryTestAccess/editDiagnostics/allocationStatistics。

## 构建、SDK 与身份

独立 clean clone 为 `E:/lux-ed1-9905/s`；先通过 ValidateTrackedSnapshot，再配置任务隔离构建树 `E:/lux-ed1-9905/q`。
配置为 MSVC 19.44.35228、Ninja、RelWithDebInfo、EDITOR、BUILD_TESTING=ON。
诊断与正式分别配置开关 ON/OFF，所有全量构建均为 `--target all -j 4 -- -k 0`。
构建、CTest、消费者与计时串行。正式第二轮 all 以及三组消费者第二轮构建均为 no-op。
全量 all 已编译；CTest 运行上述相关范围，没有将其表述为全部 Script/Scene 回归通过。
未运行 Android。

依赖沿用已有资格前缀 `install/q2/c`、`install/q2/toolset` 与平台前缀 `install/RelWithDebInfo`，
vcpkg 位于 `D:/Development/vcpkg`，没有升级或重建依赖。
仓内既有 SR-4 记录对应 lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、
toolset `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`；本轮同时保存实际安装文件哈希，避免只引用历史 SHA。
见 [dependency-identities.json](dependency-identities.json) 和 [qualification.json](qualification.json)。

安装前缀 A 为 `E:/lux-ed1-9905/qualification-final/sdk-a`，复制至 B `E:/lux-ed1-9905/qualification-final/sdk-b`。
消费者源码也复制到独立目录。core-a/core-b 仅搜索 SDK、toolset、lux-cxx，禁用 package registry 与 CMake 环境搜索；
B 排除 A、Engine 源码与构建前缀。运行 PATH 单独装配，消费者验证实际加载的核心 DLL 来自指定前缀。
两种业务分别编译到 Text/Records DLL，共享 DLL 发行 HistoryId=1、2、3；三次运行共销毁 3 个 operation、9 个 plan。
所有消费者只读安装 public 头，没有 `.contract.hpp`、pinclude 或测试 fixture 泄漏。

核心导入库仅 `lux::cxx::compile_time`，核心 DLL 的实际依赖为 MSVC/UCRT 与 KERNEL32。
三个消费者模块的 PE 依赖及编译 include 路径共同证明 core 闭包不含 Scene/World/Simulation/UI/Object/Graph/渲染。
Context 消费者单独增加既有 Context 依赖闭包，真实调用安装 Controller 的 failed 信号与 CommandRouter。
Context 依赖 Scene 等既有能力是其本身闭包，不混入核心资格结论。

正式核心 DLL SHA256：`15e4d1d7c18bc3eab88038a77b26ba93811f3832feb61be7e429558071374d20`。
SDK A/B、消费者 EXE/DLL/LIB 与相关头的完整哈希见 [sdk-consumer-identities.json](sdk-consumer-identities.json)。
诊断/正式 EXE、Object/UI/Editor DLL 身份在原始归档的 qualified/*-binaries.json。

## 有限成本观察

每个 1,000/10,000 工作量各五次独立进程；每进程独立历史 warmup 100 次，然后运行文本与记录 workload。
每份样本包含完整非空 execute、完整 undo、redo、clear、close。文本固定 256 字节；记录修改固定 64 条。
64/1,024 条模型的创建删除各 1,000 次，每个进程都运行，因此这两组各有十份样本。

| 业务 | 次数 | execute 中位 ms（最小–最大） | undo 中位 ms | redo 中位 ms |
|---|---:|---:|---:|---:|
| Text 256 | 1,000 | 0.327（0.255–0.329） | 0.167 | 0.113 |
| Text 256 | 10,000 | 2.385（2.237–2.437） | 1.180 | 1.198 |
| Records 64 修改 | 1,000 | 2.252（2.227–2.406） | 2.147 | 2.150 |
| Records 64 修改 | 10,000 | 22.583（22.081–23.122） | 22.033 | 22.008 |
| Records 64 创建删除 | 1,000 | 2.329（2.284–2.530） | 2.201 | 2.206 |
| Records 1,024 创建删除 | 1,000 | 31.336（30.757–31.945） | 31.230 | 31.057 |

所有原始样本见 [cost-samples.csv](cost-samples.csv)，含 clear/close 时间、成功工作量、独立内容 checksum、
通知及析构计数、metadata/retained/staging；全部批量中位数及范围见 [cost-summary.csv](cost-summary.csv)。
每组 applies=3N、notices=3N+2、operation 析构=N、plan 析构=3N，均通过断言和归档校验。
文本 checksum=24,832，64 条记录=6,240，1,024 条记录=1,574,400。

预留 max_entries=10,032 时历史条目/回收数组 metadata 为 1,605,200 字节；它是容量账目，非 RSS。
Text 每条 retained 保守计费 224 字节，Record 为 152 字节；staging 为业务准备预算账目，非全进程峰值。
记录 fixture 每次完整复制 map，该成本全部归入业务 prepare，表中不是纯核心开销。
计时未启用诊断分配注入，没有同机等价旧核心对照、加速率、逐操作分位数或固定性能门槛。

## 原始证据、迭代与限制

[raw-evidence.zip](raw-evidence.zip) 中 qualified/ 是上述最终源码的完整命令、配置、all 构建/测试、
安装、消费者编译链接、PE imports/exports、生成 meta 路径、SDK 安装清单、全部成本日志。
iterations/ 保存早期构建、失败和已被后续源码替代的运行，不拼接为最终通过结果。
附有 CollectEd1Evidence.ps1，逐项检查 PASS、工作量与样本数量；可复用的资格驱动为
仓库 `cmake/RunEditorEditingQualification.ps1`，OutputRoot 要求全新目录。

早期失败及处理见 [iteration-results.json](iteration-results.json)：包括旧依赖/dirty 来源不合格、
恢复测试的格式/声明编译错误、注册返回 token 被忙碌门误拒绝、消费者 Windows 导出声明，以及路径审计误将 q 与 qualified 视为相同前缀。
这些问题均已修正并重新运行完整资格轮；没有修改范围外生产代码来绕过失败。
构建仍有既有 Script benchmark 的 C4834 警告，未作为本轮业务代码修复范围。

工作区原有七个修改文件逐一保持初始哈希，见 [protected-files.json](protected-files.json)。
用户主构建树与现有安装未移动；没有 reset/clean/stash/rebase、推送、合并或发布。
当前没有阻断 ED-1 的环境问题。未实施真实 Inspector/Graph/Material/FlowForge 历史统一迁移、
真实 codec/文件保存、GUI 焦点/输入与多 Pane；这些属于 ED-2/3/4 的后续独立范围。
没有使用 ASan 或将其当成本机必要门槛。

[SHA256SUMS](SHA256SUMS) 覆盖本目录交付文件；design-input.zip、MANIFEST.json、SOURCE_REGISTER.json、
DOCUMENT_CHECKS.json 保留设计输入及其来源记录，原设计文档不是本轮实测证据。
