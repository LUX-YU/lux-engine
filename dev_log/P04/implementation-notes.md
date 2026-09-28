# P04 施工决定与交接

本轮仅 P04，起点 6a4ebe953ad227838147f28c11b2e57cb6d3fb35，分支 codex/editor-redesign-v4，初始工作区干净。
启动包 SHA256 4701690e6862cd96ddc439b5c3424f1005fbf45defaf82d4b4d940e40a7dcd5a；包内 P04 原件与现存 spec-v4 原件相同。
已核对 P00、P01、P01-R1、P02、P02-R1、P03、P03-R1 收据与当前祖先关系。唯一可变账本仍为本目录三份 JSON。
本说明解释实现决议，不建立第二份成员清单；阶段末冻结到 dev_log/P04。

## 所有权与读取

FlowSession 由 SessionStore 独占，直接组合既有 SessionState、唯一 EditHistory 和唯一可写 FlowAuthoringSource。
后者准确命名原 Content{id,name,FlowGraph}，不与另一份可写 FlowSource 同步。
私有 currentContent 从 SessionId 与 History 当前状态取得，不调用 describe、不另存 current/dirty/busy。

FlowSourceEnvironment 的 code_lifetime 必须拥有 span 底层数组、描述符以及代码。静态元信息可以没有 lease。
源和历史先析构，环境与代码后释放。测试用真实 RefClass/RefFunction、底层数组、变量 RuntimeObject 和派生 BranchNode 析构观察证明此序。
不是只用一直存在的外部 DLL 引用冒充元信息寿命。源码已有纯 Flow 库，不引入 compiler、linker、UI、SceneRuntime、ProjectStorage。

const FlowGraph 的 unique_ptr/RuntimeObject 会泄漏可写能力，因此 FlowReadView::withRead 暴露完全拥有的 FlowSource 值，
整个捕获、用户 codec 回调、临时清理与异常展开在同一 READING 中。普通编辑只针对指定值；读取冻结值是显式 O(图) 操作。
FlowSnapshot 内只有 scalar/string/vector/variant 值，无 Node、RuntimeObject、描述符或虚析构，因此不保留无必要的代码 lease；
测试在元信息最后 owner 释放后仍编码快照。它不是 FlowGraph clone。

## 编辑与混合批次

原 VariableEdit/LiteralEdit/TValueEdit/GraphDelta/GraphEdit/ExportsAccess 和纯校验全部迁入 model/src/edits。
旧 Editor 中删除算法体及声明，旧调用只组装意图，仍执行同一个 History；不为旧工具创建一份同步 FlowSession。
旧图索引是派生 UI 数据，仅在结构发布时更新，字段/布局操作不重新构建索引。模型不会向读取者暴露裸可写 Node/Pin。

单条意图继续使用原局部准备协议。多条意图在隔离候选图中逐条执行相同操作的准备与内容提交，
不建立第二个 History，不发布中间变化，不保存旧字段 scratch。全部成功后，一次 History 提交交换整图；undo/redo 同样一次交换。
混合批次因此临时复制且历史保留完整图；单项编辑仍保留局部 memento。现阶段明确接受这项成本，不宣称差异化混合历史或零拷贝。
候选和输入受现有 History staging/retained 限额约束，估算对函数 use 展开预留空间并在提交前核对实际图；限额不是进程 RSS。
FlowGraph::preserveVariableIdsFrom 仅保留既有单调变量 ID 高水位，使候选重建/交换及 undo 后分支不会复用已发出的变量 ID。

签名替换沿用旧定义：同步替换定义及调用/返回节点，按位置保留 PinId，校验已有字面值和边；非法移除/类型改变拒绝，不静默丢边。
引用中的变量不能删除或改类型。FlowInsertNode 默认为分配新 ID；明确 preserve_ids 时按已有源身份恢复，重复 ID/非法边仍拒绝。
测试覆盖 字段→签名替换→字段，以及 字段→删除→按相同节点/PinId 重建；全图及 History/observed/checkpoint/绑定核对。
净效果等于原图时 NO_CHANGE，不凭中间修改次数产生历史。

## 输入准入与兼容边界

消费输入与环境/代码形成局部拥有单元；取得准入的 lambda 第一条语句把输入移入其作用域，再做任何提前拒绝。
准入失败由外层拥有单元销毁节点再释放代码，不解锁已有 READING/CLOSING。创建、apply、私有 PreparedFlowReload 都遵守这一点。
私有重载候选仅是作者模型协议测试与未来适配基础，不实现 P05 IO 服务。

prepareBorrowedFlowInsert 为原 reference-taking API 保留“拒绝时调用者仍有原 unique_ptr”的协议，只有旧 FlowForge 产品消费，最迟 P12 删除。
纯函数只认识作者源、环境和 History 值，不反向认识旧 Editor/Context；没有新增 transition bridge 或兼容 target。
旧 UI/图布局交互、编译/链接、保存 IO、候选资产切换仍分别归 P07/P10/P12；详细期限在唯一迁移账本中。

## 验证与保留项

保留原 64 项测试及断言，新增真实 Flow 域、三种实际会话同 Store、同夹具修复后通过的 CMake 依赖负例和安装消费者。
当前开发全量构建及 82/82 CTest 通过，正式验收必须绑定最终实现 SHA 再执行，不以开发结果替代。
早期新测试的实际失败日志保留：新增函数 use 的上界估算不足；一个净效果相同的重建用例错误期待新增历史；测试构造签名/namespace 拼写错误。
前者修正估算；重建用例增加真实布局差异并继续核对载荷及一条历史，另保留 NO_CHANGE 验证。
C01/C03/C04 持续保留 FAIL，责任分别 P09/P12、P11、P12。不修复，不改判定，不把 Flow 新问题挂旧编号。
不改变磁盘格式，不新增任意插件节点 schema/编译框架；Flow 只处理现有 codec 能完整表达的节点语义。
只重跑既有自动桌面/GPU回归，不声称新增人工 IME/跨平台/DLL卸载资格；Android 仅同步公共头，不构建。
