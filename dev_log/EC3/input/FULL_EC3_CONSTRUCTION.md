# EC3 完整施工总册

应用生命周期与共享活动、低开销声明、运行身份、可扩展设置、相机边界与 inja 生成统一。

本文件由分册按施工依赖顺序合并；与分册内容一致。以 00 总指令的裁定处理旧调查方案冲突。

## 分册导航

- [EC3：应用边界、声明式注册、设置与生成链收敛——总指令](00_MASTER.md)
- [EC3 逐项问题与处置登记（输入清单）](01_ISSUE_REGISTER.md)
- [EC3-A：身份、模块声明与单份描述存储](02_IDENTITIES_AND_DECLARATIONS.md)
- [EC3-B：准确的业务 owner、中性命令路径与生命周期 Application](03_APPLICATION_AND_COMMANDS.md)
- [EC3-C：可扩展设置、窗口环境与生效链](04_SETTINGS_AND_STARTUP.md)
- [EC3-D：相机、ECS 与过度封装核查](05_CAMERA_AND_ABSTRACTION.md)
- [EC3-E：Inspector 生产生成统一到 MetaUnit／inja](06_CODEGEN_UNIFICATION.md)
- [EC3-F：AGENTS、类型复用、复杂度与可读性](07_QUALITY_AND_MEASUREMENT.md)
- [EC3 依赖顺序、文件处置与交接](08_EXECUTION_SEQUENCE.md)
- [EC3 验收主题 V01–V60](09_VALIDATION.md)
- [来源、范围与设计裁定](10_SOURCES_AND_DECISIONS.md)

---

<!-- SOURCE_CHAPTER: 00_MASTER.md -->

# EC3：应用边界、声明式注册、设置与生成链收敛——总指令

日期：2026-10-03。文件性质：下一阶段施工规范，不是实现或验收报告。

## 0. 执行目标

以长期维护为目标，一次完成最近几轮讨论中仍未关闭的应用边界、命令声明与身份、设置／窗口策略、相机过度封装核查、Inspector 生成统一。不是再调整五层目录，也不是重做已完成的 EC1／EC2。

最终必须能回答：

- 加载一个模块时，是否直接注册它自己声明的命令、视图与设置，而不是修改 Application 的字符串分派？
- 内置元数据是否复用其静态存储，动态元数据是否只有一份有明确寿命的存储？
- 已解析的命令是否使用数值身份或强 handle，不再逐条扫描名称？
- 不创建 Application／Root，是否能完成项目内容保存及项目登记？
- Editor 的有效窗口配置是否经过设置与显示环境解析，而不是每个构造点自行选择数字？
- 相机是否仍由原 ECS 组件被原渲染阶段抽取，且没有为了包装而包装的中间 owner？
- Inspector 是否真正由共同解析／语义处理／inja 生成，而不是换语言继续拼 C++？

## 1. 基线与不可改写的事实

本次在线确认的 lux-engine 分支：`codex/editor-redesign-v4`。

源码基线：`126f1b4df14316df30957208787ded9be9f9f461`。实施从实际后继开始；如有新提交，先记录差异，不 reset 到本文 SHA。

EC2 R1 已经限定范围通过；EC2 整体仍为 PARTIAL，原源码／安装 SDK 原生输入为 `NOT_RUN_USER_DEFERRED`。本阶段不把该项目自动补跑，也不改写成 PASS。P12 的 `PARTIAL_USER_WAIVER`、Linux／IME／sanitizer 的未测、旧 66/100 性能记录按原来源保留。

本文是新增 EC3 授权范围的技术施工说明。完成后停在 EC3 复审；不自动合并 main、删分支或发布。分支不是永久保留要求：合并与删除另按用户授权处理。

保护 ProjectBuilder 用户补丁及其所在原工作区。不得以“工作树干净”掩盖用户补丁尚未应用的事实，不得把补丁静默纳入本阶段。

只维护 `.internal/editor-redesign/` 中原来的唯一施工账本；阶段结束的证据冻结在 `dev_log/EC3/`。本包的 CSV 是输入种子与核对清单，不是第二个长期状态数据库。

## 2. 来源优先级与术语

优先级：用户本轮明确要求 → 本总指令的裁定 → 分册的实施细则 → 当前 AGENTS 与持续质量规则 → 原始调查参考。

实际安全性、寿命和文件事实不能被“更简短”覆盖。来源未支持的结论必须标明为设计目标或待核查，不能编造已有 API。

`reference/` 保存原调查和 AGENTS 原字节。旧报告是历史；不得把其中的 P10、P13、旧 Linux 必测或旧类型名直接当作当前要求。

当前已确认的声明清单口径：13 个生产文件，36 条固定命令定义路径，35 个唯一命令 ID，12 个 ViewFactory 描述，3 个作者／源格式描述组，共 50 个固定元数据条目。另有动态 `lux.editor.tool/<ViewTypeId>`。不是全仓穷尽统计，也不是 50 个功能缺陷。[S01]

## 3. 本轮四项核心裁定

### D01：数值化派发，而不是盲目删掉所有字符串

固定名字在编译期用 lux-cxx 计算身份；动态插件名字在加载／注册边界计算、验证一次。已解析的正常 query／execute／菜单动作使用原 handle 或数值索引，不再按业务名称逐项比较。

哈希不是无碰撞证明。注册／外部文本导入边界必须验证规范名与哈希并检测碰撞；不同规范名同哈希明确拒绝，不能当成同一命令。文件路径、用户文本、搜索、配置解码和诊断显示不在“禁止字符串”范围。

禁止全局修改 `StableNameId::operator==` 为仅比 hash。底层的通用精确身份契约继续保留；热点优化在已经验证过的局部目录和 handle 上完成。

### D02：同一种描述类型，不采纳 Spec → 拥有型 Descriptor 的双份方案

撤销原调查中“CommandSpec 物化为原拥有型 CommandDescriptor”作为最终方案的建议。

原 `CommandDescriptor` 原位改为轻量、可 constexpr 声明的描述视图：复用现有预哈希名称视图、`string_view`、scope、版本和 TypeToken。固定模块描述由 Entry 引用；动态描述由准确的 backing owner 保活。同一逻辑条目不同时永久保存 Spec、拥有型 Descriptor 和第二张字符串表。

只允许一个描述 schema。存储 owner 是寿命责任，不是另一套命令接口。不要为了零拷贝把临时字符串或插件已卸载内存借入目录。

### D03：按真实功能模块声明；显式加载注册

命令／视图／作者格式由实际功能 provider 声明。Application 只选择产品所需贡献并安装。

不创建 `AllEditorCommands.hpp`、全局业务枚举、全局 service locator、静态构造自动注册或跨 DLL 的地址身份。需要全局清单时，从模块声明汇总生成，不让所有模块互相 include。

### D04：相机按职责核查，不因调用层数机械合并

原 Camera、Transform3D、WorldTransform3D、CameraExtraction、RenderViewRequest、资源输出与退休继续复用。CameraMotion 是输入；原 CameraPose 是原组件的值组合，不是第二个渲染相机。

必须调查并删掉真实无责任的转发／重复计算；但 View、UI Element、资源呈现 owner 和 ECS→渲染抽取各有不同责任，不按“超过几层”裁剪。保留跨回调／跨帧失效后的检查。

## 4. 完整范围

| 工作流 | 必须完成的收敛 | 不要求重做 |
|---|---|---|
| A：命令与元数据 | 模块声明、哈希索引、同型无文本复制、动态寿命、默认快捷键与设置覆盖、消除字符串二次分派 | 原命令注册／派发算法、目标捕获、PINNED／CURRENT_REGISTRATION、批次保护 |
| B：Application | 迁出项目保存、工作区政策、项目插件选择／最近项目和结果特判；公开入口归准确能力 | 原 History、SessionStore、Process、Runtime、ViewHost |
| C：设置与启动 | 作用域、文档、动态设置贡献、草稿／有效值／持久化状态；窗口／字体／快捷键与插件设置实际接通 | 任意设置都即时生效、任意插件热卸载、场景 Feature 迁到全局偏好 |
| D：相机与封装 | 保留 ECS 路径、命名准确、唯一写入者、显示／拾取一致性、必要与冗余边界清单 | 新 CameraManager、GPU picking 系统、全历史帧 Registry |
| E：代码生成 | Inspector 语义迁移、inja 显式投影、正确输出依赖、SDK 工具安装、非法输入与真实编辑回归 | 第二个 C++ parser、统一所有脚本语言、重写 Render／ScriptAbility |
| F：质量与成本 | AGENTS、实际类型尺寸／分配／名称比较计数、构建闭包、真实新功能验证 | 旧慢算法补满、Linux 环境建设、重复全仓首次冷构建声明 |

## 5. 执行顺序

按 C0–C9：基线 → 身份与单份元数据 → 项目保存／工作区 owner → 模块贡献与中性命令入口 → 设置核心 → 显示环境及正式设置页面 → 相机收敛 → inja 生成 → 综合与删除 → 封存。

各批次之间提交可构建的闭包。不能先删除公共入口，把数百个消费者留给“最后统一修”。不能在一个批次只交接口而没有真实提供者和迁移消费者。

每个批次结束更新同一账本：已迁算法、唯一 owner、删除符号、保留理由、真实验证、未测和下一入口。未完成也必须诚实交付已有代码，不自动扩张范围。

## 6. 完成判据

功能等价与输入／输出关系优先于文件数、类数和测试数。测试主题不是测试程序配额。

必须删掉旧字符串分派体、旧 Application 业务体、旧 Python Inspector 生产发射体及失效安装入口。不能留下 `legacy`、alias、forwarding header 或 old/new 回落；旧文件格式的只读兼容与历史证据必须保留。

任何优化声明都应指明路径与计量：元数据文本不复制，不等于零 Entry 分配；热派发不比较业务字符串，不等于文件 IO 不解析路径；减少转发，不等于 GPU 输出可以不保活。

## 7. 交给实施 LLM 的短指令

> 只执行 EC3，先读总册与逐项清单。继承 EC1／EC2 已通过契约，按 C0–C9 完成长期收敛。固定描述直接复用同一 CommandDescriptor；注册时验证哈希碰撞，正常派发只用数值身份或原 handle；模块自己声明并在加载时注册。迁出 Application 的真实业务政策，建立可扩展设置与显示环境解析，保留原 ECS 相机和资源寿命，统一 Inspector 到既有 MetaUnit／inja 管线。用真实无 App 消费者、插件、设置持久化、命令和生成结果验证，不用包装或更名代替迁移。保护用户补丁、main、历史与免验，分别提交实现和验收，停在 EC3 复审。

---

<!-- SOURCE_CHAPTER: 01_ISSUE_REGISTER.md -->

# EC3 逐项问题与处置登记（输入清单）

这是本次讨论的逐项追踪表，不是现有实现完成表。所有状态初始为 OPEN_INPUT。

“问题”同时包括已确认代码位置、用户新增设计要求、必须保持的既有契约和待验证风险；不能把每行都描述成已发生的 bug。原 50 项声明另外逐条映射。

## BAS：基线与已完成范围

细则：[00_MASTER.md](00_MASTER.md)。批次：C0；验收主题：V01 V02。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| BAS-01 | 在线源码与实际工作区可能不同 | 固定源码/依赖/工作区；新后继先核对，不 reset |
| BAS-02 | 历史通过、PARTIAL、免验、延期不能混写 | 分别继承 EC1/EC2、P12 与原性能状态 |
| BAS-03 | 用户 ProjectBuilder 补丁未必在新工作区 | 保存原字节，报告是否应用，不自动纳入 |
| BAS-04 | 五层已完成不等于所有职责都正确 | 保留目录，按实际算法和状态裁定 |
| BAS-05 | 旧文档的期限/类型不能变当前要求 | reference 冻结，本文裁定优先，不重做旧阶段 |
| BAS-06 | 现有 50 项只是人工声明范围 | 保留统计口径，C0 扩展生产消费者，不宣称全仓穷尽 |
| BAS-07 | 多个模型/脚本/预览已有单一 owner | 继承已验收协议，不重复建设或顺手删除 |
| BAS-08 | 唯一施工账本已有位置 | 只更新 .internal/editor-redesign，dev_log 另作冻结快照 |
| BAS-09 | 分支并非必须永久保留 | 本阶段不自动合并/删除，后续明确授权即可处理 |

## ID：哈希、名字与运行身份

细则：[02_IDENTITIES_AND_DECLARATIONS.md](02_IDENTITIES_AND_DECLARATIONS.md)。批次：C1；验收主题：V03 V04 V05 V06 V07 V08。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| ID-01 | 固定名称运行期重复构造/哈希 | 使用 lux-cxx constexpr 预哈希声明 |
| ID-02 | 动态插件名称不是编译期值 | 在注册/外部文本解析边界计算一次 |
| ID-03 | 当前 find 仍逐条比较 name | 改原 Snapshot 内数值索引与已解析 handle |
| ID-04 | 拥有型 ID 的 view 可能重算 hash | 核对实际库，热路径复用已形成 token |
| ID-05 | 哈希不保证任意名字无碰撞 | 冷路径比较规范名并拒绝不同名同 hash |
| ID-06 | 相同名字重复与哈希冲突不同 | 分别诊断，不吞掉重复贡献或自动覆盖 |
| ID-07 | 跨目录版本的同哈希异名可能误绑 | CURRENT 重解析核对原规范身份，不将 A 请求发给 B |
| ID-08 | 不同语义域可能同数值 | 复用 tag/StrongId，命令/视图/设置不能互换 |
| ID-09 | 外部伪造 hash 或空名 | 从规范名验证，不能信任插件数值 |
| ID-10 | static 组唯一性不覆盖插件组合 | 编译期组内检查加运行期整批验证 |
| ID-11 | 原 StableNameId 精确相等被全局改写的风险 | 禁止全局改成 hash-only，仅优化验证后的索引 |
| ID-12 | 发布后再查旧 vector 地址会失效 | 保持原 handle/owner，或准确域代次，不缓存裸地址 |
| ID-13 | 热路径与文本边界混合导致虚报零比较 | 计数 cold resolution 与 steady dispatch 两类 |
| ID-14 | 持久身份不能使用地址或 std::hash | 保留 canonical 名与版本，反序列化重新解析 |

## MEM：单份描述与存储寿命

细则：[02_IDENTITIES_AND_DECLARATIONS.md](02_IDENTITIES_AND_DECLARATIONS.md)。批次：C1；验收主题：V09 V10 V11 V12 V13。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| MEM-01 | Spec 物化拥有型 Descriptor 会复制文字 | 撤销双份路线，原 CommandDescriptor 改轻量描述视图 |
| MEM-02 | 固定 Entry 按值长期复制描述 | 引用原模块 constexpr 描述，不复制 metadata 文本 |
| MEM-03 | 动态数据不能只改 string_view | 一份最终冻结 backing 保活同型 descriptor |
| MEM-04 | 静态插件文字也会随 DLL 卸载 | Entry/handle 持 CodeLease 到最后析构/控制块释放 |
| MEM-05 | 临时/可移动字符串导致悬空 | 固定声明绑定与动态工厂分开准入，禁止临时借用 |
| MEM-06 | dynamic SSO/vector 增长后视图失效 | 先完成存储再建立 view，冻结后不移动字符 |
| MEM-07 | 每个字段一个 shared_ptr 开销过大 | 一个 entry/backing 统一保活，不按 string_view 分配 owner |
| MEM-08 | 菜单复制完整业务描述 | 复用 handle/immutable source；仅保留真正呈现状态 |
| MEM-09 | 返回 descriptor 借用被跨期保存 | 文档/接口绑定 handle 寿命，不把裸 view 当 owning |
| MEM-10 | 静态元数据不等于全局自注册 | 显式加载安装，禁止 static initializer 副作用 |
| MEM-11 | 同类 View/Session 描述重复规格 | 复用原 descriptor 与已有类型常量，不造 Spec 家族 |
| MEM-12 | 无关临时日志都抽常量反而分散 | 只集中语义元数据，正常显示格式化保留 |
| MEM-13 | 零文本复制不等于零内存开销 | 计量 Entry/callable/index/backing/rodata，不预设节省字节 |

## CMD：声明归属与命令路径

细则：[03_APPLICATION_AND_COMMANDS.md](03_APPLICATION_AND_COMMANDS.md)。批次：C3；验收主题：V14 V15 V16 V17 V18。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| CMD-01 | 35 个固定 ID 的声明散在业务函数 | 逐项迁到准确模块集中声明 |
| CMD-02 | 12 个 view 描述重复 Pane/菜单名字 | 同一稳定 view 声明被相关消费者复用 |
| CMD-03 | 3 个作者种类/源格式关系散列 | 归对应 provider，保留 discovery/save 后缀原差异 |
| CMD-04 | 一个大 AllCommands 头会反向耦合 | 模块独立声明，装配汇总，不中央枚举 |
| CMD-05 | 动态 tool/<ViewType> 被误当固定项 | 在加载时生成一次并保活，插件不改宿主 |
| CMD-06 | 固定 ID/label/默认 shortcut 并非错误 | 保留缺省，仅分离 metadata 与 receiver |
| CMD-07 | 八个 Scene role 再解释字符串 | 拆工具/运行两组，直接 ESceneTool 或具名 handler |
| CMD-08 | 未知 tool role 默认变 configuration | 删除隐式回退，入口改准确类型 |
| CMD-09 | Undo/Redo 与 SaveMode 有限分支 | 可以保留，不为开闭原则增加插件接口 |
| CMD-10 | scope/input_version/argument_type 容易被抽漏 | 与描述一起声明并验证 |
| CMD-11 | Application execute 特判 save | 迁到业务 owner 准入，通用命令不解释结果类型 |
| CMD-12 | 菜单 completion 再特判 save | 删除重复登记，入口共享同一业务能力 |
| CMD-13 | 基础 Save 先注册再 erase/replace | 装配一次选择正确 receiver，共用声明 |
| CMD-14 | query/execute 捕获整个 Impl | 绑定所需 provider，Application 只安装贡献 |
| CMD-15 | 短字符串快捷键有两套 parser | 合并唯一语法，加载/设置提交时解析，事件只比键值 |
| CMD-16 | scope APPLICATION 被误当接收者类型 | 它只描述目标范围，不要求调用 Application 业务方法 |
| CMD-17 | About 是禁用信息项而非坏命令 | 保留原行为，测试不错误要求 execute 成功 |

## APP：Application 的实际职责

细则：[03_APPLICATION_AND_COMMANDS.md](03_APPLICATION_AND_COMMANDS.md)。批次：C2 C3；验收主题：V19 V20 V21 V22 V23 V24 V25。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| APP-01 | 生命周期类拥有并实现太多业务政策 | 按算法/状态迁出，不把 Impl 搬到 Context2 |
| APP-02 | 保存物理目标与资产身份验证在 App | 归 ProjectContentSaving，复用原文件/保存 provider |
| APP-03 | 源已保存目录未登记的部分完成 | 明确保留，失败不伪装为没有副作用 |
| APP-04 | 旧编译/保存完成覆盖较新目录 | 合并时读取当前记录，保留源/compiled 的独立版本 |
| APP-05 | SaveAll/Close/Exit 对结果重复确认 | 只由准确业务 owner 消费，借阅者走原协议 |
| APP-06 | 命名对话框与无 UI 保存混合 | 对话框留工作台；活动不依赖 ReviewView |
| APP-07 | 工作区文件政策和 Host 组合混合 | IO/迁移归 activities，Host 操作归 workbench |
| APP-08 | 布局已应用与 preferences 发布不同 | 分别报告，不为后者失败抹掉前者事实 |
| APP-09 | 恢复偷偷解析任意 layout opaque | 继续只读独立 RecoveryManifest |
| APP-10 | 最近项目的文件/上限/去重在 App | 归实际项目浏览活动，Pane 不持 Impl |
| APP-11 | 插件选择同时变成通用设置副本 | manifest plugins 唯一，settings 仅提供编辑入口 |
| APP-12 | 结果面板可能被造为第二任务管理器 | 只观察原活动结果，不复制取消/完成权威 |
| APP-13 | 公开 facade 仍可能全知 | 迁移消费者或保留严格中性转调，不留业务分支 |
| APP-14 | 关闭时停止 pump 丢弃已接受完成 | 保持原 Process/Run/GPU 收集与退休直到结清 |

## SET：设置的数据、作用域与扩展

细则：[04_SETTINGS_AND_STARTUP.md](04_SETTINGS_AND_STARTUP.md)。批次：C4 C5；验收主题：V26 V27 V28 V29 V30 V31 V32 V33 V34 V35。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| SET-01 | 启动配置/偏好/作者配置混名 | 明确作用域，Scene 配置仍属于作者 Session |
| SET-02 | 现有 UserPreferences 主要 selected_layout | 保留/迁移原含义，不宣称原有完整设置系统 |
| SET-03 | SettingsView 目前只是插件选择 | 扩展页面 registry 和实际读取应用，不只改标题 |
| SET-04 | 窗口/字体直接由参数默认值创建 | 建立文件+启动覆盖+环境到有效配置链 |
| SET-05 | 设置字段无限追加 AppConfig | 复用 ConfigurationValue/descriptor，按功能声明 |
| SET-06 | 插件需动态注册设置 | 在原贡献批次增加设置描述与 CodeLease |
| SET-07 | 插件缺失/新版本配置 | 保留未知段，严格 schema 迁移，不覆盖成默认 |
| SET-08 | 缺文件/权限/格式混淆 | 只缺文件允许 defaults，其他错误明确显示 |
| SET-09 | 多 scope 覆盖顺序含糊 | 逐项声明允许作用域与 merge policy |
| SET-10 | 用户 project_instance 不是持久项目键 | 使用持久身份或明确路径派生迁移规则 |
| SET-11 | settings 与 manifest 插件启用两份权威 | 不新增 enabled 镜像，显示当前激活和下次选择 |
| SET-12 | desired/applied/persisted 混在 dirty | 准确显示三种事实及需重启状态 |
| SET-13 | Apply 失败丢草稿 | 保留 based_on/诊断，允许明确修正/重试 |
| SET-14 | 发布版本变化/Unknown | 沿原 WriteCoordinator 校验与对账 |
| SET-15 | 字体/scale 动态更换可能早释放 | 原 owner safe point 或明确 restart-required，资源仍保活 |
| SET-16 | 快捷键覆盖修改 canonical identity | 只覆盖有效输入绑定，不改命令身份/版本 |
| SET-17 | 插件命令缺失后用户覆盖丢失 | 保留未激活 override，恢复时按兼容版本解释 |
| SET-18 | bootstrap 配置依赖插件 UI 形成环 | 先稳定引导值，再贡献 schema，再功能激活 |
| SET-19 | codec 是否保留注释/原字节被夸大 | 明确值等价还是字节保证，不虚报 |
| SET-20 | 所有设置都热生效的错误目标 | 每项声明策略，不新增任意插件热卸载需求 |

## WIN：窗口策略与显示环境

细则：[04_SETTINGS_AND_STARTUP.md](04_SETTINGS_AND_STARTUP.md)。批次：C5；验收主题：V36 V37 V38 V39 V40。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| WIN-01 | 尺寸不是当前自动计算 | 记录现状，实施显式/保存/环境/缺省优先级 |
| WIN-02 | 显示分辨率乘比例过于粗糙 | 使用工作区和明确坐标单位，集中默认政策 |
| WIN-03 | 窗口坐标与 framebuffer 混用 | 分别保留，GPU extent 不直接当 UI 尺寸 |
| WIN-04 | DPI/scale 双重乘法 | 单处坐标换算与字体策略 |
| WIN-05 | monitor pointer/name/index 持久化不可靠 | 保存可解释提示，断开后重新匹配/回退 |
| WIN-06 | 多屏负坐标被当非法 | 使用有符号坐标，检查范围/溢出 |
| WIN-07 | 任务栏和边框导致窗口不可达 | 保证合理可见区域，处理实际装饰信息 |
| WIN-08 | 极小工作区/零信息 | 明确退化值或错误，不无符号下溢 |
| WIN-09 | 最大化覆盖 ordinary rect | 分开持久化模式与恢复矩形 |
| WIN-10 | 全屏与最大化混为一谈 | 至少一种明确支持的全屏模式及恢复，不假装全部后端支持 |
| WIN-11 | 窗口构造暗读文件或探测环境 | 解析在构造前完成，构造只消费有效参数 |
| WIN-12 | offscreen 意外查询 monitor | 保持明确确定尺寸，无物理显示依赖 |
| WIN-13 | 每帧按 monitor 重算默认值 | 只在启动/实际环境或用户事件发生时更新 |
| WIN-14 | 显式 CLI 覆盖无意写回文件 | 本次覆盖单独一层，保存必须明确 |

## CAM：相机与封装

细则：[05_CAMERA_AND_ABSTRACTION.md](05_CAMERA_AND_ABSTRACTION.md)。批次：C6；验收主题：V41 V42 V43 V44 V45 V46 V47。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| CAM-01 | CameraPose 不只是 pose | 正名 ViewportCameraState，复用 Camera/Transform，不复制数据 |
| CAM-02 | CameraMotion 被误当第二组件 | 保持输入增量角色，不新增 CameraManager |
| CAM-03 | ECS 链成立不证明无冗余 | 逐边记录责任/状态/频次/回调后裁定 |
| CAM-04 | 层数多不等于过度封装 | 保留 UI/请求/资源/抽取各真实边界 |
| CAM-05 | 编辑器自由相机污染作者 | 只写自己创建的辅助 entity，不进作者 History |
| CAM-06 | 借用游戏相机被同时导航/脚本写 | 明确单写者/模式，低层不隐式扩权 |
| CAM-07 | Run 自由观察被宣称游戏相机 | 明确不同模式，不默认支持未实施跟随 UI |
| CAM-08 | 期望/ECS/显示帧相机可能不同 | 测试版本对应，复用原输出观察或明确 defer |
| CAM-09 | 拾取/模型放置用不一致 extent/DPI | 统一相机/范围选择，不二次缩放 |
| CAM-10 | 延迟输出/旧实例输入重定向 | 固定原目标与代次，失败不解释成新窗口点击 |
| CAM-11 | 相机未变仍重复 patch/projection | 删除可证明同有效期重复工作 |
| CAM-12 | 公共准入与后续 render 校验看似重复 | 保留不同有效期，禁止永久验证 token |
| CAM-13 | 信号安装后漏存量 Camera | 首次完整折入，验证不同构造顺序 |
| CAM-14 | 直接 get 写不发 on_update | 全部可观察修改保留 patch/replace |
| CAM-15 | on_destroy 中立即改世界 | 读句柄入原延迟命令，后批安全应用 |
| CAM-16 | 异步资源被改成仅 on_construct | 保留完成轮询/安装/退休，不能丢晚到结果 |

## GEN：生产代码生成统一

细则：[06_CODEGEN_UNIFICATION.md](06_CODEGEN_UNIFICATION.md)。批次：C7；验收主题：V48 V49 V50 V51 V52 V53 V54 V55。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| GEN-01 | Inspector 是 Python 生产 emitter | 迁至原 MetaUnit/语义/inja 链，不否认其现有作用 |
| GEN-02 | Render/Script 已用模板不能重写 | 保持原 projection 与基础，改公共工具时相应回归 |
| GEN-03 | 全部 .py 不等于生产生成器 | C0 分类 producer，测试/归档/编排可留 |
| GEN-04 | 不能再写 C++ parser | 复用 MetaUnit，缺 IR 修真实输入，不正则猜语义 |
| GEN-05 | 函数体通过 f-string/列表拼接 | 结构由 inja 表达，helper 不先拼完整 body |
| GEN-06 | 作者/Run 生成后全文 replace | 显式 binding mode/namespace/fields 投影 |
| GEN-07 | 现有字段语义不能丢 | 逐项迁移标量/容器/嵌套/optional/variant/Eigen/custom |
| GEN-08 | readonly 仅禁表层按钮 | 覆盖结构修改与嵌套自定义控件 |
| GEN-09 | 64 位数值规则用 double 降精度 | 精确解析/溢出与 finite 验证 |
| GEN-10 | 多处格式规则容易漂移 | 统一一份 formatter 配置 |
| GEN-11 | sidecar 不在真实输出清单 | 声明 OUTPUT/BYPRODUCT，缺失可重建 |
| GEN-12 | 广泛递归头依赖造成过度重建 | 有真实 depfile 后再收窄，不能偷删依赖 |
| GEN-13 | 模板/support/flags 变化未失效 | 依赖包含所有真实输入与工具版本 |
| GEN-14 | 部分输出写入失败被当成功 | 先语义/渲染/staging，失败不发布成功 stamp |
| GEN-15 | 内容未变也重写时间戳 | 比较内容，仅必要写入，验证二次无工作 |
| GEN-16 | 重复 stem 与跨机路径差异 | logical path 唯一、确定排序、路径不泄漏 |
| GEN-17 | SDK 偷读源码/private/旧 Python | 安装 generator/templates/support，独立消费者 |
| GEN-18 | host 工具误用 target executable | 明确 host 工具来源与架构，构建图准确 |
| GEN-19 | 组件/底层反向依赖 UI 生成器 | 行为看数据，工具与运行 target 隔离 |
| GEN-20 | 保留旧 emitter 开关造成两套实现 | 最终只留一个生产路径，历史快照不删 |

## QUAL：质量、性能与交付

细则：[07_QUALITY_AND_MEASUREMENT.md](07_QUALITY_AND_MEASUREMENT.md)。批次：C8 C9；验收主题：V56 V57 V58 V59 V60。

| 编号 | 当前问题／约束 | 必须采取的处理 |
|---|---|---|
| QUAL-01 | 静态存储少不等于整体零分配 | M1/M2 记录真实 ABI/owner/index/rodata |
| QUAL-02 | 名字不比较但包装反复查表 | 记录实际调用栈/lookup 次数，不用行数证明性能 |
| QUAL-03 | 旧微基准被当当前产品性能 | 保留原 SHA/环境，EC3 新成绩单独记录 |
| QUAL-04 | 复杂判断未遵守具名语义分组 | 按 AGENTS，前置有效性不能外提破坏 |
| QUAL-05 | 多层 Result/异常捕获重复 | 保留准确边界，内置热路径不堆 try/catch |
| QUAL-06 | 数据头重依赖/别名 shim | 精确 include/provider，一次迁完旧入口 |
| QUAL-07 | 全部模块一个 giant library | 目录不是库，保留 CPU/工具链/GPU/SDK 真实闭包 |
| QUAL-08 | modules 公共头旧安装被 parser 读取 | 三个 include 前缀同步，不冒称 Android 构建 |
| QUAL-09 | 构建与真实 DLL/GPU 并行 | 严格顺序，all -j4 -k0，二次无工作 |
| QUAL-10 | 测试数量替代语义/性能百分比 | 按 V 主题与实际计量，不设不必要样本配额 |
| QUAL-11 | 文档仍混写 V7/V8 与当前 V9 | 更新当前文档/SDK，历史版本原样保留 |
| QUAL-12 | 有限完成被当无缺陷或全平台资格 | 分开已运行/继承/免验/延期/未测并固定 SHA |

## 使用规则

每行必须回填实际文件、类型、消费者和证据。允许结论为“核查后保留”，但须说明为什么原实现已经满足，不能空写 N/A。
最终完成意味着条目处理、消费者迁移和验证一致，不是把 OPEN_INPUT 批量替换为 PASS。新增发现追加到同一账本，不改写本输入种子的历史。

---

<!-- SOURCE_CHAPTER: 02_IDENTITIES_AND_DECLARATIONS.md -->

# EC3-A：身份、模块声明与单份描述存储

## A1. 已确认的现状，不要误读为目标设计

`CommandDescriptor` 当前拥有 CommandId 和三个 `std::string`；Entry 按值保存它。`CommandRegistrySnapshot::find()` 当前逐条执行 `entry->descriptor().id.name() == id.name()`；快照创建用嵌套循环排重。[S03,S04,S06]

本次读取的 lux-cxx 参考版本中，`StableNameIdView` 已缓存 constexpr FNV-1a64；相等运算仍为 hash 与 name 双重检查。拥有型 `StableNameId::view()` 调用 `fromVerified()`，会重新校验名字哈希。不能因为类型名字包含 hash 就假定每次调用没有读字符。[S05]

这个参考版本来自在线仓库，不是用户机器当前安装资格。历史 P10Q 明确用过不同的 lux-cxx SHA；C0 必须核对实际源、三个 include 前缀、工具包和 TypeToken。不得为了适配本文示例而降级依赖。

## A2. 区分名字、稳定身份与本次调用句柄

| 层面 | 用途 | 是否可以读取字符串 |
|---|---|---|
| 规范名 | 插件注册、文件恢复、日志和外部文本入口 | 可以；是冷路径 |
| 预计算稳定身份 | 在同一身份命名空间内建立索引 | 比整数；不得把 hash 当密码或权限 |
| CommandHandle | 固定条目、版本和代码寿命 | 直接访问条目，不重新按名称查找 |
| 显示文本 | label、分组、用户搜索、翻译 | 可以；不是业务分派键 |
| 领域目标 | SessionId／ViewId／ContentStamp 等 | 继续用现有完整身份与代次 |

本轮不将不同语义域的 uint64 混用。命令、视图、设置、源格式身份有各自的 tag／已有强类型。一个 hash 不能代替 SessionId、文件摘要、资产魔数、输入版本或代码 ABI。

规范名继续可持久化。不要把旧配置中的稳定名字改成指针、进程内槽位或 `std::hash` 的不稳定结果。国际化 label 和用户重命名也不能改变命令身份。

## A3. 注册时验证，派发时不读业务名称

固定声明：使用现有 constexpr 字符串／预哈希视图。常量必须在编译期形成，不在每次 query 里构造拥有型 CommandId 再 `.view()`。

动态声明：在接受整个候选目录时规范化并计算一次。验证顺序如下：

1. 验证规范名非空、编码、长度、身份域、版本和完整描述。
2. 重新计算或验证输入携带的 hash；不信任插件自己填写的数值。
3. 建立候选 hash 索引，检测重复。
4. 同一名字重复注册按原重复／替换政策处理，不能混成 hash 冲突。
5. 两个不同名字同 hash：返回明确 HASH_COLLISION 类诊断，整个候选不发布。
6. 验证 callback、参数类型、shortcut、owner 和容量；成功后按原批次协议共同发布。

外部文本入口只在解析／激活边界比较规范名，取得已解析 handle；任意文本不能只取 hash 后直接命中另一个同 hash 条目。该边界可以保留一次精确比较，不能为满足“零字符串比较”破坏正确性。

有限宽度哈希对任意扩展集合不可能保证绝对无碰撞。这里选择明确拒绝碰撞，换取活动目录内的纯数值查找，而不是在每个热查找中做字符串回退。

不要修改 lux-cxx 全局相等规则。相同数值的目录外 raw hash 不是已验证的命令权限。原 API 如果接受 `CommandIdView`，把它明确列为身份解析边界；正常 UI／快捷键和程序化循环持有解析后的 handle。不得每次转成 string 再调用 resolver。

## A4. 直接扩充原快照的索引，不建立第二个注册器

在 `CommandRegistrySnapshot::Data` 中添加一次构造的数值索引。首选紧凑排序 `{hash, entry_index}` 数组，冷路径 O(N log N)，热 lookup O(log N)；如实际容器基准支持现有 lux-cxx hash container，允许替换，但不是本轮必须另写 hash table。

保留 `entries` 的展示／装配顺序。不要为了索引把用户菜单顺序隐式改成 hash 排序。

该索引只是原不可变目录的派生索引。不得复制 CommandEntry、字符串、回调、代码 lease 或第二套 revision。快照替换时索引与条目共同发布。

已有 CommandHandle 直接引用条目，PINNED 调用不需要新 slot map。CURRENT_REGISTRATION 在出队时用固定稳定 ID 查当前数值索引，再验证原 scope／参数／input_version；不能把旧请求重绑成任意新接口。

### 跨目录版本的同哈希不同名字

仅检测同一候选集合还不够：旧队列可能持有 A，而新的活动目录已移除 A 并加入同 hash 的 B。CURRENT_REGISTRATION 不能因此执行 B。

原 handle 固定其规范身份；当 registry revision/entry 改变需要重新解析时，必须验证当前条目仍是同一规范身份，或者使用已有的经过验证的 intern identity。默认采用**版本切换的冷重解析**：hash 定位后一次精确身份核验，再建立可复用的当前 handle。正常未变版本的派发不做该字符串工作。

持久配置、快捷键文件和外部文本同样走冷解析；不能将只存 hash 的旧来源直接认作当前名称。若使用 intern，必须属于原 registry 的有限身份管理，不是跨项目全局符号池，并记录旧 handle 释放与容量回收。没有必要不新增 intern。

这项正确性开销单独统计为 cold resolution，不能从性能日志隐藏。不要为宣称任何时候零比较而接受错误命令。

如果为 UI 事件增加一个紧凑 locator，它必须具有目录域／代次或仍由原 handle 锚定，不得缓存 vector 地址并跨发布使用。没有实际需要则不增加该类型。

## A5. 同一种 CommandDescriptor：literal 与动态来源共享 schema

撤销旧建议中的长期 `CommandSpec + materializeCommand()` 双份结构。

目标形状如下。它是本阶段目标代码，不是当前已编译接口：

```cpp
struct CommandDescriptor final
{
    CommandIdView id;                  // 复用 lux-cxx 的预哈希、tag 化名称视图
    std::string_view label;
    std::string_view group;
    std::string_view shortcut;
    ECommandScope scope{ECommandScope::APPLICATION};
    std::uint32_t input_version{1};
    cxx::TypeToken argument_type;
};

inline constexpr commands::CommandDescriptor exit_command{
    commands::CommandIdView{"lux.editor.exit"},
    "Exit",
    "File",
    "Alt+X",
    commands::ECommandScope::APPLICATION
};
```

沿用原类型名、公开逻辑 include 和语义字段。不要在 Editor 复制另一套 FNV、FixedString、TypeToken。若实际 TypeToken 的 constexpr 支持不足，先在正确的 lux-cxx 基础处补齐或采用已有 literal token；不以增加第二个 CommandSpec 绕过。

CommandDescriptor 此后是一份非拥有描述视图。借用不是默认安全：必须与下一节的 Entry 存储契约一起迁移，不能只把 `string` 搜索替换成 `string_view`。

## A6. 静态、插件静态、动态描述的三种寿命

### A6.1 固定内置描述

实际模块在 `.hpp` 或 `.cpp` 声明 `inline constexpr/static constexpr CommandDescriptor`。若外部调用方需要类型安全引用，就放最窄公共语义头；仅模块内使用则留 CPP／pinclude，不能强迫所有模块 include 总表。

Entry 引用同一 descriptor，不为四段文本申请堆内存，也不把它再 materialize 成拥有型描述。可用引用非类型模板参数绑定固定声明，保证调用方不能传临时 descriptor：

```cpp
// 目标示意；实现应复用原 Entry / callable，不增加执行期转发。
auto entry = bindCommand<exit_command>(code, query_exit, execute_exit);
```

这只是创建时的静态绑定辅助。命令执行仍走原 Registry→实际 callable。

### A6.2 插件内的静态描述

同样直接引用插件静态 descriptor，但原 CodeLease 必须覆盖描述、文字、callback、payload、析构以及控制块清理。不能假定静态文字在 DLL 卸载后仍可用。

检查现有 `pinCodeOwner` 和弱控制块回归，不重写已验证的代码保活链。不允许插件用 builtin lease 伪报其来源。

### A6.3 运行时生成的描述

动态工具菜单、插件产生的 label、运行期默认值：需要一次拥有型 backing。复用已有 immutable owner／SharedBytes 等基础，在私有 `DescriptorStorage` 中最终冻结全部字符和同一 CommandDescriptor。

冻结前可以用局部 string 构造；所有增长、移动完成之后再建立 string_view。不能指向构造器临时 string、即将 reallocate 的 vector 或可被 settings 页面改写的缓冲。

Entry 只保留 descriptor 指针／引用与可选 backing owner；静态路径无需 backing 分配。动态路径不再保留一份源 Spec 或一份额外拥有型 Descriptor。

允许两种内部实现：原 Entry 持指针和可选 owner；或引用同一 immutable allocation 中的 descriptor。C0/C1 对比实际 `sizeof`、Entry 分配和间接层数后选择。不得为了省一个 owner 指针引入悬空借用。

public `descriptor()` 返回借用，其有效期绑定 handle／entry／snapshot。需要跨期持有者保留现有 handle，不裸复制 string_view。

## A7. 哪些成本可以消除，哪些必须承认

| 项目 | 目标 |
|---|---|
| 固定命令元数据的文本堆复制 | 0；引用模块静态文本 |
| 固定 descriptor 的第二份长期 schema 对象 | 0；Entry 引用原声明 |
| 运行期动态文本的物理内容副本 | 一份冻结 backing；不按每个消费者复制 |
| Entry／callable／CodeLease／索引 | 有实际用途，不能宣称零成本 |
| UI 的树节点、状态与个性化 label | 可以存在；它们不是另一份业务目录 |
| 每次已解析命令派发的按名称扫描／重新哈希 | 0 |
| 外部字符串第一次解析与注册碰撞验证 | 允许并单独计数 |

不要预写“节省 X 字节”。在实际 ABI、MSVC/STL、32/64 位条件下记录 `sizeof(CommandDescriptor/CommandEntry/CommandHandle)`、静态数据量、字符容量、控制块及索引成本。

默认不为每个 string_view 增加 shared_ptr。一个 entry 或 snapshot 的 owner 保持整份描述足够。字符存储不重复，不等于 schema record、索引、引用可以不存在。

## A8. 菜单与快捷键：复用描述，不重新建字符串业务表

CommandMenu 保留固定 handle 与输入。UI 层不能 include Editor command 实现；使用共同的现有 UI CommandIdView 与明确寿命的菜单观察。

读取当前 MenuItem 契约后选择：让 CommandMenu 以原快照保活轻量菜单描述；或者在现有 UI 菜单模型中补中性的 source owner。不要为每一项复制 descriptor 的全部字符串，也不让 Root 借一个临时 span。

菜单布局可以保存自己的树和 group 节点，但只在目录／有效菜单配置 revision 变化时重建。用户翻译或重命名的显示文本与固定 canonical ID 分开存储。

当前 validShortcut 与 CommandMenu::shortcut 存在两段相近语法处理。[S03,S07] 提取一份中性解析规则放在实际共同 provider；复用原按键类型，不复制第二个 EKey，也不能让 activities/commands 链接 ImGui。

固定默认快捷键在装载时解析；用户覆盖在设置提交时解析，键盘事件不再解析 `"Ctrl+..."`。明确无绑定、未知按键、重复修饰、多个命令冲突、平台等价键等结果。保留当前支持的键集合，不靠静默回退为 key=0 消化错误。

group 的 `File/Submenu` 文本只在菜单编译边界解析。稳定 UI 帧不得重新切割所有 group 或逐名字匹配目录。

## A9. 三类声明的统一范围

命令的同型策略优先完成。ViewFactoryDescriptor、SessionKindDescriptor／SourceAuthoring 使用同样的 lifetime 原则，但必须优先复用已有类型和常量，不能再建对应 Spec/CompiledSpec 套餐。

固定 `content_kinds`、extensions 等数组可用 module-static array/span；动态集合由一份 immutable backing 保活。Entry 保留类型版本、默认关系和 CodeLease。

PaneTypeId 与 ViewTypeId 的既有同一语义不应拆开；SessionKind 与 AssetType 不应错误合并。新插件不需要修改宿主中央枚举。

35 个固定命令 ID、12 个固定 view、3 个 source 描述是初始必须迁移的集合。C0 扩展核查同类生产注册；逐条标注迁移、已符合、动态规则或确实不适用，不把正则命中数当成完成数。

---

<!-- SOURCE_CHAPTER: 03_APPLICATION_AND_COMMANDS.md -->

# EC3-B：准确的业务 owner、中性命令路径与生命周期 Application

## B1. 固定边界

EditorApplication 类的核心职责是产品生命周期与具体装配：创建、激活、主循环、请求退出、停止新工作、结清已接受工作、销毁顺序和最终错误出口。

application 层仍可以承载真正跨内容、工作台和平台的用例，但它们不必都是 EditorApplication::Impl 的方法和成员。

持有 ProjectStorage 不等于实现项目保存政策；启动时恢复工作台不等于在 Application 中实现布局目录政策；安装命令不等于成为全部命令 receiver。

禁止把所有 Impl 字段搬进 `EditorServices/EditorController/Context2` 后继续让每个功能捕获它。只迁有完整输入、结果和唯一责任的用例。

## B2. 现有命令不是两套系统

当前菜单通过 CommandMenu→CommandDispatcher→CommandRegistry；程序化 facade 通过 Application::execute→同一 CommandRegistry。[S07,S08,S09]

保留这一点：命令描述、query、execute、拥有型 invocation、PINNED／CURRENT_REGISTRATION、代码 pin 和 publication Batch 是已经存在的真实能力。

删除的是下面这些外围问题，而不是重新造 ICommand 层：

- Application::execute 对 `AcceptedOperation.kind == "save"` 的解释。
- 菜单 takeCompletions 后同样的 save 特判。
- save() 已登记，外层又登记 pending/results 的分散责任。
- 先安装基础 save，再按名字 erase，最后安装项目 save 的替换政策。
- Scene role 八字符串既生成 ID，又在 callback 里解释一次，再在 showSceneTool 解释一次。
- 大量命令 capture 整个 Impl，而不是其实际所需 provider。

## B3. 项目保存迁移——不是把原 SaveService 再包一层

### 当前迁出位置

`editor/application/src/EditorSaving.cpp` 的 prepareSave、save、settleSaves 中关于物理路径、AssetId、项目源登记、manifest 发布和采用的算法；以及 Impl 中与这些结果相关的记录。[S10]

### 目标归属

在 `editor/activities/project` 的真实项目活动 target 中形成 `ProjectContentSaving`（目标名称）。如已有等价活动，扩充它而不是新增同义类。

该类型拥有的是**源保存加项目登记这一复合活动**，不拥有作者 Session，不复制 SaveService 的编码状态机，不复制 WriteCoordinator。以原 SaveId 或现有可辨别的复合操作身份关联记录，不再建第二份 source outcome。

它的依赖是原 SessionStore／保存角色、SaveService、ProjectStorage、WriteCoordinator 及执行接线；不得依赖 Pane、ReviewView、EditorApplication 或 CommandInvocation。

### 必须迁入的政策

| 算法／状态 | 处理 |
|---|---|
| source 规范名、版本和默认后缀选择 | 读取已有 factory 的 SourceAuthoring；不得恢复三类型 switch |
| Save As 目标存在／其他资产占用验证 | 在实际物理解析后验证；路径文本 hash 不能替代物理身份 |
| AssetId 分配 | 仍一次分配并固定于实际请求；重试不得改号 |
| 源已发布但目录登记失败后的恢复 | 保留原物理绑定和部分完成事实，不猜当前目录 |
| manifest 基于最新记录形成候选 | 保留当前 compiled 信息；不能让旧完成覆盖较新字段 |
| 文件冲突、Unknown、清理失败 | 分别保存，不能降格为普通 BUSY 或成功 |
| report/pending/acknowledge | 活动准入成功时即建立完整责任，不靠命令回执补登记 |
| SaveAll | 固定 Session 集合，包含无视图工作副本；源和项目登记分别有结果 |
| Close／Exit 借阅结果 | 与原结果 owner 协调，不由两个地方重复 acknowledge |

### 留在 UI／产品组合的内容

ReviewView 的路径输入、用户回答、关闭 review Pane 与取消临时交互。UI 得到保存请求数据后调用同一个 ProjectContentSaving。无 Root 消费者直接调用同一 API。

命名／overwrite 的用户决定与文件实际许可分开。UI 较早说“可以覆盖”不能取消真正发布前的物理文件版本检查。

普通 Save 的目标政策应按 C0 冻结的原行为保留：会话目标固定；当前内容在正式准入时确定。明确携带 based_on 的请求不得偷偷丢弃或重绑定。不同入口存在语义差异时先统一为有明确名称的两种请求，不默改现有测试。

Export Copy 不采用作者 checkpoint；Save As 不清 History；编译成功不等于源已保存。原 P05／P11 的可靠完成接收与清理保护一并继承。

## B4. 工作区、恢复与最近项目

原 WorkspaceStore 继续负责文件、版本、迁移字节和 WriteCoordinator 接入。实际 Host 布局准备／提交仍在工作台。

迁出 `EditorWorkspace.cpp` 的 save/rename/delete/fallback/selected-layout/迁移推进政策，到现有 workspace 活动；需要 Host 的完整工作台组合放 `workbench/desktop` 的 `WorkspaceActions`（目标角色），不让 activities 反向依赖 UI。

内容恢复只读取 RecoveryManifest，不解析任意 Layout opaque 去打开资产。跨内容打开与 Host rebind 的协调可为 application 中独立 `RestoreWorkbench` 用例，Application 只启动、驱动、等待它。

保留两个事实：布局在 Host 提交成功；selected_layout 文件写入可能失败。不能为简化事务回滚已真实应用的 UI，也不能把后者成功假定为前者成功。

最近项目是用户级文档，不是通用 EditorApplication 私有状态。将读取／去重／上限／发布放可复用的项目浏览活动；RecentPane 不持 Impl&。已有同名 activity 时复用，禁止增加全能 PreferencesManager。

ProjectCreation、ModelImporter、ArtifactPublicationOperation 已有真实 owner，直接使用它们，不再次迁出同一算法。Application 中仍有未迁政策时以函数与调用清单定位，不按文件名猜测全部过时。

## B5. 项目插件选择

`ProjectManifest.plugins` 继续是项目启用选择的唯一权威。通过 `ProjectPluginSelection` 这项项目活动（目标角色）复用 ProjectPublicationOperation，负责草稿基线、发布、重试、放弃和结果。

新设置中心可以展示它，但不得再保存另一份 `plugins_enabled` 到用户设置文件。当前激活集合与下次启动选择不同，要明确显示需重新打开项目。

运行期插件和 Editor 扩展加载继续用原 PluginLibrary／EditorExtension。不是因为增加设置就重做装载器或默认允许热卸载。

## B6. 模块内声明及安装职责

固定描述由真正功能模块声明；有状态绑定函数接收准确 receiver，不接收 EditorApplication&。下表是默认归属，不等于必须新增相同数量的 target：

| 命令组 | 声明／绑定所在主题 |
|---|---|
| Undo/Redo、无项目基础源保存 | activities/sessions 的实际角色主题 |
| 项目 Save/Save As/Export/SaveAll/Reload | activities/project 的保存活动；命名 UI 接线在工作台 |
| Close View／Another View | workbench/desktop 的内容视图动作 |
| Scene 四工具 | workbench/scene；直接绑定 ESceneTool |
| Play/Pause/Resume/Step/Stop | 原 Run activity + 工作台控制的明确组合 |
| 新 Scene/Material/Flow | 各领域的 preparation provider 与视图创建接线 |
| Assets/Tasks/Import/Settings/Results/Recent | 各自工具模块的视图与命令贡献 |
| 工作区与恢复 | workspace 工作台主题／独立恢复用例 |
| Exit、关于产品信息 | application 的生命周期／产品声明 |

跨功能组合可以在 application 中保留一个显式装配函数，但它只拼接模块贡献，不逐个认识其 command_id，不把所有描述再抄一份。

基础源 Save 和项目 Save 使用**同一稳定声明**，由装配选择一个语义明确的 receiver；不得两者先装上再删一个。无项目消费者仍有基础保存能力，产品使用项目保存能力。不要为避免重复 ID 删除合法的低层 API。

## B7. 消除字符串二次解释

Scene 的四工具与四运行控制分别形成声明组。showSceneTool 接收原 `ESceneTool`，不再接受任意 role string；删除未知字符串自动落到 CONFIGURATION 的回退。

Pause／Resume／Step／Stop 直接绑定具名 handler 或已有封闭枚举。Save 三模式仍可用 `{descriptor, ESaveMode}` 的表；Undo/Redo 用两个具名条目共享原算法。不是每个有限模式都需要扩展接口。

动态 `lux.editor.tool/<ViewTypeId>` 在贡献加载时形成一次，使用动态 backing 与数值目录；后续每帧不重复拼字符串。标题、group 与 ViewTypeId 的复用遵守寿命，不缓存插件原始临时字符指针。

`AcceptedOperation` 可以继续是对既有业务结果的观察，但 canonical kind 需要进入既有稳定身份体系。不能只把 `"save"` 换成 hash 常量后继续在 Application 特判同一业务。

结果展示按实际活动提供的观察与准确操作构造；Activity 是结果 owner，ResultsView 不是另一个 OperationRegistry。不得引入一个知道所有活动的全局结果取消器。

## B8. 程序化入口和菜单入口

原 `EditorApplication::execute()` 的消费者是迁移对象：Application 测试、installed editor-application consumer、骨骼插件 APP 路线和其他实际调用者。C0 必须核对全部真实引用。

最终 Application 不再解释或执行业务命令。程序化调用方取得正式 CommandRegistry/Dispatcher 或产品组装返回的窄 command capability；菜单使用同一 Registry。需要直接保存的 C++ 工具可以绕过命令模式调用活动 API，但不能绕过领域准入。

如果一个中性 facade 确实简化 product API，可暂保留同名 execute，但必须只有 owner/phase 准入和原 Registry 转调，无字符串特判、业务 state 或结果登记；账本明确其理由与实际消费者。默认目标是迁出而非保留空转发，不能以 facade 为借口新增 RuntimeDispatcher 等层。

WRONG_THREAD 与 BUSY 分类继续准确。不要为了减少重复判断让外线程读到 owner 私有状态，也不要删除 query→execute 间因状态可能变化而必要的检查。

PINNED 句柄保持原语义；CURRENT_REGISTRATION 使用当前目录但验证兼容版本。拒绝不消费 invocation；BUSY 队首保持原目标；回调入队进入后批。

## B9. 生命周期最终结构

Application 持有按实际依赖顺序构造的产品对象，驱动具体 owner 的 update，不使用字符串调度各子系统。

退出阶段仍必须：拒绝新请求 → 收集用户决定 → 提交真正许可 → 停止新任务／运行 → 持续收集已接受完成 → 关闭视图 → 等待原资源退休 → 撤销贡献 → 释放引擎与平台。

实际顺序按原资源依赖调整，不能照抄上面文字强迫先后；需要保留真实 safe point 和 GPU 引用。重点是代码 lease 不早于 callback/payload/deleter，任务不因 UI 消失而丢结果。

不允许一种新活动在每帧必须遍历另一个活动的全部私有记录。用明确的公共结果／版本／通知；现有 TaskMonitor、LuxObject 与 Process 完成链继续复用。

完成判据是独立使用和唯一责任，不是 EditorApplication.cpp 低于多少行。禁止将业务体改名后放 `EditorApplicationInternalServices.cpp` 并继续依赖整个 Impl。

---

<!-- SOURCE_CHAPTER: 04_SETTINGS_AND_STARTUP.md -->

# EC3-C：可扩展设置、窗口环境与生效链

## C1. 先明确现状

当前 EditorApplicationConfig 是启动输入，提供项目／安装位置、title、width/height、offscreen、font 和 user_directory；尺寸有默认值，调用方可覆盖，不是硬编码在不可变构造体内。[S21,S22]

当前工作区有真实偏好／布局／恢复文件读写；UserPreferences 主要保存 selected_layout 与保留数据。SettingsView 主要是项目插件选择，不是通用窗口／字体／快捷键／插件自定义设置中心。[S24,S25]

本章新增目标，不得在 EC3 完成之前把上述能力描述成已经存在。默认值本身不是错误；缺少的是来源、覆盖、验证和应用的完整关系。

## C2. 设置不是一份无限扩张的 Config

| 作用域 | 例子 | 实际保存与责任 |
|---|---|---|
| 安装缺省 | 内置 UI 默认值、声明默认快捷键 | 安装只读资源／静态描述；不可原地覆盖 |
| 用户首选项 | 字体、UI scale、个人快捷键 | OS user config root 下的版本化文档 |
| 项目共享设置 | 项目插件选择、项目工具默认策略 | 项目自身文档／manifest；团队共享 |
| 用户的项目状态 | 窗口布局、上次内容、个人覆盖 | 用户根下的项目作用域，必要时只读迁移旧位置 |
| 启动覆盖 | --font、显式尺寸、offscreen | 本次内存层；不自动保存回文件 |
| 作者内容 | 某 Scene 的 RenderFeature／系统配置 | 原 Session、History 和资产保存；不迁入全局设置 |
| 运行参数 | 队列容量、线程策略、diagnostics | 原参数类型和合法性；并非全部都必须出现在 UI |

不得将这六类来源简单放在同一张字符串 map 中按插入顺序覆盖。Descriptor 必须明确允许作用域与 merge policy。

默认优先级：安装缺省 < 允许的项目共享缺省 < 用户首选项 < 用户项目覆盖 < 显式启动覆盖。对只允许用户／只允许项目的项，非法来源拒绝而非静默忽略。插件启用仍由项目 manifest 负责，不受用户同名设置覆盖。

用户项目键使用已有持久项目身份；若当前没有，采用明确版本化的规范项目位置派生策略，处理项目移动与冲突，不能用进程内 project_instance 做持久目录名。

## C3. 最少必要的角色，优先复用原类型

| 目标角色 | 处理 |
|---|---|
| SettingsDescriptor / SettingEntry | 在原配置描述和贡献目录上增补 stable ID、schema、作用域、默认值、生效策略；只有一份字段 schema |
| SettingsDocument | 文档值、schema version、未知段、来源与物理版本；不持活动 UI |
| SettingsDraft | 用户编辑的值及 based_on；复用已有 ConfigurationValue/codec，不能成为第二作者 Session |
| SettingsResolution | 固定有效值、各项来源、待重启与诊断；是结果不是服务 |
| SettingsStore | 版本化读写、候选和发布记录；使用原 WriteCoordinator／文件 backend |
| 设置实际应用者 | 窗口、字体、按键或具体插件原 owner；不要由 SettingsStore 做 GPU／窗口工作 |
| SettingsView | 注册页面、编辑草稿、显示 desired/applied/persisted，不负责唯一解析算法 |

以上是责任角色。C0 发现已有等价 provider 时直接复用。新类型只在存在独立不变量或真实生命周期时引入，禁止一角色一 DLL、一枚举一文件。

物理建议：纯设置值和描述放当前 `authoring/layout` 的准确 provider；存储与解析接线放 `activities/workspace`；设置 UI 放对应 workbench/project/settings 主题；平台观察放原 modules/platform/window；启动组装留 application。不得把所有东西塞进新 `editor/settings` 顶层。

公开逻辑 include 不随目录裁定无意义改名。不要新增全局 SettingsManager/ConfigurationManager 或中央字符串解释器。

## C4. 插件描述与载荷寿命

扩展沿现有 ContributionDraft／Snapshot 增加 settings 集合，复用同一稳定描述、CodeLease、原子批次和代码验证。不要另装一套独立插件设置注册器。

每项描述至少明确：稳定 ID、schema 版本、原 ConfigurationDescriptor／codec、默认值来源、允许作用域、纯校验、merge policy、生效方式、可选控件工厂和所属提供者代码。

字段级通用编辑器可从已有反射构建；复杂页面可以提供控件工厂。引擎层定义参数和合法性，Editor 提供存储／页面接线，底层不得 include Editor。

同一插件的动态 label、默认值、codec 和应用回调必须被同一适当 owner 保活。删除注册后尚未确认的草稿／发布／应用结果仍不能调用已卸载代码。

同一 stable ID 的不同 schema 更新需显式迁移；不同提供者同时声明同一 ID 拒绝。hash 碰撞按本阶段通用冷路径规则拒绝，不用 label 相等判断身份。

新贡献布局改变 Editor 扩展协议时，在当前 V8 后演进一个版本（当前目标 V9／lux_editor_exports_v9），同步内置扩展、外部骨骼插件、ABI 指纹和真实 SDK。运行插件 ABI、资产文件类型和脚本 ABI 不随此无关变更。若 C0 发现版本已被后继占用，采用下一个真实版本并记录，不能覆盖历史 V8 定义。

## C5. 文档读写与未知数据

沿用现有 TOML／codec／文件 backend，不另造配置语言。定义每份文档的 schema、大小／项数／嵌套上限和受支持版本。

缺文件可以使用默认值；权限、格式、checksum/版本冲突不能伪装成缺文件，不能自动覆盖损坏数据。

插件暂时缺失时保留其未知段与 opaque 值，禁止因为没有加载 editor widget 就删除。已知字段变更只替换对应节点；未修改未知节点保持值等价。若当前 codec 无法保留原注释／排版，明确承诺值保留而非逐字节保留；不要声称没做到的 round-trip。

读取新于支持的 schema 时保留原文件，提供只读／明确迁移失败，不用空默认对象覆盖。

沿用文件版本与原 WriteCoordinator 的冲突／Unknown／对账／确认；配置文件写入不是特权路径，不能直接 `ofstream` 覆盖绕过发布规则。

旧项目 `.lux/workspace/preferences.toml` 的 selected_layout 与布局／恢复关系保留。迁移到用户项目作用域要有读旧、准备新、成功标记、重复幂等与失败保留，不为了重命名 UserPreferences 一次性删除旧文件。

最近项目文档同样保留上限、绝对规范路径与用户根约束。项目移动、不存在和权限错误在 UI 显示，不能通过 hash 后丢弃原路径语义。

## C6. 草稿、持久化与实际生效

明确三项状态：desired（用户想要的值）、applied（实际已生效）、persisted（落盘确认）。不能用一个 dirty 布尔量替代全部状态。

草稿带 SettingsDocument 的来源版本和 descriptor/schema 版本。外部文件或注册变化后，提交旧草稿必须检测冲突；可显式重载／合并，不自动 rebase。

默认生效协议：准备并验证候选 → 由准确 owner 申请生效 → 记录实际结果 → 根据用户保存意图发布文档。不得承诺全部设置都可跨系统原子切换；每项描述声明 immediate、safe-point 或 restart-required。

字体与 UI scale 由原字体／UI／渲染 owner 准备与切换。首次交付可明确为重新启动生效，必须读回后真实生效；若实现 live reload，则旧 atlas／GPU 引用必须活到最后使用，不可在 draw 中销毁。

窗口状态由原平台窗口 owner 应用；实际系统可能限制位置或尺寸。保存用户意图与实际结果，不能把 OS 拒绝改成成功。

项目插件启用默认 next-project-open/restart-required。动态注册设置不等于即时重装插件。

UI 显示逐项准备失败、应用失败、已应用未保存、已保存待重启。Apply／Save／Revert 按这些真实事实工作；失败不丢草稿，取消不释放别人已接纳任务。

恢复 defaults 是构造来自 descriptor 的新草稿，不直接抹除磁盘文档或未知插件段。

## C7. 命令快捷键属于用户覆盖，不修改固定声明

CommandDescriptor 中的 shortcut 是内置默认值。用户覆盖放设置文档，以稳定 CommandId 引用；canonical identity、scope 和 input_version 不因改快捷键而改变。

在配置解析边界调用唯一 shortcut parser，得到原键／修饰符值。冲突必须有确定政策：拒绝或明确禁用冲突项并说明；不能按 map 偶然顺序赢者通吃。

插件命令缺失时保留覆盖但不激活；重新加载后按兼容的稳定身份恢复。不得保存 snapshot 内存地址或临时命令序号。

默认菜单 group、显示 label 和个人快捷键分开；翻译文本不能作为设置文件键。无需本阶段完成整个国际化系统，但不得堵死此边界。

## C8. 启动分两段，避免插件配置循环

```text
解析 Editor 启动选项
    → 确认安装／项目／用户根
    → 读取内置引导设置和项目插件选择
    → 查询显示环境（非 offscreen）
    → 得到基本窗口／字体的有效配置
    → 创建平台与必要引擎能力
    → 加载获准扩展，登记 settings 描述
    → 解释对应扩展段并验证
    → 按 owner 激活设置与工具
    → 创建完整工作台
```

哪些引擎能力必须早于读取字体等任务建立，按原实际依赖安排；不能要求还没有 ExecutionRuntime 就提交 font task，也不能为了同步读取方便阻塞错误线程。

读取 bootstrap 设置不依赖插件提供的 UI。Settings schema 未加载前保留原段，不能先激活插件才能判断它是否应该激活。

`EditorApplicationConfig` 可以保留为已解析创建输入，但不再混合“0 表示自动”“用户明确设置 0”“尚未解析”等含糊状态。使用已有 option/value 类型；必要时增加轻量 `EditorLaunchOptions`，把显式覆盖与解析结果区分。不要同时维护三份独立窗口设置。

## C9. 显示器与窗口策略

当前窗口链直接使用传入宽高，没有自动选择显示器工作区。[S22,S23] 新策略遵循：显式覆盖 > 合法保存状态 > 选定显示器环境 > 安全缺省。窗口构造只消费解析结果，不在构造内隐式读设置文件。

复用／补齐原 platform/window 的 monitor 观察接口。它返回明确坐标单位的工作区、content scale、可用显示模式及当前临时 monitor 身份。不要在 Editor 与 Launcher 各写一段 Win32/GLFW 查询。

不得把物理 framebuffer 像素直接当窗口坐标。目标显示器名字不保证唯一，GLFW 指针与枚举次序也不能持久化；保存足够的可解释提示，重启后匹配失败则回退，不猜一个有效指针。[E01,E02]

解析时至少处理：

| 场景 | 要求 |
|---|---|
| 首次启动 | 使用工作区推导有限默认矩形，比例／上限集中为一份政策 |
| 显式尺寸 | 验证单位、数值与上限；合法覆盖优先，不每帧按环境重写 |
| 保存矩形 | 恢复 ordinary-window 矩形；验证可见区域与目标 monitor |
| 原显示器断开 | 回退到可用工作区，保证主要交互区可达，不保存旧 pointer |
| 负屏幕坐标 | 正常多屏坐标，不当成非法 unsigned 数 |
| 极小工作区 | 合理裁剪；最小尺寸不可满足时明确退化，不整数下溢 |
| 装饰边框／任务栏 | 区分 content rect 与外框，不遮掉全部标题栏 |
| DPI／scale 变化 | 调整 UI／字体与输出尺寸的正确环节，不双重乘 scale |
| 最大化 | 与恢复用普通矩形分开保存；不覆盖普通矩形为最大化尺寸 |
| 全屏 | 明确一种支持模式及目标 monitor，保留恢复矩形；不要求本阶段实现任意独占刷新率编辑 |
| offscreen | 使用明确确定尺寸，不查询 monitor，不伪造物理窗口 |
| 未保存设置 | 仅本次启动覆盖，不无意写回用户配置 |

本阶段至少实现普通窗口、最大化恢复，以及原平台实际可支持的一种全屏切换；无法支持的后端明确返回 unsupported，不显示已经生效。不要新增平台特例到 application 内部。

窗口调整／monitor 事件通过原平台事件与安全点处理；不是每帧重算默认布局。持久化使用已有发布链并合并频繁事件，在退出时结清实际已接受写入。

## C10. 最小真实交付

必须接通：窗口模式／恢复矩形、字体选择、UI scale、快捷键覆盖、现有项目插件选择，以及一个真实外部插件自定义设置页。

每项不必支持即时应用，但必须有明确策略并通过保存→关闭→重开→实际应用的完整路径。不要只生成 schema 和漂亮界面而不调用实际功能。

外部骨骼插件可提供网格／骨骼显示选项或编辑习惯项；引擎功能扩展提供自己参数值与合法性，Editor 仅提供 UI/持久化接线。运行参数不引用 Editor。

无 UI 消费者必须能读取、验证、合并和发布设置。UI 消费同一 API；不能让 SettingsView 成为唯一 parser。

---

<!-- SOURCE_CHAPTER: 05_CAMERA_AND_ABSTRACTION.md -->

# EC3-D：相机、ECS 与过度封装核查

## D1. 本轮结论的边界

已确认：CameraMotion 是导航增量；CameraPose 组合原 Transform3D 与 Camera；ViewportPresentation 把它们通过 patch 写入 ECS；CameraExtraction 读取 Camera + WorldTransform3D 并构造原渲染输入。[S26–S29]

因此“ECS→渲染成立”不能证明中间没有任何冗余；但也不能由类型较多直接证明它们都是冗余。必须逐边检查真实责任、状态和失效边界。

本章要求完成定向审核与有证据的删除，不要求推翻既有相机、Runtime、Renderer 或显示输出协议。

## D2. 明确保留的角色

| 角色 | 唯一职责 | 不能变成什么 |
|---|---|---|
| CameraMotion | 一次或一帧内合并的导航意图 | 第二份活相机组件 |
| 原 CameraPose | 导航／恢复需要的相机状态值 | Renderer 直接读取的 Editor 私有权威 |
| navigateCamera | 处理原 Camera/Transform 的纯算法 | 读文件、访问 Host 或建立资源的服务 |
| SceneView / MaterialView | 本视图输入、目标绑定和状态 | 第二个 SceneRuntime 驱动者 |
| ViewportElement | 显示图像和捕获 UI 输入 | 决定场景资产或作者历史 |
| ViewportPresentation | 本视口请求实体、相机归属、输出引用与关闭 | 新 Renderer / 通用 Service locator |
| CameraExtraction | ECS 观察和渲染操作准备 | Editor 相机控制器 |
| 原 WorldTransform 更新 | 从局部／层级计算世界变换 | 第二份 Editor 私有 transform 算法 |

原 CameraPose 改名为 `ViewportCameraState`：字段仍是原 Transform3D 与 Camera，不增加平行数据。相关 navigate result、view state codec、capture/restore 和测试一次迁完；原逻辑头可保留，旧类型名不留 alias。磁盘字段与版本不因 C++ 正名自动改动。

CameraMotion 的名称和职责已合适，除非查出实际语义不一致，不需再造 Input/Request/Command 三层包装。

## D3. 相机使用模式

至少区分：编辑器自由相机、借用内容／运行相机。原 ViewportPresentation 两种 create 入口继续复用。

自由相机：其 ECS Entity 由视口请求负责创建与退休；导航只写这一实体；不进入作者 History，不保存成游戏内容。

借用相机：只引用明确的场景实体；ViewportPresentation 不因关闭而删除它。原 setCameraPose 不应偷偷写入外部相机。跟随／驾驶若要提供，是显式模式和权限，不是看到 Entity 就默认能写。

本阶段不要求新建完整相机选择 UI，但必须保留低层借用模式回归，并明确 Run 自由观察不等于游戏当前激活相机画面。

“激活”按当前每个 RenderViewAssociation 指定相机。不得退回全局单一 ActiveCamera 导致多个窗口争夺状态。

## D4. 建立一张实际调用／拥有清单

逐条记录下面链路：

```text
输入事件 → CameraMotion 合并 → navigateCamera
    → View 的期望状态 → ViewportPresentation
    → ECS patch → WorldTransform → CameraExtraction
    → RenderViewRequest/association → 原输出 → UI 采样
```

每个函数记录：调用频次、持有数据、拥有资源、分配／复制、是否有外部 callback、是否跨帧、是否改变身份、是否需要错误转换。

判定为纯转发且同时不改变抽象边界／访问资格／寿命／错误域／线程的，合并进调用者或准确 provider；迁移所有消费者并删原体。名字短、函数短或者容易内联不是单独的删除理由。

不允许为“层数少”让 UI 直接调用 RenderRuntime 的私有资源构建，不允许让 Renderer include Editor。保留使用基础设施所需的真实边界。

## D5. 期望、ECS 当前值与已显示图像

可以有不同阶段的值，但必须有清楚关系：

- 视图的持久／期望状态用于导航、窗口重建和布局恢复。
- ECS 当前相机是运行侧被抽取的事实。
- accepted/published 输出属于某个明确的 view revision、render sequence 和相机观察。

若相机期望未变，不应每帧重新编码／patch 两份组件。利用原 pending/revision，不另加一个不同步的 dirty 管理器。

如果原接口已经保证当前提交与可见图像对应，记录证明路径即可。否则，在既有 ViewportPresentation／输出观察中携带最小的相机／输出对应信息，或在不同步期间明确推迟拾取，不能默认拿最新期望解释旧图。

这不是要求保留全世界历史帧、实现新 GPU picking 或复制每帧 Registry。范围是相机姿态、输出范围、目标实例／代次的准确配对。对持续变化的 Run 几何，明确拾取采用当前仿真还是固定显示几何的已有语义，不能暗中承诺精确历史几何。

当前报告只定位了潜在失配关系，没有复现已有 bug。先用受控输出延迟与借用相机写入测试观察；如已满足要求则保留，不为制造整改成果强行加缓存。

## D6. 拾取与拖放

拾取、模型放置和 cameraRay 必须消费同一选定相机状态及 viewport extent。DPI 只在输入坐标转换的一处处理，不能在 cameraRay 内再次缩放。

输出未就绪、旧实例已退休或 view generation 已改变时，拒绝／延后原输入，不能把点击重定向到后来复用的窗口。

模型放置的作者来源戳、目标分区和 AssetReference 验证仍保留。相机优化不能将内容多分区限制悄悄放宽，也不能把缺 MeshQuery 解释成错误的默认命中。

Scene 的工作平面属于 Scene tool；通用 viewport 不需要因此认识 ModelAsset 或项目目录。

## D7. 观察者与更新顺序

必须同时验证：系统连接前已有 Camera／WorldTransform 仍进入首次抽取；运行期间 emplace/patch/erase/destroy 正确记录变化；重绑定和窗口重开不留旧关联。[S02]

所有被观察修改继续用 patch/replace。on_construct/on_destroy 只记录必要意图／句柄，不能在信号派发中销毁正在变化的实体或同池内容。延迟命令在原唯一安全点应用，排空中新加入的留到后批。

不要把异步资源就绪改为只依赖 on_construct。回执到达后仍需要原维护路径安装和确认，不能因“观察者更纯”而漏掉异步完成。

关闭需要保留输出引用、已提交渲染资源和原 retirement。减少封装不意味着取消 output evidence、surface generation、view revision、scene domain 或 frame serial 的校验。

## D8. 可以精简的检查与不能合并的检查

| 情况 | 处理 |
|---|---|
| 同一同步算法连续两次验证同一不可变参数，无回调 | 合并为一次，传真实值／引用，不重新查 ID |
| View 创建与之后运行帧分别验证相机实体 | 有不同有效期，保留 |
| 组件 patch 之后在 CameraExtraction 处理世界变换 | 输入已改变，不把前一次校验当永久许可 |
| output status、view sequence、GPU 可采样证据 | 是不同事实，不能因为字段多就删除 |
| 多次局部 projection 计算确认完全同参数同有效期 | 复用结果，记录新的生存范围 |
| 每次导航包装同样的 result/expected 但无额外语义 | 可合并薄转发，保留准确错误域 |
| borrowed camera 的非法父级／变换 | 不允许从本地 CameraPose 假定世界变换合法 |

删除每处校验都要写“首次证明点、使用点、失效事件、是否跨回调、替代约束、实际回归”。不新增可长期保存的 Unsafe/Validated 访问去绕过未来准入。

## D9. 回归范围

同源双视口各自导航／尺寸／高亮；关闭一窗另一窗存活；作者 current/dirty/history 不变；借用相机被游戏侧 patch 后原 ECS 抽取更新；系统晚连接折入存量；延迟输出时拾取不误用另一姿态；非法相机／旧代次准确拒绝；资源失败保留最后合法状态；正常与失败关闭不泄漏原引用。

旧 EC2 原生输入延期不因本章自动恢复。本章若实际修改输入坐标／导航，必须做新增针对该路径的验证并如实标记范围，不宣称完成了原来整套 OS 输入资格。

---

<!-- SOURCE_CHAPTER: 06_CODEGEN_UNIFICATION.md -->

# EC3-E：Inspector 生产生成统一到 MetaUnit／inja

## E1. 现状与范围

已读三条核心链：Render pass/operation 和 ScriptAbility 使用原 codegen job／validation／projection；Inspector 先使用同一 parser 的 MetaUnit JSON，再由 Python Generator 拼接 C++。[S31–S34]

Inspector 的 Python 不是仅测试脚本。它处理注解、类型、控件、源码发射、clang-format 和产物更新；其中作者／Run 版本还有生成后全文 replace。此为本轮明确迁移对象，不是因后缀 `.py` 就断言所有脚本不合理。

本轮统一的是生产结构代码的解析／语义／模板链。测试驱动、归档核验、依赖检查、下载、数据统计与 CMake 元配置可以继续用 Python／CMake。shader compiler、反射 parser 和 link 工具不是文本模板器，不改造成 inja。

C0 必须从所有生产 CMake 自定义命令和生成 target 建立完整 producer 清单，记录 input→semantic→emitter→output→consumer→install。已知 3 条链不是全仓统计。

## E2. 目标链路

```text
原 C++ parser 与 MetaUnit
    → 一份结构化 Inspector 语义模型与完整验证
    → inja 模板／片段
    → 作者或 Run 的显式 projection
    → 格式化／输出发布／依赖记录
    → 实际消费者编译与运行
```

禁止第二个 C++ parser、正则替代整个类型系统、Python f-string 生成生产函数体、生成后 `replace('InspectorComponent',...)`，或 C++ 先拼完整 body 再让 inja 仅输出 `{{body}}`。

保留数据／处理者／结果分工：MetaUnit 是输入数据；语义准备处理类型图／字段规则；模板是输出结构；生成产物不是语义权威。

## E3. 实现归属与工具复用

优先扩展已有 lux-cxx codegen 的 projection／validation 能力；通用 parser、MetaUnit、模板引擎不链接 UI、Editor、Runtime 或 GPU。

Inspector 专属语义归一化放 Editor 的构建工具主题。若现有生成器无法挂接所需变换，允许一个窄 host tool 复用同一 MetaUnit/inja 库；不能另立通用 codegen 框架。工具 target 不混入运行库。

模板与 support helpers 归 `editor/workbench/scene/codegen` 的实际 provider。生成代码被相应 Editor target 编译，纯组件头仍只包含字段类型与反射标记。

CMake 继续使用显式 target_files/logical_paths/marker 和原 `lux_*codegen*` 入口。不要把 output 根改成源码目录，不在 repo 中手工维护生成文件。

构建工具必须用 host 架构。若目标平台不能运行 host tool，明确 host 工具路径与版本；本阶段不新增跨平台资格声明，也不能假定所有目标二进制都能在 host 执行。

## E4. 语义模型必须保留的字段能力

从现有 Generator 与 support 代码逐项搬运，不按下表猜测删减：

| 内容 | 必须保持 |
|---|---|
| bool／整数／浮点／枚举 | 精确字段类型、原控件选择、changed 语义 |
| 字符串及固定文本 | 容量、编码和提交约束；不能用未经定义的截断 |
| 嵌套 reflected record | 正确的成员路径与叶子字段，非反射状态不得错误交换 |
| C array／std::array／vector/deque/list | 插入、删除、索引和 readonly 语义 |
| map/unordered_map/set/unordered_set | key 不可非法原地修改，集合操作与历史一致 |
| optional／variant／pair／tuple | 活跃分支、构造销毁、版本和只读控制 |
| Eigen／向量／矩阵／颜色 | 精度、排列、特殊控件和原字段语义 |
| Asset/custom widget | 实际类型与 tag，调用原自定义入口 |
| min/max/speed/step | finite、顺序、正步长、溢出及整数精确范围 |
| readonly | 覆盖完整嵌套控件和结构操作，不能只禁用外层按钮 |
| preview/commit/cancel | 原字段能力、一次领域批次、Undo/Redo |
| paused Run 编辑 | 与作者编辑显式不同接口和准入，不写作者 checkpoint |
| diagnostics | 未识别类型或注解必须失败并定位，不输出可编译但语义降级代码 |

现有 Python 使用 Decimal 等处理时，C++ 迁移不得通过 double 丢失 64-bit 整数边界。使用现有数值解析／codec 或 from_chars 的准确整数路径。不是将所有注解先转 float。

Typedef、elaborated、array 和 canonical type 的归一化应从 MetaUnit 取得确定关系。遇到缺失 IR，修补正确的输入/解析契约；不能静默假造 UnsupportedType 并给默认控件。

## E5. 模板职责

至少分开公共声明、组件实现、作者绑定、Run 绑定；片段可共享。模板参数明确 binding mode、namespace、fields capability、类型路径、只读政策和输出 stem。

差异来自结构化数据，不来自输出文本替换。可以用模板循环／条件表达重复结构，但复杂类型图遍历、循环检测和数值校验应在语义阶段完成。

合法的 C++／字符串字面量转义可用共享 callback；不能因为保留 inja 就再次手写多份转义算法。UTF-8、引号、反斜杠、换行和异常注解都要有实际负例。

模板文件名沿原工程 convention，`.template` 不必为了统一再改成 `.inja`。选择引擎和输入契约比扩展名重要。

固定格式只保留一份 style 配置。不要同时让 CMake、Python、C++ 各自内嵌一段不同的 clang-format JSON。

## E6. 输出与增量构建

声明全部 OUTPUT／BYPRODUCT：hpp、cpp、MetaUnit JSON sidecar、依赖文件、schema 或列表文件；不要只声明一个空 marker 再从旁边偷读真正变化的 JSON。

优先使用原 parser 真实 depfile。当前递归依赖 modules/engine 全部公共头是保守方案；只有真实传递依赖覆盖后才能收窄。删除广泛依赖不是自动正确的优化。

生成 input 配置、编译 flags、include 路径、宏、logical path、template、support helper、formatter 及生成器版本的变化，都要使正确步骤失效。

语义检查和模板渲染成功后再发布输出；内容不变不改时间戳。多输出全部预生成到 staging，失败不让后续编译使用新旧混合的“成功”集合。文件系统不能提供整组原子替换时，生成步骤失败且不发布成功 stamp；下次确定性重建，不声称实现了跨文件原子事务。

某个 sidecar／生成 CPP 单独被删除必须重建；不能因为 marker 未变继续使用旧文本。多个任务同名 stem 必须按 logical path／原唯一规则区分，不发生覆盖。

显式输入顺序或稳定排序使输出确定，不依赖目录遍历随机顺序、机器绝对路径、时间或 locale。路径含空格／中文应实际验证，不能只写文档支持。

统一工具迁移后同步删除 `inspector_codegen.py` 的生产调用、旧 string emitter、只服务旧脚本的 CLI 和安装文件。旧冻结 dev_log 中的脚本保持原样；测试程序可迁移调用新工具，不删除原语义断言。

## E7. 三条原生成链的边界

Render 原有七个模板文件和 ScriptAbility 的 C++/schema/Lua/native/validation projection 是复用对象，不因本轮统一重新改 ABI 或输出语义。

如果公共 codegen 修改影响它们，必须跑对应生成／编译／实际消费回归。没有变动时按准确 SHA 继承已有证据，不声称本轮重新验证全部 shader 或脚本引擎。

生产 helper 中出现 `file(GENERATE)`、配置 `.in` 或少量版本 header，不一概判成违规；逐个说明它是构建元数据、嵌入资源、语义代码还是测试夹具。不要为了数字“全部统一”把适合 CMake 的工作强行迁入 inja。

## E8. 安装消费者

新的 SDK consumer 只能使用安装的 generator、模板、support、组件头和公开 CMake 函数；不能读取源码树 pinclude、旧 build DLL 或通过源码相对路径找到 Python 脚本。

模板里可以生成 ImGui 调用，但 generator 本体不必链接 ImGui；组件 target 和运行数据 provider 不反向链接 Editor。构建工具、模板和 support 安装闭包分开列明。

修改 modules 公共头后，同步 AGENTS 指定 Debug/RelWithDebInfo/Android include 以避免 parser 读旧头；这只是头同步，不计 Android build PASS。

在实际 C++20、项目 MSVC/STL 和 codegen 输入下验证。不能只给一份本容器编译的小示例就替代安装 Inspector 的真实编辑行为。

## E9. 本章验收

完整语义正例、非法注解／类型负例、author/Run 两条真实字段能力、输出缺失重建、无关头变动不重建、相关头与 support/template 变化正确重建、第二轮无工作、安装 consumer、路径搬迁和重复 stem。

对旧／新生成器的比较以行为与稳定符号为主；格式差异可以存在，不把纯逐字节相等当作唯一标准。改 hash/stem 策略需同步消费者与删除旧产物，防止重复定义。

永久只保留一个当前生产 emitter。迁移期可在开发夹具中比较旧版本，但最终产品构建不能保留 `USE_PYTHON_GENERATOR` 回落开关。

---

<!-- SOURCE_CHAPTER: 07_QUALITY_AND_MEASUREMENT.md -->

# EC3-F：AGENTS、类型复用、复杂度与可读性

## F1. 对“复用类型”的明确理解

复用已有语义类型，不是保留不适合目标的内存布局。CommandDescriptor 原位演进成描述视图、由 Entry 保活，是复用；再创建 CommandSpec/CompiledCommand/DescriptorAdapter 是重复 schema。

相机继续使用原 Camera/Transform 数据；无需为普通导航引入三个 provider。配置值继续使用原 ConfigurationValue／codec；设置文档是来源与持久性，不是新作者数据格式。运行资产读取继续复用 EC2 的 ScriptAbility/Process/VFS，不能被 Editor 设置或命令层接管。

区分数据、准备权限、实际执行、结果观察；不是每个名词都必须有执行者对象。只读查询成员合理，拥有资源的 owner 也应承担控制行为。纯准备算法可以是自由函数。

## F2. 字符串成本与正确性

字符串比较并不天然是项目最大瓶颈，已有 hash 类型也不保证所有查找已经使用 hash。本次已确认 Registry 的 find 仍是线性名称扫描，优先改这个真实路径。

固定命令和工具角色不再运行期按文本解释。合法文本用途继续存在：显示、搜索、文件路径、配置 codec、诊断、冷注册验证以及跨目录版本重新解析。

不要将 `std::string ==` 全局替换成 hash equality；文件相同、字节相同、schema相同、命令相同是不同判据。hash 不是通用数据完整性或权限证明。

没有证据不宣称固定提速倍数。当前专项历史性能来自 P10Q 原 SHA，报告中的深链、选择同步和全快照投影数据不能当成本次 EC3 全产品成绩。[H01]

## F3. 测量五组有限问题

| 编号 | 实际路径 | 必须记录 |
|---|---|---|
| M1 | 35/256/扩展规模的命令注册与查找 | 构建耗时、业务名字 hash/compare 次数、索引容量、lookup 次数、PINNED/CURRENT 场景 |
| M2 | 静态描述、插件静态、动态工具描述 | sizeof、字符存储、Entry/回调/owner/索引分配、菜单重复文本，最终释放 |
| M3 | 稳定菜单与设置页面 | 固定 revision 下有没有重新解析 shortcut/group、重复读取文件、重建全表 |
| M4 | 导航→ECS→已显示输出 | 重复算法、patch 次数、借用／间接调用、相机输出一致性，不把 GPU wall time 当纯 C++ 成本 |
| M5 | Inspector 增量生成 | 干净生成、无变化、单字段／template／support 修改与输出删除时执行的实际 job |

M1/M2 使用真实 SDK 和实际库，不用完全不同的小类证明原实现节约内存。计数段和耗时段分开；计数改动不能冒充正式性能构建。

指定热路径目标：已解析的成功派发不做业务名称比较／重新哈希，不因描述物化复制字符；固定 revision 菜单不重新解析；相机不变时不做多余 patch；未变输入不重复 codegen。

这些目标不包含 handler 自己合理的业务工作，不包含返回错误诊断的字符串构造，不宣称整个进程零分配。

不预设 100 次硬配额或“快 20%”。使用能证明结构变化和覆盖尾部异常的有界样本，记录实际样本数、负载、编译器与条件。不得再次启动被用户停止的旧慢算法长测。

## F4. 过度封装与冗余校验登记

每个改动边界用统一字段：owner、输入、输出、必要不变量、资源寿命、调用频率、回调／线程边界、旧／新依赖、为什么保留或合并。

可删除：无语义转发、无必要类型擦除、同一有效期重复 lookup/describe、同一固定源重复编码、稳定帧无条件全量目录复制。

必须保留：外部输入、插件回调之后、异步完成、Session/History/Run/View 代次、文件真实发布、GPU 可采样与资源退休、设置外部变动后的检查。

不得用永久 ValidatedHandle、assert、忽略错误、无界重试来减少分支。不能以“单线程”否认重入和析构 callback。

## F5. 风格与小项

- 类型 UpperCamelCase；枚举 E 前缀／成员全大写；variant 别名 V 前缀；private 成员后缀下划线；方法小写驼峰。
- 控制变量用准确语义，不用 optional<bool> 表达三种业务动作，不用 label/name 充当身份。
- 长 callback 和 result 使用有意义别名，不为每个 alias 单独建头。
- 多个独立校验按 AGENTS 具名 bool 分组；先检查指针／范围／溢出，再计算依赖它们的条件，不能外提导致提前解引用。
- 数据／组件头只 include 字段类型和标注；template/inline 所需依赖精确保留，不能盲目删除必要 include。
- 只有 CPP 需要的编译器、平台、JSON 解析或渲染重头迁到实现。
- public include 安装，sinclude/pinclude 不安装；生成 support 的使用范围单独列明。
- 类型更名后全部调用和 SDK 同步，不留兼容 using 或自指 using；旧磁盘名称需要保留时由 codec 显式解释，不用 C++ alias 伪装。
- 结构化错误与退出／日志文字分开，不比较文案作业务判断；新错误使用原 error 域，不建全局 ErrorAdapter。
- 内置路径不增加 try/catch；第三方/插件/Builder/Codec/Toolchain 捕获必须在明确边界转换。禁止全局禁异常开关，也不能让错误穿 DLL/Task/Script ABI。
- 日志出口宿主装配一次，render 诊断仍走原 sink，不额外链接日志后端。
- 不为每个错误路径添加 printf，不用 assert 作为 RelWithDebInfo 必须成立的检查。
- 静态 constexpr 是描述，不在全局对象构造中注册。加载和卸载阶段显式可见。

## F6. 构建、SDK 与证据

显式当前阶段 `LUX_EDITOR_MIGRATION_STAGE=EC3` 与 `LUX_EDITOR_LAYERING_MODE=STRICT`。在原门禁中登记阶段及规则，不以 unknown stage 自动跳过旧限制。

最终全量 target all，遵守 `-j 4` 与 `-k 0`；CMake 修改后第二轮应无工作；不得并发构建与真实 DLL/GPU 运行。

Foundation/closure 先通过 ValidateTrackedSnapshot，从干净 tracked commit 建独立配置；实际用增量构建就写增量，不能当作首次全仓冷构建。

modules 公共头变化同步三个指定 include 前缀；若某前缀不可访问，记录具体缺项并确保 meta-gen 不读旧版本，不宣称同步通过。Android 默认不进入 configure/build/CTest。

新安装 SDK 必须独立消费，不能借旧 build DLL、旧生成脚本或源码 private 头。公开模板／元数据引用／ABI／DLL 代码寿命一起验证。

历史快照逐字保留；对原报告的发现新增勘误，不改以前结果。未修改路径可以按旧 SHA 继承，不记为新 SHA 重跑。

不把已免验的 P12 人工菜单观察、归档验证器项目改名为本阶段强制 smoke；本阶段新增设置、命令、相机和生成能力仍需要对应功能验证。

## F7. 文档与最终维护入口

根 README 说明 Application 的实际边界、配置来源、窗口策略和当前 Editor 扩展版本。清除当前文档中 V7/V8/V9 混写，但历史报告版本保持不变。

SDK 给出：模块静态描述注册、动态描述有界存储、冷名字解析与热 handle、无 App 项目保存、插件设置、原生窗口有效配置、两种相机入口和新 Inspector 工具的可用示例。

每个示例真实构建并使用正式 API，不在文档里发明与代码无关的 ConvenienceManager。

最终交接明确新工作区路径、HEAD、用户补丁是否应用、main 是否未改、分支处理是否获授权；不能只写“完成，工作区干净”。

---

<!-- SOURCE_CHAPTER: 08_EXECUTION_SEQUENCE.md -->

# EC3 依赖顺序、文件处置与交接

## C0：固定输入、实际依赖与逐项去向

前置：阅读 00–07、AGENTS 和 reference 原调查。记录本地／远端 SHA、原用户工作区与补丁，不修改 main。

读取当前 lux-cxx 真实源及安装头的 StableNameId、StrongId、constexpr string/hash、TypeToken、function_ref、move_only_function、SharedBytes 和可用容器；固定版本与工具 ABI。线上 bc1eab34… 只是参考，不能替代实际机器依赖。

展开 inventory/decl_* 的 50 个固定条目以及动态 tool 规则，核对新增／删除／变化。扫描 production 的 CommandDescriptor、ViewFactoryDescriptor、SessionKindDescriptor、ConfigurationDescriptor、脚本与 Feature 描述，区分已声明式、应迁、动态、正常固定协议。

记录所有生成 producer 的输入／输出／消费 target，不把全部 `.py` 认成生产 emitter。读取现有 modules UI 菜单存储、shortcut key 和 platform window API 后，裁定具体复用位置。

记录 Application 全部业务方法和真实消费者，完成 mapping：retain lifecycle / move exact algorithm / delete redundant / data-only / needs explicit composition。关闭、退出、保存、恢复的同名函数不可仅按名称归类。

产物：同一账本中的 baseline、file-actions、owner-map、declaration-map、codegen-map、coverage。输入 CSV 不是已完成结果。每个 DELETE 必须有消费者与替代证据。

出口：类型／文件／target 裁定确定；没有空目录占位；未执行功能不标 PASS。

## C1：单份描述与经过验证的数值身份

实施 02 的原 CommandDescriptor、CommandEntry 和 Snapshot 迁移。固定描述 constexpr，动态文本一个 backing，Entry 直接引用；保留 CodeLease、callback 与 weak control-block 清理顺序。

原 Snapshot 添加派生数值索引；补冷碰撞检查、跨版本重解析检查、重复 ID 与类型域错误。不要改变所有 StableNameId 的全局相等语义。

明确 primitive API：固定声明绑定、动态描述合法构造、外部文本解析、已解析 handle。删除旧 Spec/materialize 路线，删除新代码里的拥有型字符串镜像。

保持原公开逻辑 include；实际 ABI 变化记录，暂不发布半兼容 SDK。受影响消费者同批迁移。

验证：真实 constexpr 编译、静态／动态寿命、强制碰撞、PINNED/CURRENT、M1/M2 初始对比。不得在成功结果中跳过旧批次和线程检查。

## C2：完整项目保存与工作区 owner

实施 03 的 ProjectContentSaving、WorkspaceActions／原活动扩充、ProjectPluginSelection 和必要最近项目活动。优先复用现有 Operation，不新建平行状态机。

把真实业务算法与记录迁出 App；UI 决定保留在准确 view／product use case。无 App／Root 消费者完成项目源保存、manifest 登记、失败与 Unknown；WorkspaceStore 仍可无 UI 使用。

迁移 SaveAll、Close、Exit 的结果借阅／确认，不出现两个 acknowledge owner。不同用户意图的来源政策按原契约保持。

出口：Application 中相关记录只保留确实属于用户呈现／跨层用例的身份，不保留完整业务 state；原方法算法体删除。

## C3：模块贡献与中性命令路径

50 项固定描述按 inventory 目标 owner 归位；动态 tool 规则归工作台贡献组合。每个模块绑定自己的准确 receiver。

删除 installContributions 中逐 ID 选择、基础 Save erase/replace、Scene role 字符串解释、execute 和菜单完成的 save 特判。不可只是把文字换成 hash 后保留同一全知中心。

命令直接 API、程序化 facade／registry、菜单/快捷键三种入口使用同一业务实现。原 Host 菜单捕获目标、回调后批、拒绝不消费、线程分类全部保留。

将快捷键验证／解析归一，菜单引用固定描述及原 handle；定义动态文字 source owner，不在 UI 每项持一份重复文本。

出口：内置与外部骨骼插件走同一注册能力，无中央插件种类 switch，无 App::Impl 注入插件。

## C4：设置核心与动态贡献

落实作用域、SettingsDescriptor/Document/Draft/Resolution 和原 Store 的准确扩充，全部值复用 ConfigurationValue/codec。

项目插件选择接入原 manifest，不重复存储。未知插件段、schema 迁移、冲突、Unknown 和待重启结果完整处理。

在现有 ContributionDraft/Snapshot 增加设置贡献并保持原多目录批次；按实际结构演进 Editor 导出版本和 ABI，迁移所有真实插件／生成支持。原 runtime/script ABI 不做无关变更。

先完成无 UI 的读／解析／准备／保存／重开，再接界面。失败保留原文档和草稿，冷校验不偷偷激活功能。

## C5：显示环境与正式设置页面

补原 platform/window 的 monitor/workarea/scale 与 window placement；若现有接口已经存在则直接复用。解析请求、保存矩形、环境和缺省，再创建 LuxWindow。

接通普通窗口、最大化恢复、一种准确支持的全屏模式、字体、UI scale、快捷键和插件设置。不能只写 codec。

引导设置先于插件 UI；offscreen 继续明确尺寸。设置页面可编辑动态插件项，缺插件保留原值，错误不覆盖配置。

验证纯解析多屏与 DPI 夹具、当前真实 Windows 窗口事实、保存重启、插件设置实际消费。没有多显示器硬件可用时分清模型测试与真实单屏观察；不要求采购设备。

## C6：相机命名、过度封装与同步

CameraPose 正名及全部调用／codec 同步；不改原 Camera/Transform 数据和持久格式。

按调用清单标记保留／合并／删除，只删有证据的无责任包装和重复工作。保留 View/Element/ViewportPresentation/CameraExtraction 的真实资源或线程边界。

验证期望／ECS／显示输出的对应关系，补或修最小缺口，不引入全世界帧快照。模型拖放与拾取仍绑定正确 target/extent/source。

跑同源双视口、borrowed camera、延迟输出、观察者存量、失败和退休。输入延期项单独保留，不据此宣称旧 OS 全套已经完成。

## C7：Inspector inja 投影

从已读 Python 逐算法迁出语义模型；模板显式生成作者与 Run 版本。原 parser/MetaUnit/codec/字段语义保留。

CMake 完整声明 sidecar/outputs/depfile，内容未变不重写，staging 失败不发布成功结果。安装 generator/templates/support 并删除生产 Python emitter 路径。

验证所有字段种类、只读、自定义控件、Undo/Redo 与 Run 编辑；相关头、template、support、compile flags、缺失输出的增量行为。

如果改公共 codegen 基础，顺序验证 Render 与 ScriptAbility，不能把 Inspector 单个示例通过扩大为全部投影通过。

## C8：综合、性能、残留与文档

按 V 主题矩阵整合实际用例：无 App 保存、插件命令／设置、工作台恢复、窗口模式、相机和生成产物。执行有限 M1–M5 计数与测量，保留实际范围。

核对旧方法、旧声明构造、旧 emitter、旧 package/install/provider 和文档版本。不能把已删除的 App 业务体藏进别名、测试或 legacy。

AGENTS 对已改文件逐条检查；modules 头同步、生成器输入、SDK 引用与实际 target closure 同步。修实际遗漏，不增加新的 architecture stage。

## C9：固定最终实现并封存

先形成 clean tracked implementation SHA；按实际规则进行最终全量 all、第二轮、适用回归、PLAYER、安装 SDK、公开头及真实 plugin/GPU/新增窗口用例。不得并发构建与运行。

foundation/closure 用独立干净配置；旧免验／延期保持，新增未能完成项准确为 NOT_RUN/PARTIAL，不能“批准过部分就自动全部免验”。

冻结 dev_log/EC3 的收据、源码／依赖版本、工具路径、命令、退出码、行为映射、内存与计数范围；验收记录另一个提交。

正常推送后停在 EC3。报告工作区、新旧补丁状态和远端 SHA；不自动合并 main、删实施分支或发布。

## 逐文件迁移原则

`inventory/file_actions_seed.csv` 是已知路径的处置种子，不是移动脚本。实现者需补全真实文件与 target。

- 同一文件混合生命周期与业务时按符号拆迁，不能整文件盲移。
- 数据只保留一个实际定义，include/provider/安装同步；不留 forwarding header。
- 只读旧格式 loader、historical dev_log 和可复用测试继续保留。
- 每次删 API 要有新的真实消费者，不以测试跳过作为迁移完成。
- 安装包名字不随物理目录顺手变化；确需改变 ABI 接口时明确升级，拒绝旧二进制而非崩溃。

## 每批交接格式

```text
批次：C<n>
输入／输出实现 SHA：
已迁出的算法和原路径：
新增／复用类型及唯一 owner：
删除入口、剩余消费者：
本次运行（命令、环境、退出码、对应 SHA）：
继承证据（原 SHA 与理由）：
免验／延期／未测：
发现但未解决的问题：
用户补丁与 main 状态：
唯一下一入口：
```

发生真实阻塞时停在已完成批次，保留代码、日志和下一入口。不得在没有证据时宣称“所有小项已覆盖”，也不为满足阶段名悄悄删减功能。

---

<!-- SOURCE_CHAPTER: 09_VALIDATION.md -->

# EC3 验收主题 V01–V60

这些是行为与结构观察主题，不是新增 60 个测试程序。优先扩展现有真实测试及 SDK consumers。一个测试可覆盖多个主题；一个主题也可需要数个断言。

每个结论必须对应最终实现的真实证据。可控 hash、显示环境和异步延迟属于受控夹具，不冒充真实显示器/随机碰撞/全平台资格。

本阶段不要求补 EC2 延后的整套原生输入；新增修改涉及的功能需要自己的验证，未执行保持准确范围。

## V01：输入与遗留范围
批次：C0。
输入／过程：固定实际 HEAD、工作区、用户补丁及 EC2/P12 状态。
必须观察：原记录不改写；不会把 PARTIAL/延期记成 PASS。
证据：baseline、receipt、原始 patch 哈希。

## V02：依赖与声明完整性
批次：C0。
输入／过程：核对实际 lux-cxx 头/工具 ABI，并展开 50 项和动态 family。
必须观察：版本一致；每项有 owner/consumer，不宣称旧清单为全仓。
证据：安装头哈希、声明映射、target 图。

## V03：编译期固定身份
批次：C1。
输入／过程：真实 SDK 编译模块 constexpr descriptor。
必须观察：使用既有 hash/type token；不构造拥有型 ID/字符串镜像。
证据：编译命令、符号/布局记录。

## V04：重复与碰撞
批次：C1。
输入／过程：同名重复、异名同 hash、伪造 hash、错误 domain。
必须观察：不同错误可定位；候选整体不发布；旧目录不变。
证据：使用同一校验算法的可控 hash 或真实碰撞夹具。

## V05：跨版本身份
批次：C1。
输入／过程：旧 A handle 在新目录移除 A、加入同 hash B 后 CURRENT/PINNED 执行。
必须观察：PINNED 按原定义；CURRENT 不得执行 B；冷重解析明确。
证据：旧/新 entry、revision、实际 callback 计数。

## V06：数值查询与失效
批次：C1。
输入／过程：真实目录多规模命中/缺失/更换；持句柄热执行。
必须观察：热路由无业务名扫描/重算；旧指针不复用。
证据：计数段、lookup 路径、M1。

## V07：快照与批次保护
批次：C1 C3。
输入／过程：固定读取、贡献失败、旧值清理和通知内重入发布。
必须观察：保持 P11 R1 原子性；嵌套普通发布被拒绝。
证据：原真实 SDK 回归。

## V08：外部名称解析
批次：C1。
输入／过程：文件/CLI/插件动态名首次解析及错误拼写。
必须观察：解析得到固定正确 handle；未注册同 hash 名不能命中。
证据：规范名/hash/错误与调用记录。

## V09：固定描述复用
批次：C1。
输入／过程：多个同模块消费者注册固定声明。
必须观察：descriptor 指向原声明；metadata 文字不复制；实际 owner 开销公开。
证据：地址与分配/字节计数，M2。

## V10：动态描述寿命
批次：C1。
输入／过程：构造后销毁临时 string、移动/增长输入缓冲再调用。
必须观察：统一冻结 backing，所有字段正确且无悬空。
证据：真实 SDK 运行与释放计数。

## V11：插件卸载与弱句柄
批次：C1 C4。
输入／过程：旧快照/命令/动态描述/弱控制块存活时替换插件。
必须观察：文字/callback/deleter/control block 清理早于代码最终卸载。
证据：真实 DLL，不用隔离 shim。

## V12：元数据共享到菜单
批次：C3。
输入／过程：多菜单观察、目录替换、设置覆盖和旧输入存活。
必须观察：无第二业务目录；描述仍有效；显示覆盖不改 ID。
证据：实际 Root/CommandMenu 与存储计数。

## V13：静态与动态构造负例
批次：C1。
输入／过程：临时 descriptor 或短寿命借用、非法字段、缺 owner。
必须观察：编译/准入准确拒绝；不把动态值伪装成静态。
证据：真实接口正负编译/运行。

## V14：全部声明迁移
批次：C3。
输入／过程：35 个 ID/12 个 view/3 个 source，基础与项目 Save。
必须观察：各自模块声明；产品不先删后装 Save；后缀行为保留。
证据：逐项清单和真实安装目录。

## V15：Scene 工具与运行命令
批次：C3。
输入／过程：四工具、pause/resume/step/stop、未知选择。
必须观察：真实动作正确；无 role string 二次解释及默认配置回退。
证据：实际业务结果，不只检索字符串。

## V16：直接/菜单/程序化一致性
批次：C2 C3。
输入／过程：同一保存/恢复用例分别经三入口调用。
必须观察：活动只接纳一次；责任不依赖 takeCompletions。
证据：调用次数、结果 owner 与真实文件。

## V17：快捷键/分组解析
批次：C3 C5。
输入／过程：默认键、用户覆盖、非法语法、冲突、菜单多级组。
必须观察：唯一 parser；稳定事件不解析文字；冲突确定。
证据：解析正负例和 Root 路径。

## V18：动态插件窗口命令
批次：C3。
输入／过程：加载/卸载独立 ViewFactory，生成 tool/<type>。
必须观察：只在注册边界形成；动态寿命正确；不用中央枚举。
证据：实际扩展、旧队列与窗口。

## V19：无 App 项目保存
批次：C2。
输入／过程：只用正式活动打开真实作者 Session，保存并登记。
必须观察：无 Root/Application 依赖；source/checkpoint/manifest 正确。
证据：实际 IO、独立 SDK/依赖负例。

## V20：项目保存部分完成
批次：C2。
输入／过程：源成功目录失败/Unknown/被新记录更改/重试。
必须观察：保留真实源文件；不清空来源；不覆盖新 compiled。
证据：故障注入/磁盘读回/固定版本。

## V21：SaveAll 与关闭
批次：C2。
输入／过程：多视图同 Session、无视图内容、A保存 B取消。
必须观察：按 Session 去重；不提前删除；单一确认 owner。
证据：真实 SaveService/Store 活动。

## V22：工作区政策
批次：C2。
输入／过程：保存/应用/删除所选布局/回退，preferences 失败。
必须观察：Host 事实与文件事实分开；坏布局无副作用。
证据：实际 Host/文件，C01 回归。

## V23：独立 Recovery
批次：C2。
输入／过程：只选恢复清单、缺插件、部分内容显示失败。
必须观察：不扫描任意 layout opaque；部分结果精确保留。
证据：真实打开/重绑定与源/图像状态。

## V24：最近项目与插件选择
批次：C2 C4。
输入／过程：用户文档上限/去重、项目插件保存重开。
必须观察：manifest 唯一；旧活动不在 App 私有路径。
证据：文件读回、实际模块 API。

## V25：生命周期与原准入
批次：C2 C3。
输入／过程：启动连接失败、错误线程、重入、关闭有待完成工作。
必须观察：C04/WRONG_THREAD/BUSY 保留；已接受结果可靠结清。
证据：Application/SDK 定向回归。

## V26：设置作用域
批次：C4。
输入／过程：安装/项目/用户/用户项目/CLI 多来源组合。
必须观察：按 descriptor 允许范围；非法 scope 拒绝。
证据：纯解析测试与来源追踪。

## V27：设置缺省与引导
批次：C4 C5。
输入／过程：无文件启动、缺插件、插件提供额外 schema。
必须观察：不用插件 UI 才能决定加载插件；有效默认明确。
证据：启动日志/状态，不仅 schema。

## V28：配置未知段与损坏文件
批次：C4。
输入／过程：未知插件段、未来 schema、权限和格式错误。
必须观察：未知值保留；错误不覆盖为默认；保证范围真实。
证据：原/新文件与诊断。

## V29：旧偏好迁移
批次：C4。
输入／过程：现有 selected_layout/opaque，失败和重复执行。
必须观察：读旧保留、迁移幂等、Recovery 不合并错来源。
证据：磁盘版本与重复运行。

## V30：草稿冲突与 Revert
批次：C4 C5。
输入／过程：草稿编辑后外部文件/descriptor 改变。
必须观察：来源冲突准确；失败保留草稿；Revert 不删未知段。
证据：真实 values/version。

## V31：设置文件发布
批次：C4。
输入／过程：冲突、Unknown、对账、取消、退出。
必须观察：同一 WriteCoordinator；已接受完成仍可观察。
证据：实际文件与原发布回归。

## V32：动态设置插件
批次：C4 C5。
输入／过程：真实外部 DLL 注册设置、页面、codec、应用。
必须观察：无宿主类型分支；缺失后保留；卸载不悬空。
证据：实际 V9 SDK 插件。

## V33：保存与应用分离
批次：C5。
输入／过程：生效成功保存失败；保存成功待重启；准备失败。
必须观察：UI 三项事实准确，不全局回滚或伪报成功。
证据：功能 owner 实际值、文件与状态。

## V34：字体/scale 与资源
批次：C5。
输入／过程：改变字体/scale，live 或声明的重启路径。
必须观察：按策略真实生效；旧 GPU/atlas 引用不早释放。
证据：重开/真实 UI 观察；不得共享字体文件作为交付。

## V35：快捷键覆盖与插件恢复
批次：C5。
输入／过程：override、冲突、插件缺失再加载、CLI覆盖。
必须观察：稳定 ID；覆盖存活；只在变更时解析。
证据：真实命令动作/持久文件。

## V36：首次窗口策略
批次：C5。
输入／过程：工作区/scale 模型与一次实际 Windows 创建。
必须观察：使用已解析 placement，非各调用点数字。
证据：纯策略夹具与真实窗口事实分开。

## V37：保存矩形与 monitor 变化
批次：C5。
输入／过程：多屏负坐标、断开原屏、极小工作区。
必须观察：回退确定、交互可达、无越界/溢出。
证据：合成显示环境；有硬件再记实际。

## V38：窗口/Framebuffer/DPI
批次：C5。
输入／过程：不同 scale、窗口调整、输出 resize。
必须观察：单位明确、不双重缩放、GPU extent 正确。
证据：平台读回与 viewport 结果。

## V39：最大化/全屏/恢复
批次：C5。
输入／过程：模式切换/重启/失去目标屏。
必须观察：保留普通矩形；unsupported 不报已生效。
证据：实际可用后端及模型负例。

## V40：offscreen/CLI
批次：C5。
输入／过程：无 monitor 的确定尺寸、显式 font/size override。
必须观察：不查物理环境；不自动写回用户文档。
证据：无窗口消费者与文件不变。

## V41：原 ECS 相机链
批次：C6。
输入／过程：从 UI 导航到 patch 到原 CameraExtraction。
必须观察：无第二 Camera 组件/直接 Editor→Renderer 私有路。
证据：源码链+实际组件/渲染结果。

## V42：双视口与作者隔离
批次：C6。
输入／过程：同源双 View 独立相机/尺寸，关一窗。
必须观察：作者 current/history/dirty 不变；另一视图可用。
证据：实际 GPU 和身份状态。

## V43：借用相机与单写入者
批次：C6。
输入／过程：原内容相机外部 patch、free camera 导航。
必须观察：borrow 不被工具误写/删除；自由相机独立。
证据：真实 Registry/Runtime。

## V44：显示相机与拾取
批次：C6。
输入／过程：相机更新后延迟输出、old view/input 到达。
必须观察：按明确输出/相机关系取 ray 或 defer，不误投新实例。
证据：受控延迟+实际输出/输入状态。

## V45：观察者存量与销毁
批次：C6。
输入／过程：先建 Camera 再连系统、patch/erase/destroy。
必须观察：初次完整同步；不在回调修改同池；后批正确。
证据：真实 ECS 事件顺序。

## V46：失败与退休
批次：C6。
输入／过程：坏相机、资源未就绪、窗口重绑/关闭。
必须观察：保持最后合法状态；无重复释放；旧代次拒绝。
证据：原 GPU/Runtime 回归。

## V47：相机封装审计
批次：C6。
输入／过程：逐函数 owner/频次/依赖/副作用与重复算例。
必须观察：只合并无职责边界；稳定无变化不多 patch。
证据：调用清单与 M4，不按层数打分。

## V48：Inspector 语义正例
批次：C7。
输入／过程：标量/枚举/容器/嵌套/optional/variant/Eigen/custom。
必须观察：新 inja 输出真实编译和正确编辑。
证据：安装生成器+实际字段结果。

## V49：Inspector 非法输入
批次：C7。
输入／过程：未知类型/注解、数值边界、循环/缺 IR。
必须观察：精准拒绝，无静默降级/部分成功 stamp。
证据：生成错误和输出目录状态。

## V50：作者与 Run 分离
批次：C7。
输入／过程：生成同组件两种绑定实际 preview/commit/Undo。
必须观察：两条原字段能力正确，Run 不改作者 checkpoint。
证据：实际 Session 与暂停 Run。

## V51：生成增量
批次：C7。
输入／过程：无变化/改字段/template/support/flags/无关头。
必须观察：需要时重建，不需要时无工作，depfile真实。
证据：两轮日志与产物哈希/mtime。

## V52：缺失输出与失败发布
批次：C7。
输入／过程：删 sidecar/cpp、渲染/格式化/文件更新失败。
必须观察：缺项能恢复；失败不将混合产物当成功。
证据：真实构建输入/输出测试。

## V53：SDK 与 host 工具
批次：C7。
输入／过程：独立前缀，无源码相对路径，多 stem/空格中文。
必须观察：公开工具模板完整；不运行错误架构工具。
证据：安装 consumer/编译数据库。

## V54：Render/Script 回归
批次：C7。
输入／过程：公共生成设施改动时跑原相关 projection。
必须观察：保持原语义/ABI，没有反向 UI 依赖。
证据：原真实消费者，未改注明继承。

## V55：当前生产 emitter 唯一
批次：C7 C8。
输入／过程：删除 Python 生产脚本/旧 CMake/安装入口后从独立 SDK 生成。
必须观察：只用新 inja 链；旧测试/历史资料保留；无 old/new 开关。
证据：源码/target/install 清单与实际重建。

## V56：内存与名称计数
批次：C8。
输入／过程：M1/M2/M3 三种存储/热冷边界。
必须观察：实际计数支持目标，不把文本零复制说成零分配。
证据：raw samples/计数范围/类型尺寸。

## V57：过度检查与可读性
批次：C8。
输入／过程：改到的重复 lookup/expected/if/回调类型。
必须观察：证明有效期；遵守 AGENTS；不删除失效边界。
证据：函数/检查映射与对应负例。

## V58：实际依赖与删除
批次：C8。
输入／过程：App私有、UI到活动、工具到运行、旧 emitter/alias。
必须观察：非法边真实失败，修正后通过；实际安装无残留。
证据：正负配对/target/sources/install。

## V59：最终构建与支持范围
批次：C9。
输入／过程：clean tracked all/二轮/PLAYER/SDK/公共头/新用例。
必须观察：结果绑定准确 SHA；旧免验延期不扩大。
证据：命令、退出码、环境、源绑定。

## V60：最终文档和交接
批次：C9。
输入／过程：复核 issue/声明/文件处置、V9文档与工作区。
必须观察：无空 N/A；用户补丁/主分支状态明确；停在 EC3。
证据：冻结收据/唯一账本/manifest。

## 共用负例纪律

依赖负例必须是禁止边导致失败，同一夹具去除禁止边后成功；缺第三方头、脚本路径不存在或旧 ABI 链接失败不能冒充架构门禁成功。
哈希碰撞应使用同一生产校验算法的可控 hasher 或固定可验证的真实碰撞；只伪造与名称不一致的 hash 会落在非法输入分支，不能代替真正碰撞覆盖。
每个固定声明的类型版本、scope、目标捕获与结果 owner 都要检查；只比较命令数量或某个文本出现次数不是功能等价。
每个未能运行的主题写明功能是否实现、源码依据、未测原因和影响；不得把 PARTIAL、延期或用户免验统一转换成 PASS。

---

<!-- SOURCE_CHAPTER: 10_SOURCES_AND_DECISIONS.md -->

# 来源、范围与设计裁定

固定源码：`126f1b4df14316df30957208787ded9be9f9f461`。在线分支已确认未变。本文基于最近两轮源码调查、附带报告与本次对身份/注册实现的追加读取，不是新的全仓 AST 审计。

S 为当前代码/用户规范/原调查；H 为历史数据；E 为外部技术资料。章节中新增类型与流程是 EC3 目标，不表示源代码已经实现。

源文件链接固定在基线 SHA；运行时依赖特别是 lux-cxx 必须在 C0 核对实际安装版本。当前线上 main 的参考源码不替代此前机器可能使用的其他 SHA。

| 编号 | 支持内容 | 来源 |
|---|---|---|
| S01 | 附带声明式调查，13个文件人工清单 | [reference/declarative/REPORT.md](reference/declarative/REPORT.md) |
| S02 | 用户 AGENTS 原始字节 | [reference/AGENTS.original.md](reference/AGENTS.original.md) |
| S03 | 实际 find、重复检查、shortcut 校验、调用与 Batch | [editor/activities/commands/src/CommandRegistry.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/commands/src/CommandRegistry.cpp) |
| S04 | Entry、Snapshot、Handle、Batch、Dispatcher 公共契约 | [editor/activities/commands/include/lux/engine/editor/commands/CommandRegistry.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/commands/include/lux/engine/editor/commands/CommandRegistry.hpp) |
| S05 | lux-cxx 参考类型；不代表用户机器安装版本 | [https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/StableNameId.hpp](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/StableNameId.hpp) |
| S06 | 当前拥有型描述、目标与回执 | [editor/activities/commands/include/lux/engine/editor/commands/Command.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/commands/include/lux/engine/editor/commands/Command.hpp) |
| S07 | 当前菜单构建、快捷键解析、固定输入与派发 | [editor/workbench/desktop/src/CommandMenu.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/desktop/src/CommandMenu.cpp) |
| S08 | 当前产品贡献装配和 execute facade | [editor/application/src/EditorCommands.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorCommands.cpp) |
| S09 | 当前主循环、完成与退出；已核查相关区段 | [editor/application/src/EditorLifecycle.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorLifecycle.cpp) |
| S10 | 项目保存与命令实际政策 | [editor/application/src/EditorSaving.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSaving.cpp) |
| S11 | 八 role 字符串解释和 provider 常量 | [editor/application/src/EditorSceneTools.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSceneTools.cpp) |
| S12 | 内容窗口、任务与资产工具命令 | [editor/application/src/EditorContent.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorContent.cpp) |
| S13 | 项目打开/导入/最近项目和 About | [editor/application/src/EditorProjectTools.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorProjectTools.cpp) |
| S14 | 工作区政策与恢复命令 | [editor/application/src/EditorWorkspace.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorWorkspace.cpp) |
| S15 | 项目插件选择 UI 接线和发布 | [editor/application/src/EditorSettings.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSettings.cpp) |
| S16 | 项目创建命令与视图工厂 | [editor/application/src/EditorProjectCreation.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorProjectCreation.cpp) |
| S17 | 业务结果观察与视图贡献 | [editor/application/src/EditorResults.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorResults.cpp) |
| S18 | 内置源保存/History/内容/视图贡献 | [editor/application/extensions/src/BuiltinContributions.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/extensions/src/BuiltinContributions.cpp) |
| S19 | 三作者类型与源格式；完整路径见原 source CSV | [editor/activities/scene/src/SessionFactory.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/scene/src/SessionFactory.cpp) |
| S20 | 视图描述/输入/创建的当前寿命 | [editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp) |
| S21 | 当前启动参数和 Application 入口 | [editor/application/include/lux/engine/editor/application/EditorApplication.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/include/lux/engine/editor/application/EditorApplication.hpp) |
| S22 | 实际窗口与字体创建链 | [editor/application/src/EditorApplication.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorApplication.cpp) |
| S23 | 实际 GLFW 窗口创建；本次讨论已读相关段 | [modules/platform/window/src/LuxWindow.glfw.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/platform/window/src/LuxWindow.glfw.cpp) |
| S24 | 当前偏好、布局、恢复数据声明 | [editor/authoring/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/authoring/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp) |
| S25 | 当前设置 UI 主要是项目插件选择 | [editor/workbench/project/tools/src/SettingsView.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/project/tools/src/SettingsView.cpp) |
| S26 | 原 CameraPose、CameraMotion 与纯导航 API | [editor/workbench/viewport/include/lux/engine/editor/views/CameraNavigation.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/viewport/include/lux/engine/editor/views/CameraNavigation.hpp) |
| S27 | 两个 create 入口、patch、输出/退休 | [editor/workbench/viewport/src/ViewportPresentation.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/viewport/src/ViewportPresentation.cpp) |
| S28 | ECS Camera + WorldTransform 抽取 | [engine/scene/builtin_systems/render/src/CameraExtraction.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/engine/scene/builtin_systems/render/src/CameraExtraction.cpp) |
| S29 | 本地相机、拾取、放置、更新关系 | [editor/workbench/scene/src/SceneView.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/src/SceneView.cpp) |
| S30 | 通用作者/运行配置控件契约 | [editor/workbench/scene/api/include/lux/engine/editor/scene/ConfigurationEditor.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/api/include/lux/engine/editor/scene/ConfigurationEditor.hpp) |
| S31 | Inspector 当前自定义生成步骤 | [editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake) |
| S32 | Python Inspector 语义与生产字符串 emitter | [editor/workbench/scene/codegen/inspector_codegen.py](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/codegen/inspector_codegen.py) |
| S33 | Render 原模板 projection | [modules/function/render/cmake/engine_render_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/function/render/cmake/engine_render_codegen.cmake) |
| S34 | ScriptAbility 原 C++/Lua/native/schema/validation projection | [modules/function/script/core/cmake/engine_script_ability_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/function/script/core/cmake/engine_script_ability_codegen.cmake) |
| S35 | V8 扩展贡献与实际激活契约 | [editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp) |
| S36 | 上一轮 App/设置/命令调查原字节 | [reference/APPLICATION_AUDIT.original.md](reference/APPLICATION_AUDIT.original.md) |
| H01 | P10Q 历史性能；不是当前 EC3 数据 | [reference/P10Q_PERFORMANCE.original.md](reference/P10Q_PERFORMANCE.original.md) |
| E01 | 外部一手：GLFW monitor/workarea/名称寿命 | [https://www.glfw.org/docs/latest/monitor_guide.html](https://www.glfw.org/docs/latest/monitor_guide.html) |
| E02 | 外部一手：GLFW window/Framebuffer/scale | [https://www.glfw.org/docs/latest/window_guide.html](https://www.glfw.org/docs/latest/window_guide.html) |
| E03 | 外部一手：inja 项目，模板与 callback | [https://github.com/pantor/inja](https://github.com/pantor/inja) |
| E04 | 外部一手：C++ 草案 constant-expression；不是用户 SDK 编译资格 | [https://eel.is/c++draft/expr.const](https://eel.is/c++draft/expr.const) |

## 本次追加核查的确定发现

CommandRegistrySnapshot::find 逐项比较 name；快照排重是嵌套循环。原 Entry 按值保存拥有型描述。该发现来自当前实际实现，不是只根据用户对字符串效率的判断。
lux-cxx 参考 StableNameIdView 已支持编译期 hash，但 operator== 仍核对 name；StableNameId::view 使用 fromVerified。优化不能把已有通用正确性检查一律改掉。
原报告的 CommandSpec→拥有型 Descriptor 是讨论方案，本次明确被同型轻量描述+准确存储寿命方案取代。原报告字节不改，裁定写在新总指令。

## 范围限制

没有执行引擎、SDK、GPU、窗口或 codegen 功能资格；没有编译本阶段目标 C++ 示例。目录和 API 的最终消费者仍由 C0 从实际 Git 清单展开。
本包自检只验证文档结构、引用、清单关联、原始文件字节和 ZIP；它不证明目标实现正确，也不增加任何工程通过成绩。
