# P09 复审与 R1 定向补正：旧布局目录不能合并成一次恢复会话

**日期：2026-09-30**  
**仓库：LUX-YU/lux-engine**  
**分支：`codex/editor-redesign-v4`**  
**本次复审验收 HEAD：`8bfadc34d73713bf453b45d090874bbb5d8dd399`**  
**P09 实现：`38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1`**  
**已验收前置：`41167a9bdff8c192fe990d53aa8dfa132d57b081`**

## 0. 结论与证据边界

**P09 主体实现认可，但暂不放行 P10。只执行本文件规定的 R09-B01 补正，完成后停在 P09 复审。**

发现的问题是：`prepareLegacyMigration()` 用一个目录级 `locators` 表合并所有旧布局的内容定位。相同 PaneId/type 在不同布局文件中保存不同资产时，被当成冲突；该冲突发生在读取 `settings.toml` 的 selected 之前。即使两个文件各自合法、用户已明确选中其中一个，整个迁移仍被拒绝。不同 key 不冲突时，则会把其他未选布局的 locator 合入同一个 RecoveryManifest。

这不是已复现的数据损坏、进程崩溃或错误资产已经被打开。P09 还没有执行活动恢复；当前明确可确认的后果是：合法旧布局目录被过度拒绝，或其恢复候选失去所来自的布局范围。

本次检查了分支及父链、README/FILES、WorkspaceStore、LayoutPlan、WorkspaceCodec、LegacyWorkspaceImporter、WriteCoordinator 和 ProjectArtifactStore、旧 readWorkspace 的范围、实际 workspace/effects 测试及 CTest 汇总。**没有独立构建引擎、运行真实 Lux 迁移器、174 项 CTest、PLAYER、SDK、GPU、编译负例或完整归档检查器；没有逐份复算全部远端归档哈希。**

本包六组 TOML 文件经过 Python `tomllib` 解析及文件内 ID 一致性检查；这只是输入夹具自检，不是 Lux 的真实修复前失败证据，也不是 C++ 编解码资格。容器网络未能取得完整源文件依赖，未建立 C++ 隔离运行。实现方必须运行真实 SDK 负例，并保留原输出。

原 P09、P08 R1 启动文件及历史收据保持冻结；本文件是对“从多个旧布局选出单次恢复候选”的明确化。原 V4 未精确规定这一多文件选择策略，不能将本文提出的所有细则描述成原文逐字要求。

## 1. 本轮认可并必须保留的实现

### 1.1 纯值与纯计划

`DockLayout`、`RecoveryManifest`、`UserPreferences`、`LayoutCatalog` 已分开；`ValidatedLayout` 只经校验产生，`LayoutPlan` 拥有值输入与结果。Planner 不持活动 Root/Pane/Session/provider 回调。精确复用按 ViewRestoreKey＋ViewType 进行，额外视图单列保留。[S3、S5]

`effects.cpp` 使用真实已挂载 Pane、Root、dirty MaterialSession，检查规划前后源编码、身份、绑定、current、observed、dirty 和 UI revision 不变，并保持 Undo/Redo。这是实际行为验证，不是只比较 mock 计数。[S8]

### 1.2 存储结果与错误分类

新文件以稳定 LayoutId 命名；rename 改 label，不再重命名路径再同步 selected。布局发布、偏好写入和目录刷新各自报告事实。读取错误、临时 BUSY 与 NOT_FOUND 被区分；单条目录损坏形成不完整目录和诊断，目录枚举失败不伪装空目录。[S4]

### 1.3 删除进入原协调器

REMOVE/provideRemoval 复用原票据、FIFO、Unknown 与发布 owner。实际删除只由 ProjectArtifactStore 执行。原测试覆盖在途写入后删除、已确认前序、Unknown writer 退休、规范路径别名与外部改动冲突。本轮不要求重新实现删除或写入协调器。[S9、S10、S7]

### 1.4 单记录中断恢复与布局来源保护

稳定 ID 从旧文件名映射，旧文件原字节被保存，continueMigration 逐次重读并最多准入一份文件发布；已经存在且来源一致的新记录保留，marker 在记录验证之后发布。现有单布局测试涵盖了布局先落盘、marker 前中断、用户改名后继续，属于有效证据。[S6、S7]

### 1.5 目录与兼容范围

布局和恢复属于一个纯模型构建边界，另有 workspace_store；ViewInfo 的唯一定义移入轻量 contracts，公开 include 不变，旧定义文件已删除。旧 Workspace 业务仍限原 app/menu/SettingPane，最迟 P12 删除。此次 R1 不搬目录、不改包名、不新增业务库。[S2、S3]

## 2. R09-B01：恢复来源在错误的作用域去重

### 2.1 精确位置

主修改文件：

```text
editor/workspace/storage/src/LegacyWorkspaceImporter.cpp
    WorkspaceStore::prepareLegacyMigration()
```

当前结构的关键顺序：

```cpp
std::map<std::pair<std::string, std::string>, std::string> locators;
for (const auto& file : files) {
    // 解析当前旧布局文件……
    const auto key = std::pair{std::string(slot.restore_key.name()), *type};
    const auto [found, inserted] = locators.emplace(key, locator);
    if (!inserted && found->second != locator)
        return failed(EWorkspaceError::CONFLICT, "legacy recovery binding");
    if (inserted)
        migration.recovery.entries.push_back(...);
}
// 以上已经可能返回冲突，之后才读取旧设置。
auto settings = read(".lux/editor/settings.toml");
```

表位于文件循环外，key 没有布局来源；两个不同旧快照被当成一个活动视图集合。[S6]

### 2.2 为什么输入不是伪造的冲突

旧 `readWorkspace()` 先根据传入名称或 settings.selected 确定一个文件，再验证该文件的 panes。重复 PaneId 的检查仅在这个文件的 `data.panes` 内进行，不要求所有 `.toml` 之间的同名窗口绑定相同资产。[S11]

因此，以下两个历史记录可以同时合法存在：

| 文件 | PaneId | type | payload |
|---|---|---|---|
| Alpha.toml | material-1 | lux.editor.material.v1 | v1:12345678-1234-1234-1234-123456789abc |
| Beta.toml | material-1 | lux.editor.material.v1 | v1:12345678-1234-1234-1234-123456789abd |

settings 明确 `selected = 'Alpha'`。它们表示同一类窗口在两个可选快照中曾经打开不同内容，不表示同一时刻的两个活动绑定相互矛盾。

当前实现先读 Alpha 再读 Beta；第二个 locator 进入同一全局 key 时返回 CONFLICT。settings 尚未读取，选中 Alpha 或 Beta 都不能改变结局。甚至还没返回 LegacyMigration，continueMigration 无法准入第一份新文件。

这是静态控制流结论。真实 SDK 的退出码、实际日志和文件不变性须由实现方产生。

### 2.3 同一根因的另一种表现

把 Beta 的 PaneId 改为 `material-2` 后，不再冲突，但会产生两项 recovery.entries，尽管 selected 仍为 Alpha。当前函数没有之后的筛选步骤。

P09 不实际打开资产，因此不能声称已经错误打开 material-2。问题是返回的单次 RecoveryManifest 已将两个独立旧快照合并，而且条目本身没有表达这种来源范围。

### 2.4 与现有 collision 测试不同

原 collision 测试是新目标文件占据稳定 LayoutId，却没有匹配的 legacy_origin，应该继续报 CONFLICT。

本问题是两个合法旧布局使用同一 restore key，但 locator 不同。不要为了通过新测试删除新目标来源冲突检查，也不要把所有 CONFLICT 都降级成成功。

原 migration/collision 夹具均只创建 `Beginner.toml` 一个布局文件，因而不能覆盖跨文件范围。[S7]

## 3. R1 采用的语义决议

本轮将选择规则写明，避免实施者自行“选第一个”或增加新的永久状态机。

### 3.1 两种工作分别处理

1. **布局目录迁移**：每个合法旧布局都转换成一个稳定 LayoutId 的新布局，保留其原字节和来源；是否被选中不影响它被迁移。
2. **单次恢复候选**：基于旧 selected 指向的那一份快照形成 RecoveryManifest。不能把目录中所有布局的内容绑定并集当成一次恢复会话。

这一选择与旧读取入口的选中文件范围一致。它不是在 P09 执行恢复，也不改变 P12 决定是否恢复、怎样打开、如何处理当前 dirty 会话的责任。

### 3.2 未选布局仍应无损保留

其他布局仍保留稳定 ID、几何、视图配置、未知 provider/type/schema、legacy_origin 和原始文件 envelope。其原 locator 已存在于保留的原字节中，不能因为没有加入本次 recovery 就将原始信息删除。

可给出“非选布局内容定位仅保留，未加入本次自动恢复”的诊断。不新增 live SessionId，不把 locator 又塞回新布局的业务视图状态以便未来 restore 顺手打开资产。

### 3.3 selected 各状态的明确行为

| 条件 | 布局迁移 | 单次恢复 |
|---|---|---|
| selected 指向现存合法布局 | 转换全部合法布局 | 仅从选中布局提取已识别 locator |
| selected 为空 | 转换全部合法布局 | 空清单并有可解释诊断；不擅自挑第一个 |
| settings 确認不存在 | 转换全部合法布局 | 无选中快照，空清单；不把“不存在”与 IO 错误混同 |
| selected 指向不存在的文件 | 转换其余合法布局 | 空清单并诊断；偏好按既定缺失 ID 回退方式处理，不创造随机默认目标 |
| settings 权限/IO/BUSY/版本变化 | 保留输入；请求报告错误 | 不猜 selected，不成功发布默认值 |
| 一个旧布局本身重复窗口身份或引用无效 | 保留原错误处理 | 不用本轮的作用域修正放过文件内部非法数据 |

是否为“空 selected”输出诊断可以使用现有 diagnostics；不必增加公用错误枚举。对应错误区分须保持明确。

### 3.4 禁止的近似修法

- 不删除报错之后简单保留首个 locator；这会让字典序决定恢复资产。
- 不以最后读入者覆盖前者；改变文件排序不应改变 selected 对应的恢复。
- 不把 LayoutId 拼进 ViewRestoreKey，使同一逻辑视图跨布局失去复用身份。
- 不要求用户删除其他合法旧布局再迁移。
- 不随机为同一旧布局生成新 LayoutId。
- 不新增“一个万能 WorkspaceData”，将布局/恢复/偏好/结果再次合并。
- 不在迁移器中调用旧 PaneManager、WorkspaceRequest、provider.restore 或资产打开函数。

## 4. 类型、文件与删除清单

### 4.1 优先实现形状

优先先读并验证旧 settings，得到选中的旧文件身份，再逐个文件处理。旧 settings 原字节和输入摘要继续保留。另一种等价实现是先按文件保留私有恢复候选，最后按明确 selected 选择；不要维护目录级无来源 locator 表。

每份布局内部的 locator/身份校验可使用原局部容器。不要求新增公开类型；如确实需要临时 `LegacyLayoutCandidate`，仅放在当前 CPP 匿名命名空间，并限定为解析阶段的值，非可变运行期服务。

### 4.2 精确处置

| 文件/成员/逻辑 | R1 动作 |
|---|---|
| LegacyWorkspaceImporter.cpp 中目录级 `locators` | 删除其“所有文件共用且无布局来源”的用法；改为单布局范围或明确的 per-layout 候选 |
| panes 循环直接无条件写 `migration.recovery.entries` | 删除跨文件无条件合并；只写选定候选的已验证条目 |
| `legacy recovery binding` 冲突分支 | 不再因不同布局的合法差异触发；文件内部/目标来源真实冲突仍保留 |
| 文件循环之后才解析 selected 的顺序 | 重排解析或延后候选选择，保证 selected 参与恢复来源判断 |
| legacyId / 几何解析 / 字节保留 / source_digest | 保留算法和稳定 ID；需要重排摘要生成时，固定规范化顺序，不能因遍历顺序改变同一输入摘要 |
| continueMigration / marker / WriteCoordinator | 原幂等和发布协议保留；仅在选择元信息交接确有必要时做小调整 |
| WorkspaceStore.hpp 的公开 LegacyMigration | 优先保持现有布局；不要仅为遍历方便增加重复可写源；必要变更需说明真实消费者 |
| editor/tests/workspace/workspace.cpp | 添加多布局真实测试与原单文件对照；不删原体和断言 |
| editor/tests/workspace/CMakeLists.txt、原 workspace SDK 组 | 登记实际新增用例，SDK 仍使用安装头/库，复用当前测试机制 |
| editor/workspace/README.md、当前施工账本 | 记录选中快照恢复规则及保留策略 |
| dev_log/P09/* | 不修改，保留原历史结果和原单文件覆盖范围 |

禁止新增业务 target、DLL、队列、迁移 Manager、平行 writer、History、Session 或 Root owner。无须触碰三类模型、UI 交互、编译预览及 Runtime。

### 4.3 完成标记与已有新文件

所有文件仍通过原协调器，既有且来源一致的新记录和用户修改继续保留；不得通过删 marker、覆盖 recovery 或重建布局来“重新跑一遍”。

本轮新恢复选择必须写入可复核说明。已经完成的旧迁移标记不是静默覆盖已有新恢复清单的授权；如真实交付需要区分先前生成策略，应以独立、显式、可诊断的迁移决定处理，而不是在 R1 中顺手覆盖用户值。不要仅为测试方便增加磁盘 schema 版本。新增负例至少在尚未写完成 marker 的真实项目目录中验证。

## 5. 真实测试要求 R09-R1-01～06

本包 fixture 可复制到测试专用临时项目。不得写用户真实 `.lux` 目录。每项先确认输入两份文件均可由旧规则分别读取，记录其完整 SHA256；不要只检查函数退出码。

### R09-R1-01：同 key、不同资产，选中 Alpha

- 输入两个合法 `.toml`，PaneId/type 相同，payload 分别为资产 A、B；selected=Alpha。
- 修复前记录 prepare 的具体错误，预期当前代码命中 `legacy recovery binding`。不要把未来预期直接写成已经取得的失败记录。
- 修复后两份布局均返回并保持不同稳定 LayoutId；同一个 ViewRestoreKey/type 不被随意改名。
- recovery 只有 A；Beta 原 payload 仍可从保留原字节恢复。
- 准备阶段不写磁盘、不创建 Root/Session、不发布票据；正式 continue 后才落盘。

### R09-R1-02：selected=Beta，与枚举顺序无关

- 使用独立测试根，selected 改为 Beta；结果应为 B，而非第一份文件的 A。
- 改变创建顺序／文件枚举顺序时，选择结果不变；布局 ID 仍由原文件名确定。
- 不在完成标记后修改旧输入再强行要求继续成功；那仍是应报告的输入变化。

### R09-R1-03：不同 key 的未选窗口不能混入恢复

- Alpha 保存 material-1/A，Beta 保存 material-2/B，selected=Alpha。
- 当前代码会形成两项 recovery；先保存真实输出。
- 修复后只输出 material-1/A，但两份布局及 Beta 原字节仍完整。
- 与纯 LayoutPlanner 的 extras 保留政策区分：现存额外视图保留，不等于从历史上所有未选布局自动恢复额外视图。

### R09-R1-04：空／缺失选择和错误分类

- 空 selected、缺少 settings、selected 指向不存在文件：不自动选第一个、不将所有布局合并；输出明确诊断和约定恢复值。
- 真正的 settings 访问失败、坏 schema、IO 变化继续报告错误，不发布默认偏好。
- 内部重复 PaneId、坏 dock、新文件 legacy_origin 冲突仍被拒绝。不同文件合法差异不能混入此类拒绝。

### R09-R1-05：两份布局的中断恢复和用户修改

- 正式发布第一份布局后、marker 前，销毁 Store 并重建。
- 用户对已迁移布局执行合法 rename，恢复过程不得覆盖其 label。
- 重试继续完成另一布局、选中恢复、偏好和 marker；没有新随机 ID，没有第二份同名逻辑记录。
- 全部旧文件原字节保持；新的恢复仍来自选定快照，不因中断位置或目录顺序改变。
- 已存在但来源不匹配的目标继续冲突；同源用户修改按原语义保留。

### R09-R1-06：正向单布局、同 locator 以及原工程回归

- 原 Beginner 单文件迁移/collision 断言保持；两文件同 key 同 locator 的正向对照通过。
- byte/schema/opaque/预算、同目标 write/remove、Unknown/冲突、缺文件/权限/目录完整性、纯 effects 测试不退化。
- 显式 P09，全量构建、二次无工作、完整 CTest；SDK 重装，原十三组及新增 workspace 场景执行。
- PLAYER 11 项、八项 operation 编译负例、两个显式 GPU 模式和依赖负例按现有资格运行；不以目录名称代替 CONSUMER_MODE/test command。

测试数量可以增加，不用固定新的“应有总数”倒推验证。每个场景必须写出观察点、命令、退出码、实际实现 SHA。原 174 项行为及断言不因补正而被移除。编译负例只把预期禁用原因计作拒绝。

## 6. 包内材料与真实运行关系

```text
REVIEW_AND_INSTRUCTIONS.md
fixtures/01_... 至 06_.../
    .lux/editor/layouts/*.toml
    .lux/editor/settings.toml
    EXPECTATIONS.json
tests/multi_layout_scope.cpp.txt
scripts/validate_fixtures.py
evidence/fixture_sanity.json
evidence/review_scope.json
```

`multi_layout_scope.cpp.txt` 是插入原 Fixture 环境的测试草图，不是已编译测试，更不是生产补丁。实现方应沿现有辅助函数和错误类型适配，修复前运行当前代码，修复后保持同一业务断言。不要复制本包测试草图后未运行就宣布 PASS。

本地 Python 校验仅确认 TOML 语法、每个文件的基础字段、每文件 PaneId 唯一、expected 配置和哈希。它不执行 legacy INI 转换、Lux ValidatedLayout、实际 publication/SDK 或幂等恢复。

## 7. 交付与禁止扩张

唯一可变施工材料仍为 `.internal/editor-redesign/`；新验收冻结于 `dev_log/P09-R1/`。保留原 P09 报告、历史失败和规范文件，独立提交实现与验收。验收提交指向已经测试的实现 SHA，不使用自身 SHA 形成自指。

保护 `ProjectBuilder.cpp` 既有差异，不 reset、不纳入本轮无关修改，不修改 main。C01 完整旧 ApplyLayout 仍归 P09/P12，C03 归 P11，C04 归 P12；原判定和证据不变。新迁移问题独立补正，不挂 C01 延期。完整新产品 GPU/IME/布局应用继续归 P10/P13/P12，本轮不提前宣称。

**必须环境缺失、必测未跑或仍拒绝上述合法多布局输入，只能 PARTIAL/BLOCKED，不能进入 P10。** 这不要求无限扩展迁移范围，原文件大小、节点类型和支持格式边界继续有效。

## 8. 可直接交给实施方

> P09 主体认可，暂不进入 P10。仅执行 P09 R1：prepareLegacyMigration 把独立旧布局的 locator 合并成一次恢复清单，造成同 key 不同资产的假冲突。
>
> 先用真实 SDK 的两份合法旧布局复现并保存错误，同时测不同 key 未选窗口被合入恢复的输出。保持原旧格式字段，不改消费者类型名来规避。
>
> 所有合法布局仍完整迁移；单次 RecoveryManifest 只取明确 selected 对应的快照。非选布局的 locator 与未知数据保留在原字节中。空/缺失选择诊断且不随意选第一个；IO/BUSY 不解释为不存在。不要更改 ViewRestoreKey 逃避冲突。
>
> 修改限 importer、必要值交接/说明和原测试/SDK 登记。保留稳定 LayoutId、legacy_origin、输入摘要、同源用户修改和 marker 幂等协议；真正的目标来源冲突仍拒绝。不得复制写协调器、改 UI、重写模型或增加通用管理器。
>
> 执行 R09-R1-01～06 和原 P09/较早行为，真实 IO、SDK、依赖、八项编译负例及两项显式 GPU 模式，最终 P09 门禁。原快照和 ProjectBuilder.cpp 保护。实现与 dev_log/P09-R1 独立提交，正常推送后停在 P09，等待复审。

## 9. 固定源码与证据来源

所有链接均固定在本次验收 HEAD。以下源码链接支持“当前实现是什么”；第 3 节的具体恢复选择策略是本轮设计决议。

- [S1 分支验收](https://github.com/LUX-YU/lux-engine/commit/8bfadc34d73713bf453b45d090874bbb5d8dd399)；[实现](https://github.com/LUX-YU/lux-engine/commit/38c88aeaf14ca27687d1baf844b4cc2d28cfcfe1)
- [S2 文件清单](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/dev_log/P09/FILES.md)
- [S3 P09 验收报告](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/dev_log/P09/README.md)
- [S4 WorkspaceStore.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/workspace/storage/src/WorkspaceStore.cpp)
- [S5 LayoutPlan.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/workspace/layout/src/LayoutPlan.cpp)
- [S6 LegacyWorkspaceImporter.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/workspace/storage/src/LegacyWorkspaceImporter.cpp)
- [S7 workspace.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/tests/workspace/workspace.cpp)
- [S8 effects.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/tests/workspace/effects.cpp)
- [S9 WriteCoordinator.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/persistence/src/WriteCoordinator.cpp)
- [S10 ProjectArtifactStore.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/adapters/project_io/src/ProjectArtifactStore.cpp)
- [S11 旧 EditorWorkspaceStorage.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/app/src/EditorWorkspaceStorage.cpp)
- [S12 完整 CTest 日志](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/dev_log/P09/logs/ctest.log)
- [S13 WorkspaceCodec.cpp](https://github.com/LUX-YU/lux-engine/blob/8bfadc34d73713bf453b45d090874bbb5d8dd399/editor/workspace/layout/src/WorkspaceCodec.cpp)
