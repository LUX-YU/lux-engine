# P10 R1 真实测试计划

此文件只规定测试，不含“已通过”成绩。所有新增负例尚未在本审阅环境运行。

## 公共试验约束

使用真实 FlowSession／MaterialSession、原 SessionStore、正式 FlowView／MaterialView、原 Root／GraphCanvas 和安装 SDK。复用现有新桌面集成装配，不另造模型或控件实现。

另一路编辑可以来自第二个正式 View，也可通过同一模型的正常公开 apply 发出；后者是确定性安排输入顺序，不应被声称为已经测试第二个 GUI 的全部交互。

观测至少包括：完整编码、SessionId、HistoryId、current、observed、dirty、绑定及可取得的 checkpoint；native 可补历史 entry/cursor/收费。每条失败后的“源不变”以 S1（外部合法编辑完成后）为基线，不错误地要求回到 S0。

修复前日志必须实际从原 P10 生产实现运行得出。保留原输入、退出码、安装公共头／库身份和命令。源码推导如果没有在目标环境触发，明确报告，不伪造 FAIL。

## R10-R1-01：旧 literal 的来源

1. 构造真实有 DATA_IN literal 的节点；完成视图选择，使正式属性缓存属于 S0。
2. 用正式属性控件改变本地 literal，尚未 Apply。验证作者编码仍是 S0。
3. 对同一 Pin 做另一条合法提交得到 S1，值与草稿不同，Pin 身份仍有效。
4. 运行原视图正常更新，使图显示已经读取 S1；不要重选节点或 Revert 原草稿。
5. 通过实际 Apply literal 按钮／其生产共用处理函数提交。

修复前应记录是否旧草稿被按 S1 接受；修复后必须报告来源冲突，不改 S1 编码／历史／checkpoint。草稿保留或明确失效均要可诊断，不能无说明换成新来源。

正向对照：不发生第3步，同样真实入口应成功一次；Undo／Redo 完整恢复。

## R10-R1-02：旧签名不能回滚新签名

使用 FuncDef 与关联 use/call 节点。S0 时加载正式属性草稿，在本地修改名称或一个参数；另一路合法提交改变签名或另一个字段到 S1。刷新 display 后 Apply signature。

检查不仅是名称，还包括完整 arguments/results、Pin/连接、函数引用和 encoded source。修复后不应把未被用户当前编辑的 S1 签名部分还原为 S0。

禁止把这条测试替换成直接传入手工构造的旧 FlowEditBatch：那只能测模型已有的 expected 校验。

## R10-R1-03：History、重绑与相同数字身份

在同一 Session 合法重载出新 History，或关闭／新代际绑定；新图可以使用相同 NodeId/PinId 数值。旧属性不能根据相同数字命中新图。

native 可使用原私有重载测试入口；SDK 不得复制私有头，使用合法已安装的绑定／生命周期操作或仅验证其可公开覆盖的范围，不能为了测试新增整个公共 ReloadService。

成功重绑定后原旧草稿应按现有策略终结；失败重绑定保留旧绑定、相机和可解释的旧草稿；临时 BUSY 不能清空。

## R10-R1-04：两个图视图的排队 BEGIN

分别对 MaterialView 和 FlowView：

- 图显示 S0；产生实际 CanvasMove（绝对位置）或明确会冲突的连接/删除事件。
- 确认事件已经经过正式接线进入 View 的待执行状态，但 BEGIN 尚未执行。
- 另一路改同一目标得到 S1。
- 驱动原 maintain，检查请求没有因调用 beginEdit 而取得 S1 作为新来源。

要求：旧来源拒绝、S1 不变、没有伪 History、没有无限重试 stale 队首。正向同来源事件正常提交；多帧 Preview／鼠标释放仍仅一条历史。

试验可在 owner 上设置两个正式操作间的顺序；无需数据竞争。受控事件不等于实测系统原生鼠标，口径分开。

## R10-R1-05：BUSY 与错误清理

在真实 read gate 或另一个槽位回收时，让旧输入暂时无法推进。检查地址／载荷／based_on／stage 未改且不提前析构。

解除 BUSY：

- 内容仍同戳：继续原阶段并恰好一次完成，不重启第二手势。
- 内容变戳：拒绝原来源，不自动补新戳。

Material 动态节点／字段 payload 的自定义清理观察应确认 code/data 寿命与原 gate 不回归。不要通过放宽原 Store 门禁使测试成功。有限队列在显式取消后恢复容量。

## R10-R1-06：真实恢复操作与整体回归

发生冲突后，操作真实 Revert properties 或重选；新的节点值与戳来自同一次捕获。用户重新编辑后 Apply 成功，恰好一条 History。另一窗口可收敛，保存/编译捕获不包含未提交草稿。

保留并执行：185 项原行为、PLAYER 11、14 组 SDK 97 及新增场景、8 项 operation 编译负例、实际依赖负例、正式双 SceneView GPU／readback、原生输入和两个旧显式 GPU 模式。

X10-04 功能矩阵需要注明哪些项走了真实属性/画布处理，哪些是公开领域入口测试，不能再将后者当作前者的等价覆盖。系统 IME 没有实测则仍是 NOT_RUN。

## 建议结果字段

日志可记录 `fixture/model`、`input_content`、`display_content`、`live_content`、`draft_retained`、`source_unchanged_after_conflict`、`history_unchanged`、`revert_recaptured`、`accepted_once`、`queue_capacity_recovered`。

这些是测试观测，不要求将所有字段新增到生产状态。最终收据以实测名称和数量为准，不硬编码预期新增几个 CTest 就算验收。
