# P04 R1 验收：Flow 已发号身份高水位

**状态：PASS（P04 R1 范围）；停在 P04 等待复审，不进入 P05。**

前置验收为 `ced216040539d022a6564a896210b00a92e6f06e`，实现提交为 `af8310f3`（完整 SHA 见 [receipt.json](receipt.json)）。
分支仍为 `codex/editor-redesign-v4`。开始时工作区干净，前置等于 HEAD；未 reset、未修改 main。
本目录是冻结验收快照，唯一可变账本仍位于 `.internal/editor-redesign/`。原 P04 和所有更早快照未改写。

## 修复前的真实负例

先只增加真实 `FlowSession` 测试与现有 target 的场景登记，在原验收生产代码上全量构建并执行。
附件缩小算法探针没有作为本机验收结果使用。

| 场景 | 原图曾发出的身份 | 错误的新身份 | 原结果 |
|---|---|---|---|
| R04-01 混合插入 → Undo → 自动新建 | NodeId 4；PinId 8、9、10、11 | NodeId 4；PinId 8、9、10、11 | 节点和全部 Pin 复用；携带最新 ContentStamp 的旧 Pin 写入被接受 |
| R04-02 删除最高节点 → 活记录重建混合候选 | NodeId 4；PinId 8、9、10、11 | NodeId 4；PinId 8、9、10、11 | 候选准备期间重新发出旧号 |

两项真实断言均失败，退出码 `3221226505`。编号来自本次实际 Fixture，不是测试硬编码的预期起点。
生产 SHA、测试差异哈希、退出码见 [before/results.json](before/results.json)，原输出及仅测试补丁也保存在 `before/`。
修复后两项的新身份均为 NodeId 5、PinId 12、13、14、15，旧 Pin 写入被拒绝，完整状态不因拒绝改变。

## 窄修复与所有权

- `GraphTopology::preserveIssuedIdsFrom()` 合并其既有节点/Pin 游标；不分配、不重编号、不改节点/边、也不发布通知。
- `FlowGraph::preserveIssuedIdsFrom()` 委托 topology，同时保留既有变量游标。
- 删除 `preserveVariableIdsFrom()` 及全部四处生产调用；不留转发别名或同义壳。
- 接线覆盖输入图捕获后的 create 复制边界、混合候选执行任何发号意图之前、提交及每次 Undo/Redo 的双向交换。
- 拓扑 `0` 耗尽是吸收状态。`advanceNodeId/advancePinId` 不再因显式插入/恢复较小旧 ID 重新启动发号；变量 `UINT64_MAX` 耗尽规则保持。
- 正常 Redo 和显式 `preserve_ids` 恢复原身份，随后自动新建仍不得复用已发布身份。

身份状态仍只有图中原来的三个游标；没有 Session 计数器副本，没有另一套 History、current、dirty 或 busy。
失败或 NO_CHANGE 候选中从未发布的临时号码允许随候选丢弃；不为维护游标伪造历史提交。
同一 Session/History 域的已发号身份不回退；不要求跨文件或新 History 的全局单调。

仅修改 [files.json](files.json) 所列 8 个文件。生产改动限于两个图类型的窄接口和四处调用；其余为原测试 target 的六个场景及安装消费者回归。
无目录、target、include 路径、包名或磁盘格式变化。普通单项编辑仍局部准备，原混合批次算法、读取 gate 和输入清理保持。
旧 UI/编译/链接/IO 及私有桥的消费者与 P12 删除期限不变，未进入 P05。

## 行为及工程验收

| 场景 | 具体观察 |
|---|---|
| R04-01 | 同一 HistoryId；Undo 后 NodeId 与全部 PinId 不重叠；最新内容戳 + 旧数据 Pin 拒绝；完整源、历史、observed、dirty、绑定、checkpoint 不变 |
| R04-02 | 候选发号前继承删除记录的高水位；混合批次仅一条历史；Undo/Redo 的完整编码一致 |
| R04-03 | 节点身份不变时扩展签名，分别 Undo/收缩后另开分支；位置稳定的旧 Pin 保留、新 Pin 与旧已发号集合不重叠；合法边和完整回放保持；原字面值/签名回归继续执行 |
| R04-04 | Redo 与显式恢复得到原 ID 和完整编码；再次自动新建仍用新号；额外覆盖 create 从输入活记录重建时继承已删除节点/Pin 高水位 |
| R04-05 | 候选插入后遇到非法变量/连接；预算不足；净效果相同批次；完整状态不产生半提交或伪造历史，后续正常编辑可用 |
| R04-06 | 直接使用实际共享 GraphTopology/FlowGraph：正常、双向、自合并，极值耗尽的空源，节点/Pin 独立耗尽，较小身份显式恢复/插入，变量耗尽 |

仓库没有独立的 GraphTopology 单元测试 target。低层行为直接加入现有 Flow native 测试程序的 R04-06，未新增测试框架或 target；完整回归同时覆盖共享图的 Material 和旧 Flow 产品消费者。

最终验证绑定同一实现 SHA，显式 `LUX_EDITOR_MIGRATION_STAGE=P04`：

- 全量 `all -j 4 -- -k 0` 通过；第二轮 `ninja: no work to do`。
- 完整 CTest **88/88**，保留原 82 项及原断言。原 Flow 模型、三实际会话、P01/P02/P03/R1、旧产品和共享图消费者均参与。
- 重新安装 RelWithDebInfo SDK；原八组消费者重新构建、第二轮无工作、运行 **9/9**。Flow 安装消费者另实际执行混合插入/Undo/旧 Pin 拒绝，记录独立 stdout。
- 两个修改的 modules 公共头同步到 Debug、RelWithDebInfo、Android include，共六份归档核对；没有 Android 构建。
- 原四套实际依赖负例继续通过：基础 13、Scene 10、Material 16、Flow 18。禁止边失败、同一夹具修复后通过，规则未放宽。
- clean tracked 实现提交核验；独立 clean clone 的配置与真实依赖闭包通过。未冒称在 clone 中又执行一次全量构建。
- 原阶段验证器按各自实现 SHA 核验，原 P04 门禁也重新运行。新增 [P04 R1 门禁](check_receipt.py) 明确使用 P04，检查修复前后证据、原断言、删除项及文件范围。
- 只携带归档与 Git 对象的迁移位置可以核验；缺必需日志、改日志或缺前置收据均失败，恢复后通过。

实际命令、日志、哈希和逐项结果见 [receipt.json](receipt.json)；不以测试总数代替六项身份观察。

## 保留失败及限制

| 编号 | 原判定及本次复跑 | 原责任 |
|---|---|---|
| C01 | **FAIL**，`visible_before=0 visible_after=1` | P09/P12 |
| C03 | **FAIL**，`released_during_query=1` | P11 |
| C04 | **FAIL**，`create_succeeded=1 outcome_succeeded=0` | P12 |

三项原探针继续返回失败退出码，没有顺手修复或改判定；本次 Flow 身份缺陷已独立补正，没有挂到旧编号延期。
P04 R1 无未通过的必测项。实测环境仍为 Windows/MSVC；未新增跨平台、人工 IME、实机 DLL 卸载或 Android 资格声明。
原 P04 整图混合候选成本、codec 种类与有限身份空间等限制继续保留。
