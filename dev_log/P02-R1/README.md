# P02 R1：定向补正与目录收拢验收

**状态：PASS，仅 P02；等待复审，不进入 P03。**

输入：`3a4d6ce367cfe3b9551ccde01a0de6d7088fa256`。
功能补正：`8ef17a1014ca02265c535d7855cf6848b8f9b936`。
目录收拢及最终验收实现：`5d266dfa29877d4e226ac7d04ea38d55e4f3d86d`。
本目录由独立验收提交冻结。唯一可变施工账本仍是 `.internal/editor-redesign/`。
原 P00、P01、P01-R1、P02 的收据、SHA、日志和成绩没有改写；没有修改 main。

## 真实修复前结果

在输入提交上，仅加入真实 CPU 模型测试和保留已有 lambda 排版，未修改产品语义。
完整输出、退出码及测试补丁见 `negative/`。复审包及逐文件 SHA 见 `spec/`。

| 回归 | 修复前实际结果 | 最终证明 |
| --- | --- | --- |
| R02-01 | apply 成功，但完整替换载荷比较失败 | 字段→删除→重加保留新 translation/scale；一次 Undo/Redo 恢复完整前后状态 |
| R02-02 | 字段→删除被错误拒绝，INVALID_COMPONENT | 成功后组件不存在；Undo 恢复原组件 |
| R02-03 | apply 成功，新载荷被旧 scratch 覆盖 | 重加后修改 scale 以新载荷为基线；同周期重复字段最后一次生效 |
| R02-04 | 原本已通过，未伪造失败 | 非法字段时序全批拒绝；全部对象、三份配置编码、历史 cursor/entry_count/current、变化游标、dirty 不变 |
| R02-05 | nested_apply=1，快照戳不变而 live 戳和正文改变 | nested_apply=0，SESSION/BUSY；快照戳、正文及变化游标同属 S0 |
| R02-06 | 原本已通过，未伪造失败 | codec 返回错误/抛普通异常后恢复；capture 和 owning component 两条路径均验证；后续编辑成功 |
| R02-07 | 读取回调能取得关闭许可，拒绝断言失败 | close/rebind/undo/redo/reload adopt 均 BUSY；候选未消费，退出读取后可采用；已有关闭许可拒绝读取 |
| R02-08 | owning component 编码期间 nested_apply=1 | 同一 gate 覆盖完整 codec；捕获值析构回调也不能重入修改 |

原 pluginSnapshot 继续验证普通字段编辑不解码无关插件组件，未知 payload 和插件代码寿命原断言继续运行。
未把失败标成 WILL_FAIL，也未缩减原场景语义。

## 改动与唯一所有权

`SceneEditBatch::edits` 在同一组件上按顺序解释。Remove 同时结束该 object/schema 的私有 scratch
和字段 delta；之后 Add 提供新基线，之后字段操作首次解码新载荷。删除后没有重建的字段操作整体失败。
沿用原结构候选及单条 History；没有第二套批次模式、组件 generation 或整源字段编辑回退。

`SessionState` 仍拥有唯一 EditGate，增加 READING 和 `withRead()`。私有栈作用域覆盖编码、错误返回、
异常展开及 callback 局部值析构。SceneSession::capture 和 SceneReadView::component 都进入同一 gate。
View 只借用 gate，不成为会话 owner 或长期共享锁；无回调的同步查询保留原借用失效契约。
没有 capture_busy、第二份 current/dirty，也没有通过末尾版本比较冒充源寿命保护。

所有权仍为 `SessionStore → SceneSession → SessionState + SceneSource + 唯一 EditHistory`。
R1 的私有、无分配 currentContent 和关闭内容戳校验未改。没有新增过渡桥。
旧 Registry/驻留/UI 适配及唯一 LegacyPersistenceState 按原限定消费者和 **P12** 期限暂留。

## S01 目录与接口

| 原物理路径 | 新物理路径 | 不变项 |
| --- | --- | --- |
| editor/history | editor/editing/history | edit_history、公开 include、命名空间、库名和 package |
| editor/sessions | editor/editing/sessions | edit_sessions、公开 include、命名空间、库名和 package |

`editor_editing` 仍是旧协议 target；没有把新历史/会话源码合并到兼容 target。
旧两个目录不存在，不留转发 CMake、头、别名、符号链接或备份源码目录。
原到期的 `editor/editing/src/EditHistory.cpp` 等精确路径仍不存在。

S01-01～05 证据：`evidence/structure.json`、移动前后实际 target 图、真实依赖负例及安装消费者日志。
移动后的库源码/公开头与功能提交逐字节相同，每个实现只编译一次；构建 target 及完整闭包不变。
规则只修改精确路径并增加两个旧目录到期检查，未扩大纯基础范围至整个旧 editing。
历史纯化检查、scope_compile、安装消费者及活动账本同步新路径；历史 fixture 中的旧路径明确固定到其 SHA。

`check_historical_evidence.py` 在临时目录按各阶段 implementation_sha 物化源码，调用未经改写的原收据验证器。
P01、P01-R1、P02 原门禁都通过。P00 没有独立原验证器，此处核验其历史 tracked 源码存在性及仓库 fixture
哈希，不宣称重新验证未归档的生产机器原始日志。P00 的 JSON fixture 原 hash 来自 Windows CRLF checkout，
核验明确恢复该编码；二进制 fixture 使用原 Git bytes。生成的机器路径仅作历史来源，不拿当前文件代替。

## 最终验证

所有正式命令绑定最终实现 SHA，显式 `LUX_EDITOR_MIGRATION_STAGE=P02`。

- 全量 `all -j 4 -- -k 0` 成功；第二轮 `ninja: no work to do`。
- 完整 CTest **51/51**：原 43 项保留，新增八项真实模型回归；另运行逐场景详细输出、P01/R1 关闭及 Session 回归。
- SDK 重新安装；六组安装消费者重新配置、构建、二次无工作和运行，合计七项测试通过。
- foundation/model 的直接、传递、alias、imported、LINK_ONLY、私有头、未知依赖负例均通过；每个非法夹具修复后同环境通过。
- 原 P01 证据搬运、缺失、篡改、缺日志与恢复测试再次通过；三阶段历史收据按源码 SHA 校验。
- clean tracked 校验后建立独立干净 clone，配置与真实依赖闭包通过。此项是独立配置/闭包验证，不声称又做了一次 clone 全量构建。
- 安装可执行文件 imports 无旧 Editor/Context/Runtime、ProjectStorage 实现和 GPU 动态库反向依赖。

测试与构建顺序执行。无 modules 公共头变化，三个安装前缀头同步条件未触发；Editor SDK 已重装。
未做 Android、额外人工 IME 或本轮不涉及的产品流程扩展验证。

运行 `python dev_log/P02-R1/check_receipt.py` 执行本轮最终 P02 门禁。
验证器只按归档相对路径读取证据；缺失或 hash 不符立即失败。命令中的机器绝对路径仅作来源。
文件增删改和 Git 内容 SHA 见 `files.json`，冻结文件 SHA 见 `artifacts.json`。

## 保留失败、限制与交接

| 既有缺陷 | 实际结果 | 后续责任 |
| --- | --- | --- |
| C01 | FAIL / exit 1；visible_before=0 visible_after=1 | P09/P12 |
| C03 | FAIL / exit 1；released_during_query=1 | P11 |
| C04 | FAIL / exit 1；create_succeeded=1 outcome_succeeded=0 | P12 |

复现源码、断言、判定未改，不顺手修复。
新增测试的配置编码比较曾因缺少显式 AssetEncodeLimits 编译失败，日志保留于 `logs/development`，
修正测试调用后重新全量构建及最终固定 SHA 验收。修复前真实缺陷输出与这些编译错误分别归档。

G01：原 SceneObjectEdit.cpp lambda 排版差异经空白核对后原样纳入功能提交；diff 和原件 hash 见
`local-format/`，没有 reset 或覆盖用户修改。它不被追认为旧 P02 已验证内容，本次成绩覆盖新实现。

既有索引 World 内容编辑限制、结构编辑完整 CPU 候选成本、快照预算非 RSS 上限保持不变。
P02 必测无缺项；没有进入 P03。阶段交付后等待复审。
