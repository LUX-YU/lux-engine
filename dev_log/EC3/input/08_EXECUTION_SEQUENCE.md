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
