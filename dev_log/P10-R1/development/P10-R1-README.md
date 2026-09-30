# P10 R1：UI 草稿与排队输入来源补正

本轮只补正 MaterialView / FlowView 的草稿与入队执行路径，停在 P10。未实施 P11。

## 问题与实际修复前证据

原 P10 安装 SDK 的四个真实负例均返回 1：Flow literal、Flow signature、Flow canvas、Material canvas。日志都记录 `source_and_history_preserved=0 stale=0`。运行实际 Flow 属性缓存与 ImGui Apply 按钮，或实际 GraphCanvas 的 edited 信号接线；外部 S1 来自相同真实 Session 的合法 apply。没有向模型手工传入旧 expected 来冒充 UI 测试。

`before/` 保存当时的测试源码、输入、配置、命令、退出码和 SDK 库/头/程序 SHA256。绝对路径是运行溯源，不是归档验证的输入；`check_receipt.py` 只读取 dev_log 中的相对归档路径，并用实现 SHA 检查历史源码。缺失和损坏证据必须拒绝。

## 生产代码

- 两个工具的 `NodePropertiesDraft` 将节点副本与捕获的 `ContentStamp based_on` 组成一个值。显示刷新不更新该戳。值与戳在同一 owner 同步调用内取得；首个扩展回调已在 withRead/copyNode 的原读取 gate 内，不能在取戳与复制间编辑会话。
- 每个原有有界 `CanvasRequest` 在入队时保存 `based_on`。属性按钮也直接进入这条带来源的队列；删除松散的 `edits_` 与 `properties_base_` / `draft_base_`。
- BEGIN、PREVIEW、COMMIT 前检查真实会话和 admission/current。BUSY 返回原错误，保留同一请求的 stage、载荷及来源；不会重新 begin。执行时不补 current。
- 终止失败通过原 Session gate 清理对应队首载荷。临时访问失败不能清空输入；真正 STALE_SESSION 才允许无活会话清理。冲突不会无限占据队首，不会自动 rebase。
- 冲突诊断保留，并提示实际 Revert / Cancel 按钮。Revert 或明确重选才同时捕获新值与新戳；成功后旧输入和草稿在原 gate 内清理。
- widgets 无作者模型依赖。唯一 History、SessionStore、模型、保存、执行器、Host、GPU、Workspace 的实现均未变。

测试依赖登记仅增加指定测试文件使用的实际公共头和 native-only 私有检查头。SDK 不定义 native 检查宏，不复制私有头；生产依赖允许集没有放宽。未新增 Manager、队列、target、库、公开 API 或兼容旁路。

## R10-R1 验收对应

| 条目 | 本轮执行入口与检查 |
|---|---|
| 01 | `draft_source_literal`：正式 S0 属性控件输入、S1 外部提交、正常 display 刷新、实际 Apply literal；拒绝且完整 S1 编码/身份/current/observed/dirty/绑定不变。`positive` 走相同真实入口成功一次与 Undo/Redo。 |
| 02 | `signature`：FuncDef 和关联 call，S0 本地改名称，S1 修改签名参数及返回值，显示刷新后实际 Apply signature 拒绝。比较完整编码，包含函数引用、参数、结果、Pin 和连接；Revert 后新提交及 Undo/Redo。 |
| 03 | native `history` 通过现有私有 PreparedFlowReload 换 History，保留相同数字节点身份，旧属性拒绝。SDK/native `lifecycle` 验证 BUSY rebind 保留旧目标、关闭后新代际同数字 ID 不被旧草稿命中、成功重绑后重新编辑。 |
| 04 | 两种 `*-canvas`：真实 edited 信号进入原队列，BEGIN 前 BUSY，再外部改 S1，旧队列拒绝，S1 不变。原始 before 未借 BUSY 直接复现入队→S1。 |
| 05 | 两种 `*-queue`：真实 read gate 下 BEGIN/后续 PREVIEW 暂停，保持 overlay 地址、来源及原载荷 x=20，恢复后验证新预览 x=25；恢复原阶段，一次提交。65 次输入证明原 64 容量限制、取消后恢复。Material `lifetime` 通过动态 ConstantNode 克隆及析构回调，验证 BUSY 不提前清理、code 存活、清理回调编辑返回 BUSY。 |
| 06 | 实际 Revert 按钮再输入/Apply，Undo 一次回到完整 S1，Redo 恢复。多帧画布的多个 Preview 只形成一次提交。未提交草稿的作者编码/捕获不变。 |

native 额外核对 checkpoint、HistoryId、entry_count、cursor、revision、event_sequence、charged_retained_bytes、closed 和 binding revision。SDK 使用公开源编码、SessionInfo、真实交互和 Undo/Redo 验证；不声称 SDK 可以直接访问私有 checkpoint。

## 验证口径

正式结果、命令、哈希、构建与测试总数见 `receipt.json`；所有执行输出在 `logs/`。新增 10 个 native 行为场景、9 个 SDK 场景，原 185 个 native 场景与断言保留。最终从实现提交的独立干净 clone、显式 P10 配置，全量 all -j 4 -- -k 0、第二轮无工作，再顺序运行 CTest、重装 SDK、14 组消费者、PLAYER、依赖负例、8 项 operation 编译负例、真正新双 SceneView GPU/读回与原生输入，以及两个旧显式 GPU 模式。

X10-04 口径进一步明确：本轮 literal/signature Apply/Revert 和 graph move/begin/preview/commit 走生产控件缓存/接线；其他原 P10 graph/property 功能仍按原记录区分 UI 入口与公开领域入口。本轮不把同一模型的外部 apply 说成第二个 GUI 的完整交互。

受控 ImGui 导航/字符输入与 GraphCanvas 信号用于确定顺序；它们不等于 OS 鼠标。原 P10 Windows 原生输入单独重跑。系统 IME 候选、组合、提交仍为 **NOT_RUN**，未扩大资格结论。

本次独立重复验收中，Workspace `catalog` 曾一次异常退出（0xC0000409）。实际转储定位到测试夹具的 `std::filesystem::rename` 抛出 `filesystem_error`，Windows 错误码为 5（访问被拒绝）；未确定具体占用来源。此前完整 CTest/SDK 的同场景已通过，同一二进制、相同参数随后连续 10 次普通运行通过，并由正式顺序脚本重新验收。没有修改 Workspace 生产代码或原测试以消除该失败。`development/P10-R1-catalog-failure.json`、转储、分析日志和 `catalog-failure-files/` 保留异常、现场与重跑证据；该次失败没有被删去或记成首次通过。

C01、C03、C04 仍按原契约 **FAIL**，责任不变。保留原 P10/P09 等快照、原冷构建失败/修复证据与责任。ProjectBuilder.cpp 用户修改未纳入提交，哈希保持不变。Q04/Q21/Q50/Q51 的产品应用/完整性能等未完成范围沿用 P10，不借此次通过更改为 PASS。

## 交付与删除

实现与验收各一提交，推送 `codex/editor-redesign-v4`；不修改 main。

本轮无文件删除：删除的是两类松散来源字段、未标记来源的暂存数组及执行时转换这份暂存数组的旧循环。完整增改清单见 `FILES.md`。Owner 不变：View 拥有本地草稿与原有有界请求，SessionStore / 领域 Session 拥有作者与历史，原服务拥有后台和 GPU 责任。既有 P12 限期桥范围未扩张。
