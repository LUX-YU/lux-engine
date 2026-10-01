# L4：装配边界、根构建和旧产品退场路径

本批只整理现有组合、准确归属与文档；不会借机实现 P11 动态命令系统或 P12 完整新产品。不要把目标设计中的未来类名全部写成空实现。

## 1. application 的实施边界

允许在这里组合：原引擎上下文和执行/渲染设施、SessionStore、项目活动/目录、WriteCoordinator、保存/编译/Run 服务、TaskMonitor、DesktopShell/Host、现有静态工具工厂。

只有真正的组合/实例化 CPP 可以同时 include 具体后端和具体工具创建入口。其他层通过构造接收必要的引用/窄能力，不接收整个 application 或可遍历的服务表。

不新增：ServiceLocator、ApplicationContext::get<T>()、抽象工厂工厂、反射按名字查找任意服务、超大模板参数的 TEditorApplication。

固定生命周期、构造后必需的依赖优先引用/直接成员；确有失败可选能力才 optional/unique_ptr，并由工厂整体返回失败，不能构造半成功应用再让后续到处判空。

## 2. 当前旧 app/launcher/context 怎么处理

本设计区分 **已存在的新集成装配** 与 **尚未切换的旧产品入口**。

- 现有正式新模块集成测试继续不安装成第二产品。
- 可独立抽出的真实启动/依赖组合代码迁 application，只有被实际 consumer 使用时才建立生产 target。
- 旧 Editor/EditorImpl、EditorContext、PaneManager 的行为型代码在 P12 切换前可留原位置，明确标记旧产品岛；不整包改名为 EditorApplication。
- launcher 的原项目创建 UI 若直接依赖旧 Context，先留旧岛；可直接复用新公开契约的部分迁 workbench/project 或 application/launch。
- 不为根目录数量把旧 app/context/transition 移到 application/legacy；不添加新兼容 facade 让新链依赖旧产品。

当前这一批允许根目录仍有明确的旧入口目录。**最终五层目标不等于本批能跳过 P12 的完整切换。**

## 3. metadata/plugins 按职责拆分，不整体搬目录

每个真实类型/函数作以下四分：

| 实际行为 | 所属 |
|---|---|
| 纯配置值、作者字段描述、不需要 UI 的映射 | authoring 对应域或已存在的 engine/modules schema |
| 创建/绘制具体 UI 控件的 factory/registry | workbench 对应域或通用桌面契约 |
| 创建保存、编码、运行编辑活动的角色 | activities 对应域 |
| 调原插件装载并安装上述贡献、固定注册版本 | application/extensions |

旧 `EditorPlugin/CommandRegistration/PaneRegistration` 若仍承担待 P11 替换的完整策略，只能保留旧消费者，不能在 E0 放一个通用 EditorPluginCatalog 让所有层依赖。

真正的 DLL 装载、平台 API 仍由 engine/project/plugins 提供。新贡献的代码 owner 由现有 lease 贯穿回调、任务、payload 和 destructor；不重写 loader。

## 4. 根 CMake 的最终与过渡形态

正式部分应只配置：

```cmake
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)
# 原测试能力/BUILD_TESTING 规则保持，按真实配置进入 tests。
```

旧产品的少量受控配置可以在 root 一个显式“P11/P12 到期”段保留，或复用现有迁移 CMake 片段。**这是列举已有 targets 的施工配置，不是新的兼容层或新的产品 option。**

原则：
1. 不再在 root 逐个枚举新 tools/*/model、persistence、interaction、ui。
2. 每层负责层内真实 target 的拓扑顺序。
3. 同层 API/值先于实现；跨层集成测试最后建。
4. 若只为 target 尚未配置而出现旧顺序例外，调整构建定义，不引入 source 逆向边。
5. 不用根 GLOB_RECURSE 自动编入所有源，包括旧文件；生产 source 列表必须可审阅。
6. layer 不是五个聚合库；不得创建五个链接全部子库的 INTERFACE 总目标来掩盖实际使用。

## 5. 根文档必须同时说明三件事

`editor/README.md` 重写为正式长期入口：
- 当前已落地新五层、所有权和依赖图；
- 原产品尚未切换的事实与明确旧目录/target/消费者；
- P11/P12 的下一入口，而非声称这些未来用例已存在。

删除默认指导“Pane 拥有业务源和历史”“普通新工具都直接借旧 EditorContext”等与新目标冲突的表述；旧行为留在明确历史/旧产品小节，不与正式规则并列为两个同等方案。

更新 AGENTS/开发说明中的源路径、生成器入口、实际目标清单；历史 dev_log 文本不要全局替换。模块 README 写长期职责，施工日志仍在 .internal/dev_log。

## 6. 真实跨层流程检查

沿生产代码或正式集成装配检查以下链，不增加新流程状态机：

| 链 | 装配应保证 |
|---|---|
| 编辑 → 编译/预览 | View 的输入保留 based_on，Session 提交；编译使用冻结源，结果仍服务所有者 |
| 保存 → View 关闭 → 完成 | 关闭只移除 View，Save/Task/Write 继续由原 owner 结清 |
| Run → 停止 → 晚查单步 | Runtime 回收重实例，Run 通过原结果表晚读取直至确认 |
| 读取布局 → 纯计划 → UI准备 | 活动不知 Host，application 传值；本批不新增完整 P12 ApplyLayout |
| 项目目录替换 → 多 View | Model 只一份数组/索引/修订，通知作为刷新提示，读取仍核对版本 |

旧 C01/C03/C04 不改判定，也不新造同编号豁免。

## 7. L4 出口

根 CMake 与 actual graph 能对应五层；新正式路径不会落回旧 Context。应用不提供通用服务查找。所有暂留旧文件有实际消费者与 P11/P12 期限；没有空目录、空模块和“未来再填”的默认成功函数。

无需立刻把所有旧产品目录迁到最终五层，但长期新源码的去向必须全部落实；不能以等 P12 为由继续把 TaskMonitor 留在 UI 包或把新模型放旧工具目录。
