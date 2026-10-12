# RD3-D0 设计一致性审查

审查对象是原包和基线 `246fe21eb6d060801b9a16ddcadde2be89dbbecc`，不是已实现 RD3。
结论 **PARTIAL**。没有发现必须放弃 Effect/Graph/Domain 核心模型、另造 Manager 才能解决的矛盾；
发现以下确切追踪冲突和仍需裁决的合同缺口。所有修正均为文档建议，不改变原包，也没有生产修复。

## 核心所有权和依赖

| 对象 | 唯一权威、借用和释放 | 审查结论 |
|---|---|---|
| RenderRuntime | 应用拥有 Client；持协议端点。Server/Transport 生命周期由应用装配 | 与现有 Runtime::Impl 独占 Native+Transport 的实现不同，P1 必须实际拆责任 |
| RenderBackend | 单 owner-thread，拥有 Scene/Effect/Graph 引用表/Target/Domain；一次 Native 装配 | 不新增第二帧循环或 EventBus |
| Scene / View | Backend 拥有 Scene；Scene 的 SlotMap 真正拥有 View；全局索引仅存身份 | 不能保留现有 Backend.views 作为第二份可变权威 |
| RenderEffect | Backend 拥有可编辑配置和 revision；Feature 按配置组合 | EffectId 是稳定逻辑身份，删除后 generation 失效，不能等同 GraphId |
| Graph / GraphId | Backend 缓存不可变编译程序及 Native owner lease；同构 E1/E2 可共享 | 设备、结构、ABI、目标相容签名决定去重；实际资源、SceneId、热值不进代码 key |
| Target | Backend 独立拥有中立输出/层次语义；Native backend 拥有 WSI/offscreen backing | 一次 Graph execute 必须包括最终像素；R01 尚需定义混合 Effect 的完整闭包 |
| ResourceDomain | Backend 域表，父子逻辑关系；每域一个同实现 Registry | 域拥有逻辑资源，Native RAII 一份；祖先只能读，跨 Scene 无隐式借用 |
| GraphExecutionContext | Executor 拥有；按真实 Graph/目标/视图组合准备 slots/descriptor/transient/ticket | 不复制 Shader/PSO，不复制 View 业务状态；持久 History 只借 View 域资源 |
| Compiler / Executor | C0–C8 生成中立 IR，C9–C13 降低为 Native code，Executor 实际执行 | 迁移已有算法，不再注册通用 CompilerPassManager |
| Feature / Shader / Schema / Layout | Feature 建图；Shader 算法；Schema 定义数据/访问；Layout 定义完整绑定结构 | Layout 不拥有业务资源；F6.b full shape/ABI 负例保留 |
| Plugin / Retirement | Host 拷贝 ABI 元数据；Graph、回调、资源析构仍需 pin DLL；GPU/WSI 证明后释放 | Effect 逻辑删除或 FrameSerial 均不是 GPU/代码退休证据 |

建议依赖图与第 02 章/LAYOUT 一致：`client -> core/transport/neutral protocol`；
`graph -> core/data/description`；`backend -> neutral scene/effect/target/resource/graph/transport`；
`backend_vulkan -> neutral backend/graph + native foundation/toolchain`。
Native factory/ops 仅在 Server 装配边界；中立模块没有反向 Vulkan 头/链接依赖。
`features -> authoring SDK/data`；Engine/Editor 通过 Client，不能从底层反向依赖产品。
不需要每个目录一座静态库。这里只审目标 DAG，真正 include/link/install 闭包在获授权生产阶段核验。

## R01 — 多 Effect 共用 Target 时，单一完整 Graph 的派生规则未冻结（OPEN）

位置：第 02/06/10 章、15.E07、RD3-D05/D07/D19、NM04/NM09。
各 View 只选择 EffectId；Effect 可编译多个 Target variant；同一 Target 多 View 又必须合成一张完整 Graph。
文档没有明确 View A 选 E1、View B 选 E2 时，这张 Graph 的构建输入、缓存身份及依赖消费者集合。
这是合同缺口，并非证明目标架构不可实现。

最小反例：Target T 的两个有序层分别使用 E1/G1 和 E2/G2。只按 E1 或 E2 构建 G 会丢另一个层；
分别执行 G1/G2 后由 Target 补一个隐形合成 Pass 违反“一次 Graph 完整最终画面”；
更新 E1 若只枚举直接选 E1 的 View，则与 B 共用的完整输出作业/Context 也受影响，却可能不在原子事务里。
源码证据：`runtime/pinclude/RuntimeBackend.hpp::RuntimeTargetLayer` 和
`runtime/pinclude/Presentation.hpp::PresentationLayer` 已表达多层；
`runtime/src/Presentation.cpp::PresentationTarget::present` 目前在 Native Graph 之后 clear/blit/copy，
说明不能只搬类名就满足 RD3 的最终输出合同。完整源路径、Blob 和行锚点在迁移表。

建议：明确 Target 组合描述是完整 Graph 的构建输入，Graph key 包含有序 Effect **结构签名**及层输出合同，
不包含 Scene/View 实例身份或曝光等热值；Backend 从现有引用关系计算所有受影响的完整输出作业闭包。
View 仍只选择 Effect，Target 不拥有第二份可编辑 Effect，组合产物仍属于同一 Graph 缓存。
也可明确同 Target 仅支持一个多 View Effect，但必须证明不削减 I43/W11/Legacy 多层功能；不能默认这样缩限。
关闭条件：用户选定语义后同步 02/06/10/15/19、表格及 P2/P3/P5 工作单；列出双 Effect、多 Target、失败全回滚 oracle。

## R02 — 候选编译期间消费者/Target 变化的发布条件不足（OPEN）

位置：05、06、P5：expected Effect revision + 收集全部 View + 冷态准备 + 安全点发布。
单 owner-thread 避免同时写，不自动排除跨多个循环的候选构建期间发生合法 CONTROL 修改。

最小反例：候选收集 A/B 后，C 绑定同 Effect 或 B 的 Target resize；Effect revision 仍为旧值且 expected 匹配，
候选若直接发布，C 未验证或 B 仍绑定旧 extent。当前 `runtime/src/Frame.cpp::applyRuntimeControl`
和 Runtime 消费循环说明 membership/resize/publication 都在同一现存协议通道范围内，无需新增线程才有此问题。
这是假设采用跨帧候选准备时的设计反例，不声称已复现当前生产 bug。

建议在现有 Backend 事务中二选一并冻结：整个准备/发布期间有界串行化相关控制修改，或记录并在发布点重验
消费者代际/Target epoch/资源代际/Device/CodePin 条件；发生变化即重建候选或拒绝，旧 active 不变。
结合 R01 的派生输出闭包，不新增 GraphRecompileManager；expected Effect revision 单独不足。
关闭条件：create/remove/rebind/resize 插入 prepare 与 publish 之间的确定性正负例，任何失败全部 LastGood。

## R03 — CSV 阶段冲突及不够具体的 Owner（OPEN，追踪建议已逐行给出）

`RequirementsTrace` 的 REQ09/REQ10 写 `RD3-P3`，但 W17/W06、I40/I21 和 P4 工作单才交付
增量发布/资源域资产；REQ11 的身份可以 P1/P2 建立，但完整数据权威消费要 P4。
REQ27–31 写 `RD3-L` 而无具体子阶段；REQ51 写 ALL_RD3_GATES，而 H06/W25 的真实算法在 H6。
`GoldenWorkloads` 和 `RequirementsTrace` 无独立 Owner 字段；章节编号并不等同负责对象。
`D0_TRACE_REVIEW.csv` 给每行明确 Owner、建议阶段、原 oracle 与工作单引用，保留原阶段以供对照。
其中多阶段值表示每阶段的不同义务，不把 W07 真 Skinning 提前宣称 P3 已实现。
关闭条件：批准建议并在下一份候选中统一各表/工作单；ID、原验收文字、证据类型不得减少。

## R04 — 类型表回滚/分类不充分（OPEN，补充清单待审）

原 `TypeOwnership` 有 60 项，每项有内容/Owner/写权/生命周期，但大量 RequiredProof 为通用句，
没有逐项 rollback 和 why-not-value。比如 RenderResult/RenderError 不应机械解释为“都校验 Device/Queue generation”；
GpuBuffer/GpuImage 的 “Backend or domain registry” 要根据资源类别落实为一个实际 Native owner，不能双方各一份。
`D0_TYPE_REVIEW.csv` 补充逐项类型形式理由、首个消费者、失败/回滚和阶段；多数是值、元数据或自由算法，
没有因清单里出现名称就要求新增长期 class。Native backing 与 Context import 状态必须区分：资源 owner 保存真实
accepted last-use/ready，Context 保存执行绑定及该执行的状态证据，不能复制两份独立更新的资源权威。
关闭条件：对类型清单和资源类别的实际唯一 owner 接受明确裁决；P3/P4 用跨 Context 共用 Root 资源负例证明。

## R05 — Client 停止次序的表达需补充安全条件（CLARIFICATION_PROPOSED）

第 02.3 章写“销毁 Transport → Client 失效”。其前提已经是“禁止新提交”，不能据此实现一个仍能解引用
已销毁 Transport 的 Client。现有 `runtime/Runtime.hpp::RuntimeRequest` 明确保留 Runtime 死后 Reply arena 安全；
`transport/test/ReplyLifetime.cpp` 是应继承的真实负例。
建议明确：撤销提交能力先于 Transport 析构，Client 方法返回终止错误；已发 Reply 的 consume/abandon 继续由
原 R2 arena 生存合同保障。无需新增生命周期服务。P1 用 Client/Request 晚析构验证，不能弱化原 R2。

## 21 章和配套材料逐章审查

| 章/材料 | 已审内容与结果/限制 |
|---|---|
| 00 | 目标/基线/非实现声明一致；授权状态以本轮用户指令为准 |
| 01 | 少类型、具体算法、热态成本、成熟引擎对照；不以类数量为目标 |
| 02 | 中立 DAG 和所有权成立；R01/R05 补充 |
| 03 | Data/Pass/CPU/Wire/GPU 分离，不强制 CPU mirror；身份域保留 |
| 04 | 域级逻辑归属/父链只读/Native 退休成立；R04 唯一实际 backing owner 要落实 |
| 05 | 三 Lane/Reply/Admission 保留，整体 Effect 事务方向正确；R02 |
| 06 | Effect owns 配置、无状态 Feature/可选 cold Ops，E1/E2 共享代码合法；R01/R02 |
| 07 | C0–C8/C9–C13 分离，完整 final output、Scope proof、无第二分析器 |
| 08 | 同源 Schema、SPIR-V/full shape/relocation、绑定优化，保留 F6.a/b 算法 |
| 09 | Compiler/immutable Graph/Executor/Context 分离；Queue/WSI/partial-submit 不能伪完成 |
| 10 | GraphId 连续兼容 job 分组保持顺序；多 Effect Target 与 R01 相连 |
| 11 | 外部 SDK/C ABI/CodePin，资源析构代码也 pin；尚未实现 |
| 12 | F36/I44/H6 来源和验收逐项保留；资源-only Legacy Feature 不强行建空 Feature |
| 13 | H1–H6 原算法/质量/fallback 要求保留，SoftSplat 不是 3DGS，4D 仍后续 |
| 14 | C++20/noexcept/expected/fatal OOM、禁止无责任 Manager；R04 补实库存 |
| 15 | 目标伪代码已声明未实现，不当可编译 SDK；E07 存 R01；“N 阶段”按对应 P 阶段解释 |
| 16 | 历史性能不豁免；F5 表列旧时点 +7.9439%，最新 F5-C 仍 PARTIAL +5.5749%，见来源补充 |
| 17 | NM01–18、四类证据和拒绝矩阵齐全；不把机制 oracle 等同业务全量 PASS |
| 18 | 每阶段独立 I/V/STOP、只授权一阶段；“dirty 7 files”是旧环境快照 |
| 19 | D01–26 与十项目标一致；正式设计替换须用户批准，R01/R02 待补 |
| 20 | 全部旧章节/决策组/源码职责有迁移位置；本轮补精确当前文件/符号/Blob |
| LAYOUT | 模块依赖无反向 Native，目录不强制 target；图外旧 compositor 不能只重命名 |
| 7 工作单 | D0/P1–5/L/H 独立边界清楚；P2 CPU 候选与 P5 全链事务是不同完成度；R03 阶段要统一 |
| 4 CSV | 86/25/51/60、主键、源码/章节/Golden 引用机检；逐项追踪在补充表；没有未来功能已完成声明 |
| 合订版/来源/交接 | 21 章均包含，另附文件逐字节相同；Manifest 与 ZIP 独立校验 |

## 正确性与性能不能在迁移中损失

绑定优化仅在同一 command buffer 内依据完整相容性跳过冗余 bind；barrier/alias 优化必须保留
subresource、queue-family、初始化与实际 memory overlap 证明。不同 Scene 可共享代码但不共享可变数据；
同 Scene donor 复用仍要检查输入版本、side effects 和接受的真实 ticket，并延长至最后 consumer。
History 持久资源不变成无 owner transient。热态固定拓扑零首方 heap、不 lazy 创建 PSO；冷态额外成本仍要实测。

F4/F5/F6.b 性能债和本机历史原始样本不可用的限制见 [来源报告](D0_PROVENANCE.md)。
本轮未运行新 GPU/性能实验；上述审查不构成任何生产资格更新。
