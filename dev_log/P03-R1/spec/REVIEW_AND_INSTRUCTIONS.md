# P03 复审 R1：重载拒绝路径的输入所有权与清理作用域

**日期：2026-09-28**  
**项目：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**复审 HEAD：`7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb`**  
**P03 实现：`8486370d13497137529cdd1efb7602608a27d85b`**  
**已验收前置：P02 R1，`e0f440067792dc661e99a4f861a4116eeab4d304`**

## 0. 结论、范围与证据等级

**暂不进入 P04。只补正 P03 私有重载准备的一处输入所有权缺口，完成后停在 P03 复审。**

独立 Material 模型、顺序批次、唯一 History/SessionState、读取 gate、旧算法单向复用和目录收拢是本轮应当保留的成果。不是重做 P03，不是要求增加插件系统、IO 服务或另一套会话框架。

本次发现的同一缺口有两个后果：

1. 重载请求被拒绝之后，输入图节点在读取 gate 外析构；析构回调可以重新进入当前会话编辑。
2. 当传入的 CodeLease 是输入图所需代码的最后一个强拥有者时，拒绝分支依赖两个按值参数的析构顺序；存在代码先释放、节点后析构的允许执行顺序。

证据严格区分如下：

| 等级 | 本次实际完成 | 不代表什么 |
|---|---|---|
| 仓库源码阅读 | 读取固定 HEAD、提交父链、Material 模型/快照/编辑算法/重载/测试、旧调用入口、CMake、图容器与 59 项 CTest 归档 | 不声称逐行审遍全仓或证明所有回归都充分 |
| 源码推导 | 对 `PreparedMaterialReload::prepare` 两个提前返回分支追踪参数、lambda、ReadScope 和节点的生存期 | 完整引擎中的新增负例尚未运行 |
| 独立最小 C++20 探针 | Clang 17 与 GCC 14.2，分别 O0/O2；运行对应控制流和局部 ownership-scope 对照 | 不是 Lux 引擎构建、不是 CTest、没有真实 DLL 卸载 |
| 实施方归档 | CTest 59/59；README 记录七组消费者 8/8 及 P03 门禁 | 不冒称这些是本次独立重跑所得，也未独立复算全套日志哈希 |

本次容器无法解析 raw.githubusercontent.com，因此没有取得完整源码/依赖用于原引擎编译；通过 GitHub 连接器完成上述源码读取。探针源码是独立编写的控制流模型，不是完整引擎源码的代用品。

## 1. 已确认应保留的实现

### 1.1 领域、目录与依赖

`material_model` 是 `editor/tools/material/model/` 下的一个 STATIC target，直接依赖 `edit_sessions`、`edit_history` 和既有 `material_graph`。不应为本轮再增加新 target，不能重建 `editor/history` 或 `editor/sessions`。

继续沿用：

```text
editor/editing/history/       唯一历史实现
editor/editing/sessions/      唯一 SessionState/SessionStore/EditGate
editor/tools/material/model/  Material 作者域
editor/tools/material/src/    按期限暂留的旧产品接线
editor/transition/            原限期私有桥
```

`MaterialSession::apply` 与旧 `MaterialEditor::Impl::edit` 都调用 `prepareMaterialEdit()`，这是实质性的旧→新算法复用。不要因为旧窗口仍持有旧 source/history，就创建第二个会话与其双向同步。

### 1.2 已实现的读取和快照

`MaterialReadView::capture/encode/withRead` 已通过同一 `EditGate::withRead()` 保护其函数体内的 clone、codec、临时对象和异常展开。`MaterialSnapshot` 以外层 CodeLease 容器覆盖图节点，移动赋值也显式维护销毁次序。

本轮问题不在于这些入口完全没有 gate，而在于**重载准备函数的按值输入，不属于内部 lambda 的局部对象**。不要删掉现有有效保护，也不要让快照变回浅层别名。

### 1.3 归档成绩的使用

59 项 CTest 的归档包含原 Scene/P01/R1 回归、新 Material 七个场景及依赖负例。它们是真实的历史交付记录，不因为本次发现了未覆盖路径就改写为未执行。

但 `material_session.cpp::reading()` 当前准备的是带绑定的会话，并保持一个外部强 code owner；它覆盖成功准备后的重载采用、clone 抛异常、非法 clone 与临时析构，不覆盖本轮的未绑定提前拒绝和“输入只有最后一份 lease”组合。

## 2. B01：被拒绝的重载输入仍在 gate 外销毁

### 2.1 精确位置

```text
editor/tools/material/model/src/PreparedMaterialReload.hpp
    PreparedMaterialReload::prepare(MaterialSession&, MaterialSource, CodeLease)

editor/editing/sessions/include/lux/engine/editor/sessions/SessionState.hpp
    EditGate::withRead()

modules/function/material/src/MaterialGraph.cpp
    MaterialGraph::~MaterialGraph() = default
```

当前形状：

```cpp
static MaterialEditResult<PreparedMaterialReload> prepare(
    MaterialSession& session,
    MaterialSource source,
    CodeLease code)
{
    auto& owner = MaterialSessionAccess::data(session);
    return owner.state.gate().withRead([&]() -> MaterialEditResult<PreparedMaterialReload> {
        if (!owner.state.binding())
            return rejected(INVALID_SOURCE);
        // 只有到这里才把 source/code 传给 MaterialSession::create。
        // create 内部有 InputRelease；它不能保护从未进入 create 的分支。
        ...
    });
}
```

`[&]` 仅引用外层参数；并未把 source 的所有权转给 lambda 内部局部对象。

### 2.2 不依赖参数求值顺序的失败路径

1. 创建合法但未绑定文件的 Untitled MaterialSession。
2. 为重载输入图放入一个允许动态特化的 ConstantNode；它的析构回调尝试在该会话执行一次合法 rename。
3. 为这个场景保留外部 code 强引用，先排除卸载次序因素。
4. 调用私有 `PreparedMaterialReload::prepare`。
5. `withRead` 成功进入 READING；`!binding()` 返回 INVALID_SOURCE。
6. `withRead` 的 ReadScope 离开，gate 恢复 AVAILABLE。
7. 外层按值 source 参数才被销毁，其 graph 同步销毁节点。
8. 节点析构中的 rename 此时可以通过编辑 gate；准备结果是失败，但当前内容、历史和 observed 已经被改变。

本轮不要求允许 Untitled reload。**继续拒绝这个请求，但失败清理不得重新开放本来受保护的会话修改入口。**

这也不是调用方非法绕过 gate：负例应通过真实公开 `MaterialSession::apply` 执行 rename，不修改 `Impl` 或私有 source。

### 2.3 第二个后果：代码保活与参数析构顺序

若只把 code 与 source 作为两个彼此独立的按值参数传入，且提前拒绝后 source 未移动，代码释放与图析构的顺序不能由参数书写顺序可靠保证。

C++ 调用参数的初始化之间没有统一的从左到右保证，参数按其构造顺序的逆序销毁；参数具体在被调函数退出或外层完整表达式末尾销毁也由实现决定。两种位置都晚于当前内部 ReadScope 的退出。

本轮探针在 Clang 17 上观察到最后 lease 先于输入节点析构释放；GCC 14.2 的这个调用没有出现该次序，但仍观察到未绑定拒绝后的重入编辑。**不能把某一个编译器未触发卸载次序当作接口正确的依据，也不能声称所有编译器都已复现卸载问题。**

实际插件 DLL 若依赖这份 owner 保活，过早释放还可能影响节点虚析构代码的可执行寿命。本轮只使用弱引用观察这种寿命关系，没有加载或卸载真实 DLL。

### 2.4 为什么仍属于 P03

P03 原指令已经要求重载准备、回调/临时析构、失败原子性和节点代码寿命；不是新增一条“必须防御任意恶意插件”的需求。已有测试本身使用了 clone_hook/destroy_hook 进行同类受控回调验证。

这是新 Material 作者模型内部的问题，不能写入旧 C03 并拖到 P11，也不应拖到 P05 的文件 IO 阶段。

## 3. 本次最小探针的结果与边界

运行入口：`python probes/run_reduced_probe.py`。

| 输入条件 | Clang 17（O0/O2）原形状 | GCC 14.2（O0/O2）原形状 |
|---|---|---|
| 未绑定，外部 code 保持强引用 | 请求拒绝，但 nested_edit=1、author_value=1 | 同样观察到重入修改 |
| 未绑定，参数是最后 lease | 重入修改；节点析构时 weak code 已失效 | 重入修改；该次序下 weak code 仍有效 |
| gate 已为 CLOSING，参数是最后 lease | 请求拒绝，修改受阻；节点析构时 weak code 已失效 | 请求拒绝，修改受阻；该次序下 weak code 仍有效 |
| 局部拥有型输入 + 已准入 lambda 内消费的示例 | 三项都不出现修改或提前释放 | 三项都不出现修改或提前释放 |

源码、编译器版本、构建命令、输出及逐项 `safe_under_target_contract` 在 `probes/`。

脚本退出 0 仅表示探针完成并观察到预期差异；**不表示原形状通过目标契约，更不表示仓库补正已完成。** 正向 ownership-scope 是说明性对照，不是已提交补丁。

## 4. 要怎样修改，及不应怎样修改

### 4.1 需要改变的唯一责任关系

建立一个**拥有型的重载输入清理单元**：它持有该输入需要的 code 和 source，确保节点先析构，code 后释放。可采用本文件局部类型，或仓库中确有相同语义的既有类型；不要求安装 SDK 新增公共类型。

在获得 READING 后，把这个单元移动为 lambda 内部局部对象；**必须先完成这个移动，再判断 binding、身份、预算或其他可能返回的条件。**

这样：

- 成功准入但校验拒绝：局部图在 READING 内销毁，随后 code 释放，最后 ReadScope 退出。
- 准入本身因 BUSY/CLOSING 拒绝：外部拥有单元仍负责安全的 code/source 析构次序；不得擅自改变已有 gate 状态。
- 正常准备：create 消费源；返回完整候选，保持现有 loaded 基线、History 和来源校验。
- 普通异常：所有尚未移交的节点仍在正确 code 寿命内清理；已有读取 gate 通过 RAII 恢复。

### 4.2 实现形状参考（不是必须新增公开类）

```cpp
// 实际类型和命名空间按现有代码补齐。
struct ReloadInput {
    CodeLease code;       // 先声明，后析构
    MaterialSource source;
};

ReloadInput input{std::move(code), std::move(source)};
auto& owner = MaterialSessionAccess::data(session);
return owner.state.gate().withRead([&]() -> MaterialEditResult<PreparedMaterialReload> {
    auto admitted_input = std::move(input); // 必须在任何提前返回之前
    if (!owner.state.binding())
        return rejected(EMaterialEditError::INVALID_SOURCE);

    auto candidate = MaterialSession::create(
        owner.state.id(), owner.state.binding(),
        std::move(admitted_input.source), admitted_input.code, owner.limits);
    if (!candidate)
        return unexpected(candidate.error());
    return PreparedMaterialReload{owner.content(), std::move(*candidate)};
});
```

本例不使用该持有器的移动赋值。若实现方将其提升为可复用类型且提供移动赋值，不能默认先释放旧 code 再释放旧 graph；应删除无必要的赋值，或明确安全销毁次序。不要为了修这一点引入全局 SourceEnvelope/OperationManager。

### 4.3 禁止的“修复”

- 仅把参数顺序交换：不能约束全部编译器的参数构造/析构次序。
- 仅把 `withRead` 提前，仍按引用捕获外层 source：节点依旧可能在 gate 外析构。
- 仅在函数最外层加图清理 guard：可能解决 code 次序，但成功准入后提前返回的图清理仍晚于 ReadScope。
- 为失败输入延长全局 code 的寿命、泄漏节点或禁止插件卸载来掩盖问题。
- 把未绑定校验删除，让原应拒绝的请求变成成功。
- 增加 capture_busy/reload_busy，或在析构里临时启动第二套 gate。
- 把节点析构回调从测试中删掉，或仅因同一编译器的 lease 次序恰好安全就宣称通过。
- 把问题转给旧 C03、P05 或 P11；也不要重写已经成立的 History/SessionStore。

## 5. 文件级处置清单

| 文件/对象 | 本轮动作 | 保留/禁止 |
|---|---|---|
| `model/src/PreparedMaterialReload.hpp` | 修改输入拥有与清理作用域；覆盖未绑定、BUSY、无效候选及成功路径 | 保留 prepare/adopt 的身份与 History 校验；不公开 replaceSource/markClean |
| `model/test/material_session.cpp` | 为真实模型增加下面 R03-01～04 行为；可直接复用现有 PluginConstant/hooks/Saved | 不用简化探针代替实际模型；原七场景不减弱 |
| `model/CMakeLists.txt` | 在现有测试 target 注册新增场景 | 不新建 model 库、不为单个测试新增业务模块 |
| `model/src/MaterialSession.cpp` | 核对 create 的 InputRelease 与新输入流程的交接；只有实际需要才改 | 已有 capture/withRead 正常保护不回滚，不删除合法校验 |
| `MaterialSnapshot.hpp`、History、SessionStore、EditGate | 回归验证，原则上无业务改动 | 本轮问题不是 gate 本身错误，不复制或重写它们 |
| 当前 `.internal/editor-redesign/` | 增补本轮缺口、测试映射、输入所有者与最终 SHA | 仍是唯一可变施工账本 |
| `dev_log/P03-R1/` | 新冻结补正记录、修复前/后证据和回归收据 | 原 `dev_log/P03` 的历史成绩/日志/SHA 不改写 |
| 旧 MaterialEditor/UI/预览/编译/IO 和私有桥 | 保留原消费者及 P12 删除期限 | 不顺手修 C01/C03/C04，不实施 P04 |

本轮不需要再次移动目录，不修改 target/package/include 名称。若为真实回归需增加一个同模块测试 CPP，允许，但它不形成新的业务架构层。

## 6. 必测的实际引擎场景

### R03-01：未绑定拒绝，输入析构不得修改当前会话

使用真实 `MaterialSession` 与 `MaterialSource`。会话 SourceBinding 为空，源本身合法；不要通过改私有状态把绑定清空。

为拒绝输入加入既有测试模式的 PluginConstant。外部 code 保留强引用；其析构回调只做一次公开 `session.apply(MaterialRename{...})`，记录结果。

观察：

- prepare 仍明确失败，且原因为未绑定/INVALID_SOURCE，而不是先做一次无关修改。
- 输入动态节点确实被析构，不是以泄漏避免回调。
- 回调内编辑失败；未进入闭合编辑提交。
- 原源完整编码、History current/cursor/entries/revision/charge、observed、dirty、SourceBinding 不变。
- 函数完全返回后，一次正常公开编辑能够成功；没有遗留 READING/BUSY。

先在固定修复前 SHA 运行并保留实际输出；本条直接检验原错误清理顺序。

### R03-02：未绑定拒绝，输入只剩最后一个 CodeLease

创建一个与目标会话、既有 snapshot/history 无关的独立 code owner；输入节点只持弱引用，随后释放加载方/测试外层强引用，使按值传入 lease 成为最后的合法代码 owner。

记录每个输入节点析构时 weak code 是否仍有效、节点总数和最终 code 释放时刻。最终要求：所有节点析构都在 code 有效时完成，调用返回后最后 code 可释放，不泄漏。

不能沿用现有 reading() 中一直保持强引用的 `code` 局部变量来声称覆盖本场景。不同编译器修复前结果可能不同；如实记录，不能要求每个编译器都必须先失败。

### R03-03：gate 拒绝准入时，同样保住输入代码

对一个正常绑定会话持有合法 ClosePermit，或在外层已有 withRead 中调用 prepare。使用与 R03-02 一样的“输入独立最后 lease”。

prepare 返回 BUSY，输入节点安全释放；外层 permit/READING 状态保持，不被本次失败解锁；放弃 permit/退出外层读取后正常操作成功。

本场景主要检验 lambda 未执行时的外层拥有单元，不能只测进入 lambda 后的清理。

### R03-04：已绑定身份失败/clone 异常与成功重载不回归

保留已有 reading() 与 history 换代测试，并对已绑定但输入 AssetId 不匹配的候选补清理观察。clone 普通异常时仍不能产生半候选，源和基线不变。

正常 prepare/adopt 继续保持 SessionId、更新 HistoryId、恢复 loaded checkpoint；过期候选不采用。快照跨替换、晚析构及移动赋值的既有测试继续运行。

### 一般验收要求

原 X03-01～04、混合批次、稳定读取、P01/P02/R1、实际架构负例和七组安装消费者继续保持。原 59 是基线索引，不要求最终固定成某个更漂亮的数字。

最终显式 `LUX_EDITOR_MIGRATION_STAGE=P03`。测试实际消费最终修复 SHA；重新构建、二次无工作、完整 CTest、SDK 安装及受影响安装消费者重新配置/构建/运行。若某项必须环境无法取得，报告 PARTIAL/BLOCKED，不用本包探针成绩替代。

本轮不强制新增完整 Linux/Android 或真实插件 DLL 平台矩阵；已有环境应实际跑真实模型回归。本包双编译器探针仅说明标准语义风险，不升级为整个项目的跨平台验收承诺。

## 7. 最小交付与停点

1. 保留当前工作区与完整提交链，不 reset、不 force-push，不修改 main。
2. 新增真实负例并在修复前源码运行，记录每条场景原结果；不是所有负例都必须原先失败。
3. 提交输入所有权/作用域修正，不搬目录，不增加万能框架或永久兼容层。
4. 从最终实现 SHA 执行完整本轮验收；独立提交 `dev_log/P03-R1`。
5. 归档路径可搬运，缺失证据不跳过，原 P03 成绩保留为历史记录。
6. 报告唯一输入 owner、节点与代码析构次序、失败准入的责任、新测试结果及实际限制。
7. 正常推送现有实施分支，停在 P03 等待复审，不自动进入 P04。

补正通过后下一阶段仍是原 P04 FlowForge 作者模型，不改变依赖顺序，不重做已经完成的 Scene/Material 设计。

## 8. 可直接发送给实施方

> P03 主体设计与交付方向认可，但本轮暂不放行 P04。只执行《P03 复审 R1：重载拒绝路径的输入所有权与清理作用域》。
>
> 修复 `PreparedMaterialReload::prepare` 的按值输入在内部 ReadScope 退出后才销毁的问题：输入的 code/source 必须有一个正确排序的拥有单元；获得 READING 后先把它转为 lambda 内部局部对象，再作所有可能提前返回的验证。准入本身失败时也必须安全清理最后 code lease，不改变已有 gate。
>
> 先通过真实 Material 模型复现未绑定拒绝后的析构回调编辑，以及独立最后 lease 在未绑定/BUSY 分支的次序；记录原实际结果。保留 source、History、observed、dirty 和绑定不变的完整断言。最小控制流探针不能替代真实回归。
>
> 不改 History、SessionStore 或读取机制的职责，不新建平行 busy，不改目录/target/包名，不删除现有测试，也不修旧 C01/C03/C04。本轮不实施 FlowForge。
>
> 最终使用 P03 门禁重跑受影响回归、完整 CTest 和安装消费者，补正实现与验收分别提交，保留原证据，推送后停在 P03。

## 9. 固定源码与规范来源

以下源码链接均固定到本轮审阅提交；文中的行为判断是对已列源码的审阅，探针结果另列，不混同两类证据。

- [P03 验收说明](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/dev_log/P03/README.md)
- [PreparedMaterialReload.hpp](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/src/PreparedMaterialReload.hpp)
- [MaterialSession.cpp](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/src/MaterialSession.cpp)
- [MaterialSnapshot.hpp](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/include/lux/engine/editor/material/MaterialSnapshot.hpp)
- [CodeLease 的外层代码拥有](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/contracts/include/lux/engine/editor/contracts/CodeLease.hpp)
- [EditGate 与 withRead](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/editing/sessions/include/lux/engine/editor/sessions/SessionState.hpp)
- [Material 模型测试](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/test/material_session.cpp)
- [MaterialGraph 容器及析构](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/modules/function/material/src/MaterialGraph.cpp)
- [MaterialGraphEdit 纯编辑](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/src/edits/MaterialGraphEdit.cpp)
- [旧 MaterialEditor 调用适配](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/src/MaterialEditor.cpp)
- [material_model CMake](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/editor/tools/material/model/CMakeLists.txt)
- [59 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/7ead91c5f111c5edfd7d9f156e23e3f00af4a8eb/dev_log/P03/logs/ctest.log)
- [C++ 工作草案 expr.call：参数初始化与销毁顺序](https://eel.is/c++draft/expr.call)

原范围与验收契约来自本会话提供的 V4 `P03_material_authoring.md` 及 `LUX_ENGINE_P03_START_AFTER_P02_R1_2026-09-28.md`。本轮不重写这些历史文件；当前施工说明增加本 R1 即可。
