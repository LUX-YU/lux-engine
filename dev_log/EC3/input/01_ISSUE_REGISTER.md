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
