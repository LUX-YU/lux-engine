# 构建组织与实际依赖门禁施工规格

本章规定怎样让五层从目录变成可检查的源码事实。继续扩展现有Editor架构检查与CMake File API，不建设新的通用构建分析平台。

## 1. 四张表应一致

| 表 | 最少字段 | 实际用途 |
|---|---|---|
| source/header归属 | 文件、层、角色、逻辑include、定义owner、target | 防止物理移走但旧头仍编译 |
| target声明与实例化 | SOURCES、private/public includes、实际编译依赖、模板实例化者 | 查include/生成/模板穿透 |
| link/安装闭包 | 直接及传递边、imported targets、LINK_ONLY、export/find | 防止静态链接和SDK漏声明 |
| exe/DSO装载与owner | 真实产物、运行库、共享身份/元信息位置 | 避免静态复制唯一全局状态 |

不是要求每张表一份新数据库；可以作为唯一账本和现有File API输出的不同视图。

## 2. 明确允许方向

- E0只使用基础设施与纯值。
- E1使用E0与对应纯领域能力，不依赖E2/E3/E4。
- E2策略使用自己的契约；E2具体provider实现可使用对应E1与引擎技术能力，不依赖E3/E4。
- E3通用UI不依赖具体作者模型；E3领域UI可以使用E1/E2公开契约和通用E3。
- E4组合正式能力，不能通过私有状态绕过任何层。
- engine/modules不依赖Editor；test target独立标记，不作为production边。

“workbench不依赖活动实现”具体指：不include后端私有头、不持活动内部状态、不直接选择/构造与其业务无关的文件后端；**不是说最终静态链接不能包含SaveService的函数定义。** 公共服务普通类的实现必须链接，静态库传递依赖真实存在。不能以最终exe里有FilePublication就判UI穿透，亦不能以PRIVATE名义忽略政策target里的反向include。

## 3. layer与真实闭包标签分开

每个target至少标明：`editor_layer`（E0..E4或TEST/RETAINED）、`role`（VALUE/POLICY/PROVIDER/INTERACTION/UI/COMPOSITION/TEST）、必需能力（CPU/PROCESS/TOOLCHAIN/GPU/PLATFORM）。字段可并入现有rules模型，不新增产品运行状态。

例子：
- scene_model=E1/PROVIDER/CPU；
- editor_persistence=E2/POLICY/CPU；
- editor_persistence_execution=E2/PROVIDER/PROCESS；
- editor_tasks=E2/PROVIDER/CPU+PROCESS；
- material_interaction=E3/INTERACTION/CPU；
- GraphCanvas所属widgets=E3/UI，不得引入作者ContentStamp；
- 新双视口test=TEST/INTEGRATION/GPU+PLATFORM。

不能仅按物理层全量授权，例如“E3允许UI，因此interaction也可以链接ImGui”。

## 4. 施工模式与最终模式

在现有 `rules.json` 增加 `editor_layering` 规则组，使用一份精确source/target映射。

施工期允许已列出的旧产品targets处于RETAINED。每一个旧target要有原消费者、退出P11/P12、禁止新入边。未知文件/新target默认REVIEW，不作为native leaf忽略。

L6最终模式：所有新正式target必须有层及角色；原新路径迁移完成；只有账本中的原旧产品岛允许RETAINED。不能留下“所有editor/tools/*都豁免”的粗规则，因为它会放过新代码。

阶段仍P10Q，不改变正式阶段序号。历史check_receipt按原SHA的rules执行，不因为当前层次变化改旧判定。

## 5. CMake组织步骤

1. 保留原add_component/lux_classify_target/安装工具能力；不要复制一套Editor专用宏系统。
2. 每层CMake显式列真实子领域，保证提供者先于消费者；公共契约/纯值先于实现。
3. 同主题必要的多个targets在同一CMake定义，SOURCES明列，不根目录递归收集。
4. 跨层tests延后到editor/tests/integration配置；不能把尚未定义target当普通-lfoo蒙混链接。
5. PUBLIC仅用于公共头真正暴露的依赖；PRIVATE仍记录实际closure。
6. generated source依赖指向产生文件的真实custom target；不依赖碰巧已安装的旧头或第二轮重编。
7. install exports包含静态库解析所需的传递依赖；不要删除find命令后用开发机器PATH补齐。
8. 同一target不编入两份逻辑相同的CPP；注意旧生成meta注册和新直接实现重复。

## 6. target/包名与动态库决定

物理迁移默认保持准确的原target/逻辑include/package，减少无意义兼容成本。新真实任务CPU分割可引入editor_tasks；旧tasks_ui仍编真实View，不是alias。

只有确实删除/合并旧模块时才删除其包入口；所有消费者一起改，不留下empty INTERFACE转发包。

不全局改BUILD_SHARED_LIBS。已有History/Session共享身份、metadata/反射注册、插件跨DSO需要共享owner的边界保留；内部新实现默认STATIC。禁止whole-archive、export-all-symbols解决静态注册或missing symbol。

检查来自exe和插件的身份是否同域、code/delete是否同有效期。此项本轮继承既有边界资格，不提前设计新的P11 ABI。

## 7. 真实禁止边夹具 N01–N14

每行都需“正常配置/编译 → 加边被指定规则拒绝 → 去边恢复”的记录。可扩展原test_editor_boundaries等同一个测试程序，**无需每行一个新项目模板框架**。

| ID | 人为注入 | 预期拒绝 | 正向对照 |
|---|---|---|---|
| N01 | E1 material include MaterialView或工作台私有头 | authoring_outer_dependency | 原MaterialSource/codec允许 |
| N02 | E0链接任何具体Session model | editing_domain_dependency | 原纯身份/History允许 |
| N03 | SaveService core链接三模型具体provider | persistence_policy_concrete_source | 单独SceneSaveSource target可合法链接SceneSession |
| N04 | WorkspaceStore include IViewHost/Root | activity_workbench_dependency | DockLayout/ViewInfo纯值可使用 |
| N05 | widgets include ContentStamp/SceneSession | widget_authoring_dependency | CanvasEdit/UI IDs允许 |
| N06 | TaskMonitor经tasks_ui引入Pane/ImGui | task_monitor_ui_dependency | 原Process/Object允许 |
| N07 | MaterialView使用scene_ui取得viewport | cross_tool_ui_dependency | editor_viewport允许 |
| N08 | CPU interaction直接或传递引入GUI | interaction_capability_leak | 对应authoring允许 |
| N09 | engine/modules链接任一Editor target | product_reverse_dependency | PLAYER原依赖闭包允许 |
| N10 | PUBLIC/INTERFACE或LINK_ONLY隐藏N01 | 与直接边同一规则 | 删除同一真实边恢复 |
| N11 | 模板策略头include全部后端 | policy_instantiation_leak | provider实例化CPP可以包含具体后端 |
| N12 | 新正式库include旧transition/context | new_legacy_dependency | 旧产品消费新正式API允许 |
| N13 | generated header来自旧隐藏路径 | generated_provider_mismatch | 实际新generator输出且依赖已声明 |
| N14 | 未分类新target/未解析imported依赖 | unclassified_dependency | 显式归属真实leaf及版本后通过 |

模板和header扫描要结合编译器依赖输出；仅regex看到词汇不能当语义证明。检查器无法处理的genex/imported边报告未知并定位，不默认放过。

## 8. 最小能力消费者

除原SDK矩阵外，以下可以放入现有消费者目录作为额外模式，不要求新建六个库：
- E0＋任一真实作者模型：无需workbench或编译后端开发包；
- layout纯值/plan：无Root/Provider callback；
- TaskMonitor：无Pane/ImGui的创建、observer、结果查询；
- SaveService core：用真实保存契约测试，不链接三种具体模型；
- 三个interaction：CPU层，不启动窗口；
- widgets/viewport：根据本身实际图形需求，但不带具体作者工具。

这里的“无需”是实际配置/编译/链接闭包，不是仅没调用相关构造函数。不能关闭生产功能来模拟依赖消失。

## 9. C++与平台审查

检查actual compile flags确为C++20而非默认23；无跨平台不支持的扩展关键字；Windows头、API、user32和链接参数在适当分支。文件大小写、UTF-8路径、资源生成工作目录、Windows/ELF linker format应按原资格保留。

Linux仍NOT_RUN不阻塞，但不能据Windows编译成功宣称Linux支持。目录迁移不得继续强制普通CPU test找lld-link；工具链测试只在相应能力启用时配置。
