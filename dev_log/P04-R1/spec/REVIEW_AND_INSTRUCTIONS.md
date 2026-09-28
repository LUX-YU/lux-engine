# P04 复审 R1：Flow 混合批次的节点与 Pin 身份高水位

**日期：2026-09-28**  
**项目：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**本次已读取验收 HEAD：`ced216040539d022a6564a896210b00a92e6f06e`**  
**P04 实现：`89461d7e6d7b734b9eafbe48795e5b3cfd172a68`**  
**已验收前置：`6a4ebe953ad227838147f28c11b2e57cb6d3fb35`**

## 0. 结论与证据边界

**暂不进入 P05。P04 主体架构应保留，只补正同一身份分配缺口的两个发生位置，再停在 P04 复审。**

新 FlowSession、唯一 History/SessionState、元信息所有权、稳定读取、输入清理与三实际会话共用 Store 已有实际实现和相应测试。这不是重做 P04，不是增加另一套图框架，也不是借复审继续扩张目录。

本轮发现：**混合批次只同步了变量 ID 高水位，没有同步 `GraphTopology` 内的 NodeId 与 PinId 高水位。** 候选从活节点重建时会丢失已删除身份的分配记录；交换到较旧图时又会回退计数器。结果可能是新的自动分配重复使用已经发给另一个对象的 ID。

| 证据类别 | 本次完成 | 不代表什么 |
|---|---|---|
| 仓库源码 | 固定 HEAD/父链、FlowSession、编辑准备/整图回放、GraphTopology 分配、FlowGraph 移动与插入、测试、旧入口与 CMake | 不是逐行审遍全仓或全套独立验收 |
| 源码路径推导 | 两条可由公开 FlowSession 操作触发的 NodeId/PinId 重用路径；旧测试只保护变量高水位 | 实际新增 Lux 回归尚未在本环境运行 |
| 独立 C++20 算法探针 | Clang 17 与 GCC 14.2，各 O0/O2；变量-only 保留的旧形状与三类高水位保留的对照 | 不编译 Lux，不使用 SDK/真实 FlowGraph，不是 CTest，也不是正式修复补丁 |
| 实施方归档 | 82/82 CTest；README 报告八组消费者 9/9、显式 P04 门禁和保留失败 | 不冒称是复审方独立重跑；未独立复算全部远端日志哈希 |

容器对 raw.githubusercontent.com 的 DNS 解析失败；本次通过 GitHub 连接器读取原仓库。探针是独立编写的缩小算法模型，源码与输出全部附带，不能用它替代下文真实 Flow 测试。

## 1. 必须保留的 P04 成果

### 1.1 模型及目录

`FlowSession::Impl` 组合 SessionState、FlowSourceEnvironment、代码保活容器、FlowAuthoringSource 和唯一 History。声明次序让源/历史早于环境和代码析构。作者源是 `{id,name,FlowGraph}`；FlowSource 是拥有型捕获值，不是与作者图双向同步的另一份可写源。[S3][S4]

`flowforge_model` 位于 `editor/tools/flowforge/model/`，只建立一个 STATIC target。继续使用 `editor/editing/history/` 和 `editor/editing/sessions/`；不得恢复两个旧根目录，不新增 identity_manager、FlowHistory 或新编辑 target。[S10]

### 1.2 元信息与读取

FlowReadView 给回调提供完全拥有的 FlowSource，而不是依靠 `const FlowGraph` 假装深 const。capture/withRead 在同一 gate 内；私有重载输入先组成环境—源的拥有单元，获准后首先移入回调局部，保留前两轮输入清理修正。[S3][S5]

元信息测试使用实际 RefClass/RefFunction 数组、RuntimeObject 和派生节点析构；释放外部 owner 后仍验证历史停放节点寿命。快照在元信息释放后仍可编码。不要因本轮发现另一问题撤销这些设计。[S9]

### 1.3 真实共同基础与旧算法迁移

`three_sessions.cpp` 使用真实 SceneSession、MaterialSession 和 FlowSession：同一 Store 中发布、typed key、修改、关闭与快照独立性有明确断言，不是三份 Fake。[S11]

旧 FlowForgeEditor 的 applyEdit/insertEdit 已经调用纯 `prepareFlowEdit()`；保留这个单向复用。当前旧 UI/编译/IO 仍按期限承担产品功能，不应整目录删除。`prepareBorrowedFlowInsert` 保持已有限定消费者和 P12 期限，不扩为新模型的入口。[S12]

## 2. B01：内容可撤销，已发出身份的分配历史不可回退

### 2.1 三类身份当前不一致

当前实现：

```cpp
// FlowGraph.hpp
void preserveVariableIdsFrom(const FlowGraph& source) noexcept {
    if (source.next_var_id_ > next_var_id_)
        next_var_id_ = source.next_var_id_;
}
```

混合批次准备：

```cpp
auto graph = materializeFlowSource(*frozen, owner_.environment_);
// 只从现存节点/pin/变量重建图。
candidate->graph.preserveVariableIdsFrom(owner_.source_.graph);
```

提交/回放：

```cpp
live.graph.preserveVariableIdsFrom(next.graph);
next.graph.preserveVariableIdsFrom(live.graph);
swap(live.name, next.name);
swap(live.graph, next.graph);
```

但是 `GraphTopology` 自己保存 `next_node_id_`、`next_pin_id_`，`addNode/addPin` 从这里取号；`FlowGraph` 的移动会一起移动 topology，而不是把其高水位留给逻辑作者会话。[S1][S2][S6][S7]

`FlowSource` 捕获只保存实际节点和 pin 等值，不保存历史分配游标。活记录重建不能推导已经删除/撤销的最高身份。`preserveVariableIdsFrom` 恰好只修正变量，不能替节点和 pin 同步。[S1][S8]

### 2.2 身份应有的准确含义

在**同一 Session/History 身份域**内：

- Undo 可以使节点/Pin 从当前作者内容消失。
- Redo 可以让同一个历史对象恢复原 NodeId/PinId，这是恢复，不是向新对象分配旧号。
- 自动新建另一个对象，不能复用已经对外发出的节点/Pin 身份。
- 显式 `preserve_ids` 是现有恢复协议；本轮不取消它，也不把它等同于普通自动分配。
- 重载产生新的 HistoryId 后，是另一个内容身份域。本轮不要求跨所有文件、所有进程保持全局单调，也不修改磁盘格式。

**历史状态身份与对象身份不相互替代。** `batch.expected` 可阻止旧 StateId 的请求；但调用者可能读取最新内容戳，同时继续使用之前保存的 NodeId/PinId。新节点若占用旧号，该请求就可能合法命中错误对象。

这个问题不需要 GUI、并发或恶意插件。它发生在本阶段已有的自动新建、混合批次和 Undo 中。修复不会引入未来 P10 的选择功能；只要求作者身份本身不混淆。

## 3. 两条独立发生路径

### R04-01：混合插入 → Undo → 新分支插入

示例数值只用于说明；真实测试不要硬编码起始 ID。

```text
旧图 next_node=4，next_pin=20
    ↓ 混合批次：rename + 插入 A
A 获得 NodeId=4、若干 PinId；新图游标继续前进
    ↓ 提交交换
旧图进入 parked；其节点/pin游标仍停在4/20
    ↓ Undo交换
恢复旧内容，也恢复了旧分配游标
    ↓ 新建 B（普通自动分配，不是Redo，不是preserve_ids）
B 再拿到 NodeId=4；PinId也可能重复
```

变量游标已被双向合并，但节点/pin游标没有。修复前源码由这个调用链直接给出重复分配结果。[S1][S2][S7]

回归应额外使用**新的正确 ContentStamp＋原 A 的数据 PinId**执行 FlowSetLiteral。正确行为是目标不存在而拒绝，不修改 B；旧路径可能接受并修改 B。这样可以显示它不是单纯的计数器美观问题。

### R04-02：删除最高节点 → 从活记录创建混合候选

```text
单条插入 A，记录已发出的节点及全部pin IDs
    ↓ 删除 A
live的计数器仍正确保留高水位
    ↓ 开始混合批次：rename + 插入 B
capture只包含其它活节点
    ↓ materializeFlowSource
候选计数器按活记录重建，丢失 A 的发号记录
    ↓ 只继承变量游标
候选为 B 自动发出 A 的旧节点/pin身份
```

**只在最后交换前合并计数器，不能修正这条路径**：候选在准备过程中已经把旧号交给 B。必须在候选执行任何可能分配节点/Pin 的意图之前，继承原作者图的已发出身份状态。[S1][S6]

### Pin 独立验证

不能只比较 NodeId。签名扩展会给同一个定义/调用者/返回节点增加 Pin，而 NodeId 本身可能完全不变。应补测签名扩展、撤销/收缩后新分支扩展，确认自动新 Pin 不复用过去已发出 PinId；Redo 和保留位置的 PinId 仍按原身份恢复。[S1][S6]

## 4. 为什么 82 项通过仍没有覆盖它

已有 `content()` 确实测试混合事务发出变量 ID，Undo 后分支的新变量 ID 更大；已有 `mixed-recreate()` 又测试显式相同节点/PinId 恢复。[S9]

缺少的是：自动新建的 NodeId/PinId，在混合事务的候选重建和 Undo 分支后不得重复。显式恢复测试不能替代新身份分配测试；只测变量也不能证明拓扑游标。

原 82/82 是已执行的历史结果，不因新增缺口就改写为未执行或整体测试造假。应保留原报告，新增 P04-R1 记录本次缺口及修复。[S13]

## 5. 修正设计：保持原图模型，只补齐高水位合并

### 5.1 最小责任划分

不引入新 owner、新 UUID 格式、新全局分配器或第二份 History。

建议的接口方向：

```cpp
// GraphTopology：只负责自身节点和Pin的发号状态。
void preserveIssuedIdsFrom(const GraphTopology& source) noexcept;

// FlowGraph：委托topology，并保留现有变量发号状态。
void preserveIssuedIdsFrom(const FlowGraph& source) noexcept;
```

这是本轮建议的新名称，不是声称当前仓库已经存在。实现方应先确认精确符号是否冲突；没有必要再封装一个公共 `IdStateManager`。

现有 `FlowGraph::preserveVariableIdsFrom` 若被新方法全面替代，应连同声明和全部原调用删除；不留同义 forwarding alias。名称更新必须同步实际调用、当前账本、测试和安装头。原 dev_log 中引用旧名称的证据不应删除或伪装成当时使用新名。

### 5.2 需要接入的三个位置

1. **FlowSession 创建的复制边界**：由输入作者图捕获并重建图后，按现有变量策略将完整已发号状态继承给新图。不修改输入用户数据。
2. **FlowBatchEdit::prepare 候选起点**：materialize 完成后、执行 factory 意图前，继承 live 图的三类高水位。
3. **FlowBatchEdit::Plan::applyContent**：首次提交与每次 Undo/Redo 都把 live/parked 的三类高水位合并到不会回退的状态，然后交换内容。不能只在 execute 分支处理。

实现可通过双方取并集/最大已发号上界，或等价的明确定义完成。合并不改任何现存记录的身份，不修改边、不发布通知、不分配新 ID，也不使脏状态增加。

### 5.3 耗尽哨兵不可以当普通数值取max

当前 GraphTopology 用 `next_node_id_ == 0` / `next_pin_id_ == 0` 表示耗尽，而变量以 `UINT64_MAX` 拒绝继续分配。若直接对两个 next 值做 std::max，拓扑的“耗尽=0”可能被较小的正常高水位复活。[S6]

新增合并必须保持耗尽是吸收状态，并与现有显式插入/恢复的推进规则一致。低层测试用公开且合法的极值身份构造耗尽，不靠遍历 2^64 次。任何为完成这项一致性而调整的 `advanceNodeId/advancePinId` 行为，都要单独说明并跑共享图消费者回归。范围只限身份推进/合并，不能顺手重写 GraphTopology。

### 5.4 NO_CHANGE 与失败不应伪造提交

净效果相同批次仍应是 NO_CHANGE，不能为了维护 ID 游标而强行增加历史。失败候选不得改变当前作者图、历史、observed、dirty 或绑定。

本轮不要求失败候选内部、从未发布给外部的临时号码永久消耗；至少必须保住已发布的所有节点/Pin/变量身份。对尚未发布号码是否保留，明确现有策略，不借此引入全局日志。

## 6. 文件与符号处置表

| 文件/符号 | 本轮动作 | 不得做什么 |
|---|---|---|
| `modules/function/graph/include/lux/engine/function/graph/GraphTopology.hpp` | 为现有两个游标增加窄的保留接口，明确耗尽语义 | 不暴露可任意回退计数器的 setter，不新造拓扑模型 |
| `modules/function/graph/src/GraphTopology.cpp` | 实现节点/Pin高水位合并，核对极值和已有身份恢复规则 | 不改变图序列化格式或普通边验证 |
| `modules/function/flowforge/include/lux/engine/flowforge/graph/FlowGraph.hpp` | 将变量-only方法收敛为三类身份保留；委托现有topology | 不同时保留两套含义重叠的公开接口；不复制计数器到Session |
| `editor/tools/flowforge/model/src/FlowSession.cpp` | create中替换原变量-only继承调用；其余会话逻辑回归 | 不修改History或gate、不复制dirty/current |
| `model/src/edits/FlowGraphEdit.cpp` | prepare候选执行前、Plan提交/回放处替换原变量-only调用 | 不只修一处；不把局部单项编辑改为全部整图复制 |
| `model/test/flow_session.cpp` | 增加下面真实模型回归；保留原用例及断言 | 不用最小探针代替实际Flow测试 |
| `model/CMakeLists.txt` | 在现有测试target登记新增场景 | 不另造业务库或新测试框架 |
| 现有低层Graph测试位置 | 增加合并/耗尽/恢复测试；先定位实际文件 | 不凭文档虚构已有test文件；无现存目标时仅加同域小测试 |
| `cmake/installed-consumers/flowforge-model/` | 独立SDK消费者增加至少一项公开身份分配回归，或复用其真实模型入口 | 不偷用源码私有头/引擎构建目录库 |
| 当前 `.internal/editor-redesign/` | 更新规范补充、当前迁移账本、测试映射及新SHA | 不创建第二份独立变动账本 |
| `dev_log/P04-R1/` | 新冻结实现及修复前/后证据、限制、命令/哈希 | 原P04/P03等快照不改写 |
| 旧UI/编译/IO和私有桥 | 保持原限定消费者、原P12期限；做必要编译适配 | 不扩大桥，不实施P05，不修C01/C03/C04 |

公共 modules 头发生改变，按当前工程的 Debug/RelWithDebInfo/Android include 同步约定处理并记录；这不等于运行Android构建。本轮重装SDK、重编并运行受影响安装消费者，不能靠旧安装目录恰好存在新头通过。

## 7. 实施次序

### A. 固定反例

以本次验收SHA为前置，保留工作区与提交链。先为真实Flow模型新增反例，仅加入测试及登记，记录修复前结果；保留原输出、失败退出码、当时的生产源码SHA与测试差异。

原有Fixture可直接使用。附件 `regressions/flow_identity_cases.inc` 给出了前两条用例的代码方向；本环境未编译它，实施时核对现有命名/API并只做必要适配。它不是可以直接加入产品CMake的业务文件。

### B. 低层窄接口与接线

只改上表必要方法。先测试GraphTopology的身份保留和耗尽语义，再接FlowGraph三类身份保留，最后接候选创建、提交和回放。

明确合并行为不修改作者数据、不触发观察者。逐项删除旧方法/调用，不留下临时compat方法。

### C. 完整行为与工程回归

执行下文真实回归、原P04/P01/P02/P03/R1测试、共享Graph及Material消费者回归、依赖负例；显式配置P04门禁，全量构建，二次无工作，完整CTest，SDK重装与原八组消费者。

### D. 独立交付

实现与验收分开提交，正常推送实施分支，停在P04。测试结果绑定最终实现SHA，不用这份探针或旧82/82替代新代码成绩。

## 8. 必测的真实场景

| 编号 | 场景与输入 | 必须观察的结果 |
|---|---|---|
| R04-01 | 混合 rename+插入A；记录A的NodeId/全部PinId；Undo；自动新建B | B的自动节点/Pin身份不与A重叠；同HistoryId；新内容戳+旧A数据Pin的写入拒绝；B与History不被这次拒绝修改 |
| R04-02 | 单条插入最高节点A、删除A；混合rename+新建B | 候选在发号前已继承高水位；Node/Pin不复用；一次历史提交；Undo/Redo完整源一致 |
| R04-03 | 保持NodeId不变，扩展函数签名生成新Pin；Undo或删除最高参数后，在新分支重新扩展 | 自动新Pin不复用旧已发号；位置稳定的旧Pin仍保持原ID；合法边/字面值与签名策略不变 |
| R04-04 | 正常Redo恢复A，以及现有显式preserve_ids重建 | 恢复原身份仍成功；不能靠禁止Redo/恢复或每次随机重编号通过前面反例；再自动新建仍不复用既发号 |
| R04-05 | 候选先准备创建，再遇到非法变量/连接或预算失败；另有NO_CHANGE批次 | 整个源/History/current/observed/dirty/绑定/checkpoint不变；失败没有半发布；后续正常操作可用 |
| R04-06 | 低层节点/Pin极值耗尽与正常游标合并；双向合并、自合并、显式合法恢复 | 耗尽不会因合并或恢复较小ID变成重新发低号；不改现存节点/Pin；变量原耗尽规则不回归 |

将R04-01/02的复用布尔、旧新ID、旧Pin写入结果先printf并flush，再assert，保留可解释的失败证据。不能以“数量不同”替代地址唯一性验证。

对于全部测试，正常Redo后图编码应与首次提交后相同；只比较当前容器大小不够。已撤销/删除身份仍需通过快照返回值确认，而不是为了测试直接修改私有计数器。

## 9. 本包探针说明

`probes/id_high_water_probe.cpp` 是独立的缩小模型，仅模拟活记录重建、三个图内游标及live/parked交换。原形状只保留变量游标，对照形状合并三类游标。节点使用两个Pin以简化观察，ID数值不代表实际Fixture的编号。

本轮 `clang++ 17.0.0`、`g++ 14.2.0` 各以 `-std=c++20 -O0/-O2` 运行。四次构建运行都得到：

| 路径 | 原变量-only形状 | 三游标对照 |
|---|---|---|
| Undo后另开分支 | 节点、Pin均复用（示例2→2） | 两者不复用（示例2→3） |
| 删除最高记录后重建候选 | 节点、Pin均复用 | 两者不复用 |

具体编译器版本、完整命令、输出、源SHA256见 `probes/results.json`。脚本退出0只说明预期差异已观察到，**不代表旧实现通过契约，也不代表仓库已有修复**。本探针不测试真实函数签名、插件、SDK、History或GraphTopology极值。

复现入口：`python probes/run_probe.py`。不要把探针纳入引擎验收来替代R04测试。

## 10. 不属于本轮的工作

不进入P05；不实现保存排序、Save As、持久化服务、运行预览、全局插件系统、UI选择状态或新的分配器平台。P04已披露的整图候选成本与现有codec种类不作为本轮新阻塞。

C01继续P09/P12，C03继续P11，C04继续P12。它们保持原FAIL及原探针，新Flow身份缺陷不挂旧编号延期。

不要求为了节点身份改成UUID、修改所有图文件版本，或要求全项目禁止有限ID。正确处理现有NodeId/PinId/变量ID即可。

## 11. 可直接发送的实施指令

> P04主体成果认可，暂不进入P05。本轮只补正Flow混合批次的NodeId/PinId高水位：当前只保留变量游标，候选重建和Undo交换仍可使自动节点/Pin身份重用。
>
> 先在本次验收源码上运行真实Flow反例并保留失败。新增窄的图身份保留方法，在候选执行任何发号意图之前继承已发号状态，并在提交/Undo/Redo的live/parked交换中保持三类身份不回退。按语义替换并删除preserveVariableIdsFrom原方法和调用，不留同义兼容壳。
>
> 保留正常Redo与明确preserve_ids恢复，正确处理耗尽哨兵；不重新编号历史对象，不把计数器搬入第二个Session状态。继续复用唯一History、SessionState、稳定读取和输入清理。
>
> 不改目录、target和包名，不增加新业务框架，不实施P05，不修改main。公共头按现有同步约定更新，重装SDK并运行真实消费者。
>
> 保留原82项用例和断言，新增R04-01～06及共享图回归，最终显式P04门禁、全量构建、二次无工作、完整CTest与原八组消费者。实现与P04-R1验收分别提交，原P04快照不改写，推送后停在P04等待复审。

## 12. 固定来源

下列均为固定提交源码；本报告的失败路径是基于它们的推导。附件中的探针结果单独列出，不混为实际引擎运行。

[S1]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/src/edits/FlowGraphEdit.cpp
[S2]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/modules/function/flowforge/include/lux/engine/flowforge/graph/FlowGraph.hpp
[S3]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/src/FlowSession.cpp
[S4]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/src/FlowSessionData.hpp
[S5]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/src/PreparedFlowReload.hpp
[S6]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/modules/function/graph/src/GraphTopology.cpp
[S7]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/modules/function/flowforge/src/FlowGraph.cpp
[S8]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/modules/function/flowforge/include/lux/engine/flowforge/graph/FlowSource.hpp
[S9]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/test/flow_session.cpp
[S10]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/model/CMakeLists.txt
[S11]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tests/architecture/three_sessions.cpp
[S12]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/editor/tools/flowforge/src/FlowForgeEditor.cpp
[S13]: https://github.com/LUX-YU/lux-engine/blob/ced216040539d022a6564a896210b00a92e6f06e/dev_log/P04/logs/ctest.log

- [混合批次、纯算法及回放][S1]
- [FlowGraph变量高水位][S2]
- [FlowSession][S3] / [所有权结构][S4] / [重载输入][S5]
- [GraphTopology计数器推进][S6] / [FlowGraph移动与原子插入][S7]
- [FlowSource值类型][S8]
- [原Flow行为测试][S9] / [模型CMake][S10] / [三实际会话测试][S11]
- [旧产品复用入口][S12] / [82项CTest归档][S13]
- [C++工作草案：隐式复制/移动的逐成员行为](https://eel.is/c++draft/class.copy.ctor)

**下一阶段仍是原P05；只是先关闭当前作者身份分配缺口，不改变项目路线。**
