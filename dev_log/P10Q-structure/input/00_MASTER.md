# P10Q-structure：Editor 五层收敛施工总指令

**版本：2026-10-01 / Construction 1**  
**对象：负责真实仓库实现与验收的 LLM。**  
**本文件是施工授权范围与执行规则，不是完成证明。**

## 0. 一句话任务

将已存在的新 Editor 能力，按 `editing → authoring → activities → workbench → application` 五种责任重新归属；修正真正的逆向依赖、重复协议和静态／动态绑定错误；保留已验证的行为、代码寿命和资源责任。完成后停在 P10Q 复审，不实施 P11 的新命令／扩展系统，不实施 P12 产品切换。

`A → B` 始终表示 **A 的源码或构建依赖 B 的正式契约**。标题中的五层排列是由内向外的编号顺序，**不是一条 E0 依赖 E1 的箭头链**。实际允许方向是 E4 使用 E3/E2/E1/E0，内层不得反向依赖外层；策略不包含具体后端，具体组合在实例化／装配位置完成。

## 1. 输入、优先级与范围

### 1.1 固定参考，不强制回退

- 仓库：`LUX-YU/lux-engine`。
- 工作分支：`codex/editor-redesign-v4`。
- 本施工文档读取到的远端验收 HEAD：`f7c27f9375cbf8dd8af37b30a6027a460de26213`。
- 对应 P10Q 实现：`3c20910d05d635078ef9021a3a3318086a792b8d`。
- 实施前重新核对真实 HEAD、祖先关系和用户改动；有新提交时从真实后继继续，记录差异。**不得 reset 到本文 SHA。**
- P10Q 原报告固定的实际 lux-cxx 为 `aae62e2fc17ef78ae7be2559a6d58fa0f8297b05`。这是历史依赖事实，不是要求覆盖现有检出；记录实际正在消费的 SDK、头和依赖提交，不降级到早期审阅的版本。

### 1.2 文件优先级

1. 用户最新明确决定：五层收敛、不要重复 engine/modules、优先做减法、Linux 当前不阻塞、不补旧慢算法样本。
2. 本施工包的明确裁定、L0–L6 任务与验收范围。
3. 附带的原五层设计及迁移约束；未被明确裁定的设计语义保持。
4. 现有生产行为和已取得的回归证据。
5. 更早的 core/services/presentation/tools 等目标目录草图，仅为历史背景，不再实施。

对照不一致时，在唯一账本写一条 `decision`，说明来源、具体冲突、如何遵守更高优先级。**禁止悄悄改设计，也禁止明知现状语义不符还照类型名搬文件。**

### 1.3 阶段标识

正式迁移 gate 继续为 `LUX_EDITOR_MIGRATION_STAGE=P10Q`。本轮范围名固定为 `P10Q-structure`，内部批次固定为 `L0`–`L6`。在现有架构规则增加 `editor_layering` 的规则分组和严格/施工覆盖模式，不额外发明 P14、P10R 等业务阶段。

原阶段顺序 `P10 → P10Q → P11 → P12 → P13` 保持。局部 L 批次不是对 P11 的放行。

### 1.4 本轮应完成和不应提前完成

| 必须完成 | 本轮不提前建设 |
|---|---|
| 现有正式新能力的五层归属、CMake 和安装对应 | P11 尚未实现的完整 Command/Extension 系统 |
| TaskMonitor 脱离 UI target；项目纯数据和文件副作用分开 | P12 的完整 SaveAll/Exit/OpenAndShow 产品流程 |
| 三作者模型和 CPU interaction 的独立能力闭包 | 第二个产品 exe 或新的万能 EditorApplication |
| 真实共享算法的 concept 约束、必要动态边界保留一次 | 用模板替换全部 Session/Runtime，或重写引擎框架 |
| 明确旧代码消费者、删除已到期和零消费者残留 | 为凑根目录数量把旧产品全部搬进永久 legacy |
| Windows 实际回归、SDK、实际依赖负例和受影响 GPU | 新 Linux 环境、完整 Android/macOS 矩阵、无关长基线 |

新产品入口的最终唯一切换仍由 P12 完成。此处“新链通过”指正式新模块的真实集成，不代表旧产品已经删除。

## 2. 执行方式：七个内部批次，逐个闭合

| 批次 | 文件 | 应证明的结果 |
|---|---|---|
| L0 | `phases/L0_BASELINE.md` | 当前事实、每文件去向、target 归属、用户改动和测试集合固定 |
| L1 | `phases/L1_EDITING_AUTHORING.md` | E0/E1 独立闭合；不带 UI、Process、运行实例或编译后端 |
| L2 | `phases/L2_ACTIVITIES.md` | 活动无工作台依赖；共享保存、编译、Run 与任务责任不重复 |
| L3 | `phases/L3_WORKBENCH.md` | 通用工作台与领域 UI 分开；CPU interaction 仍不依赖 ImGui |
| L4 | `phases/L4_APPLICATION.md` | 根构建、文档、测试组合明确；旧产品受控但不认证成新层 |
| L5 | `phases/L5_ABSTRACTIONS.md` | 两个真实图工具使用同一受 concept 约束的交付核；动态边界不扩张 |
| L6 | `phases/L6_QUALIFICATION.md` | 同一最终实现 SHA 上的功能、安装、依赖与删除资格 |

每批：读前置 → 更新唯一计划 → 修改一个闭合责任单元 → 定向测试 → 记录 → 提交。后批不能掩盖前批失败。

若用户把本包整体交付为一次任务，允许 L0–L6 按顺序自动执行；只在真实阻塞、计划外大范围行为改变或用户明确要求的检查点停下。若用户只点名一个 L 批次，只执行该批次。**不得以逐文件确认方式把正常实施决策反复交还用户。**

阶段门禁全量只在 L6 执行一次完整最终矩阵；前面按影响范围验证，不为每次 git mv 重跑全部长期计时。

## 3. 五层的硬边界

| 层 | 允许知道 | 不能知道 |
|---|---|---|
| E0 editing | History、Session、代际、ContentStamp、binding/checkpoint、必要纯共享身份、原基础设施 | Scene/Material/Flow 具体源、Pane、文件发布、编译或运行 |
| E1 authoring | E0；对应领域图/schema/正式纯描述/codec；项目与布局值 | TaskScope/ExecutionRuntime、SceneRuntime、ViewHost、GUI、文件后端或编译器 |
| E2 activities | E0/E1 的公开能力；原 Process/Runtime/工具链；本层窄契约 | Root/Pane/ImGui、具体 View、application 私有状态 |
| E3 通用 workbench | 原 UI/渲染/平台公开设施；必要纯身份及布局值 | 具体 SceneSession/MaterialSession/FlowSession；某个工具 UI |
| E3 领域 workbench | 对应 E1/E2 公共能力；通用 workbench | 其他工具 UI、E2 后端私有状态、application 服务定位器 |
| E4 application | 各层正式构造和调用契约；选中的具体组合 | 私下修改其他层的存储、历史和状态机 |

任何 `PRIVATE`、`LINK_ONLY`、模板实例化或生成头带来的边都算依赖。注释“只是测试”不豁免生产 target；集成测试是独立测试节点，允许组合多层，但不能把其依赖倒灌到被测库。

## 4. 开工前禁止动作

- 不改 `main`，不 force push，不 rebase 已发布历史，不执行 `git reset --hard`、`git clean -fdx`。
- 不覆盖 `ProjectBuilder.cpp` 等用户修改；未跟踪文件也不删除。先保存字节、diff 和来源，资格实现使用独立检出。
- 不新建 `EditorExecutor/EditorRuntime/EditorEventBus/EditorRenderer/ServiceLocator`。
- 不把 `Expected/Result/CodeLease/ContentStamp` 各复制一份到新目录。
- 不同时保留旧实现和新实现，再用 option 切换“待删除”；改一个完整调用链并删除原体。
- 不将缺包、缺头或缺工具造成的编译失败当作依赖/概念负例成功。
- 不靠注释掉测试、替换成 Fake、关闭生产功能或删断言获得绿色结果。
- 不将所有共享库全局改 STATIC；进程唯一身份/元信息和插件边界按现有 owner 保持。

## 5. 每个修改单元必须回答的十个问题

1. 它原来是什么事实/资源的 owner？
2. 目标层为何符合真实语义，而非名字？
3. 公开类型是否变化？变化是语义必要还是仅物理迁移？
4. 它使用了哪些实际 engine/modules/lux-cxx 能力？有没有复制？
5. 是否存在可删除的同义转发/缓存/错误翻译？
6. 采用具体类、concept、variant、virtual、signal 的理由分别是什么？
7. 旧文件、旧符号、旧 target/包的退出发生在哪个提交？
8. 哪些消费者已改？哪些旧产品消费者仍暂留？
9. 哪个行为测试证明没有回归？哪个真实负例证明禁边仍有效？
10. 哪个未测范围必须保留，不能写成 PASS？

记录采用 `templates/working-ledger.template.json` 的对应 section，或直接扩展既有同等字段；不建立两份可变事实源。

## 6. 实施验收的关键原则

**语义分层优先于移动数量。** 纯移动不是错误，但若一整批只有路径更名，没有 target 闭包、公共依赖、调用职责或文档的改善，不能报告“架构完成”。

**真实闭包优先于库数量。** 同层可以保留多个 target，跨层不应继续放在同一业务 target。公开逻辑 include 可以不变，但只有一个真实定义；不能创建 forwarding header 伪造旧路径退出。

**行为优先于原测试数字。** 输入报告的 204、178、11 等用于对账，不是新测试配额。保留实际测试名及断言/不变量映射；新增、改名、迁移都要逐项解释。

**有限测试优先于仪式化长测。** 不补旧 50k 长链的剩余 34 次；无性能算法修改时只保留短的复杂度/容量/owner 回归。

**可移植性审查不是 Linux 认证。** Windows 实测；Linux/系统 IME 没运行就保留 NOT_RUN，用户已调整范围的项目不再阻止本轮放行。

## 7. 输出与停止条件

最终交付应包括：正常实现提交、独立证据提交、准确 SHA 链、全量路径/符号/target 对照、实际依赖图、概念实例化表、删除账本、Windows/SDK/受影响 GPU 日志、Linux/IME 范围及用户改动保全证明。

冻结位置统一：`dev_log/P10Q-structure/`。日常材料继续在既有 `.internal/editor-redesign/` 的一个 `layering` 节点，不建立另一套持续变化账本。

全部必测完成才能报告本轮 PASS；未完成列明项和下一入口。**完成后停在 P10Q，不进入 P11；不得把原 P10Q PARTIAL 历史收据原地改成 PASS。**
