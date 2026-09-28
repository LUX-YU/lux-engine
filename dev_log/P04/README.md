# P04 验收：独立 Flow 作者模型

状态：**PASS（P04 范围）**。仅完成 P04，停下等待复审；没有实施 P05。

前置验收：`6a4ebe953ad227838147f28c11b2e57cb6d3fb35`。
实现提交：`89461d7e6`（完整 SHA 见 [receipt.json](receipt.json)）。
分支：`codex/editor-redesign-v4`。开始时工作区干净，未 reset、未修改 main。
本目录是阶段末冻结快照；唯一可变施工材料仍在 `.internal/editor-redesign/`。

## 实现与唯一所有权

- `SessionStore` 唯一拥有 `FlowSession`；Session 组合原 `SessionState`、唯一 `EditHistory` 与可写 `FlowAuthoringSource{id,name,FlowGraph}`。未增加 current、dirty 或 busy 副本。
- 私有 `currentContent()` 只查询身份及 History 当前内容戳，不调用分配型 `describe()`。所有回调读取、冻结捕获、临时清理与异常展开共用既有 `withRead()` gate。
- `FlowSourceEnvironment` 提供精确的元信息及其寿命，源/历史先于环境/代码析构。`FlowSnapshot` 持有完全拥有的 `FlowSource` 值，不借用 Node、RuntimeObject、元信息或 Session，因而无需多余的代码 lease。
- 节点/连接、变量、函数签名、导出、字面值和持久布局使用真实 CPU 图。普通单项编辑沿用局部准备；混合批次在候选图中按顺序应用同一套算法，全部通过后提交一条历史，Undo/Redo 也是一次操作。
- 消费输入按“代码 owner → 节点数据”的成员顺序形成局部拥有单元；获得 READING/EDITING 后首先移入回调作用域，早退清理仍在 gate 内。拒绝准入时不改变已有 gate。

新模型位于 `editor/tools/flowforge/model/`，只有一个 STATIC target `flowforge_model`，安装包为 `lux-engine-editor-flowforge-model`。
其实际闭包只包含历史、会话及纯 Flow/元信息依赖，无旧 Context、UI、SceneRuntime、ProjectStorage、Flow compiler/linker 或脚本执行服务。

## 迁出、删除与暂留

| 原路径/职责 | 本阶段结果 | 暂留期限 |
|---|---|---|
| `Impl::Content` | 改为唯一 `FlowAuthoringSource`，不留别名 | 无旧类型 |
| `VariableEdit/LiteralEdit/TValueEdit/GraphDelta/GraphEdit/ExportsAccess` 及纯校验 | 原体与声明删除，迁入 `model/src/edits/FlowGraphEdit.cpp`；旧调用改用同一算法 | 无重复算法 |
| 旧产品字段、图/变量编辑入口 | 只组装意图并调用纯模型准备与原 History | P12 |
| `read_nodes_/read_pins_/indexContent` | 旧 UI 的派生目录，结构发布时更新；新模型不重复保存 | P10/P12 |
| UI、编译/链接、保存/加载与资产切换 | 旧产品暂留，新模型不反向依赖 | P05/P07/P10 接管，最迟 P12 删除旧壳 |
| `prepareBorrowedFlowInsert` | 限旧 Flow 产品消费，保留原失败时不夺走调用方 unique_ptr 的契约 | P12 |
| `LegacyPersistenceState` 私有桥 | 沿用原白名单、原消费者和原期限，未扩大范围 | P12 |

`FlowGraph` 仅新增保持变量 ID 高水位的窄方法，防止候选图交换及 Undo 后分支重用已发出变量 ID；没有改磁盘格式。
所有文件级新增/修改及实现 blob SHA256 在 [files.json](files.json)。本次 25 个源/配置文件，15 个新增、10 个修改；旧算法按符号删除，未删除仍承担产品功能的整文件。
具体成员迁移和限定消费者见 [migration-ledger.json](migration-ledger.json)。

## 运行证据

所有最终运行绑定同一实现 SHA，显式 `LUX_EDITOR_MIGRATION_STAGE=P04`：

| 检查 | 结果与观察 |
|---|---|
| 全量 `all -j 4 -- -k 0` 与第二轮 | PASS；第二轮 `ninja: no work to do` |
| 完整 CTest | 82/82；原 64 项名称和原断言保留，不以数量替代行为证据 |
| X04-01 | 真正的变量/函数/导出/连接/布局编辑、撤销重做、冻结编码；模型无需 linker |
| X04-02 | 引用变量删除/改型、非法签名/连接/导出拒绝，完整源、历史、observed、dirty、绑定保持 |
| X04-03 | 真实 RefClass/RefFunction 数组、变量 RuntimeObject、动态节点析构及 memento 寿命；最后 owner 与输入清理 |
| X04-04 | SceneSession、MaterialSession、FlowSession 同一个 Store；正确/错误 typed key、关闭许可和独立历史/快照 |
| 继承契约 | 字段→签名替换→字段；字段→删除→同 ID 重建；读取回调/临时析构/异常展开；输入被拒与最后 lease 清理 |
| 历史回归 | P01/P01-R1/P02/P02-R1/P03/P03-R1 原行为测试及固定实现 SHA 的归档核验全部通过 |
| SDK | 重新安装；8 组独立消费者构建、第二轮无工作、运行共 9 项通过；新 Flow consumer 只使用安装包 |
| 依赖负例 | Flow 18 个真实 CMake/源码负例；禁止边实际失败，同一夹具修复后通过；原三套负例保留 |
| 固定源码资格 | clean tracked commit 验证；该 SHA 的独立 clean clone 配置与真实 target 闭包核验 |
| 证据可移植性 | 仅归档文件及 Git 对象、无生产源码/安装/构建路径时可验；缺日志、改日志或缺前置收据必定失败 |

对应原始日志见 [receipt.json](receipt.json) 的 commands；固定测试名在 [logs/test-names.log](logs/test-names.log)。
验证器为 [check_receipt.py](check_receipt.py)，按各自实现 SHA 验证历史源码；原阶段快照未改写。

Q 场景按阶段范围报告，不宣称后续完整工作通过：

- **Q01**：无窗口、GPU、ProjectStorage 的真实 Flow 域及独立链接闭包。
- **Q06**：三种真实会话 typed key、关闭后失效；继承槽位 generation 回归，Flow 过期内容/重载采用拒绝。运行对象属于后续阶段。
- **Q09**：准备校验、限额与候选失败不产生半次提交。未引入新的 UI 选择状态。
- **Q11**：冻结后继续编辑/关闭，冻结值仍可编码；稳定读取和环境寿命成立。新异步保存/发布编排留 P05。
- **Q49**：新增 native 模型及安装消费者没有 PowerShell、定制 LLVM 路径或 linker 命令依赖。当前实测 Windows/MSVC，未宣称跨平台 CI 完成；旧工具编译/链接回归仍启用。

## 保留失败与实际限制

| 编号 | 本次复跑判定 | 原责任 |
|---|---|---|
| C01 | **FAIL**：`visible_before=0 visible_after=1` | P09/P12 |
| C03 | **FAIL**：`released_during_query=1` | P11 |
| C04 | **FAIL**：`create_succeeded=1 outcome_succeeded=0` | P12 |

三项仍返回失败退出码，保留原探针、判定和日志，未顺手修复。没有把新 Flow 问题挂到旧编号延期。

混合批次准备/历史目前承担整图成本；单意图字段编辑仍局部处理。限额核算已知载荷/容器成本，不等于进程 RSS。
现有 Flow codec 节点种类和 schema 保持，不实现任意插件节点序列化框架。元信息寿命测试使用真实对象与最后 owner 析构观察，不冒称实机 DLL 卸载认证。
独立 clean clone 验证配置与闭包，不声称执行第二套全量引擎构建。公共头同步 Debug、RelWithDebInfo、Android include；没有 Android 构建。
既有自动桌面/GPU、Flow GUI/编译/链接回归已重跑；本 CPU 作者模型阶段没有新增人工桌面、IME 或跨平台矩阵。

开发中真实出现的失败及修正理由见 [implementation-notes.md](implementation-notes.md)，原日志保存在 `logs/development/`。
没有未通过的 P04 必测项；以上三项是原规范明确保留的失败。后续工作需复审放行，不自动进入 P05。
