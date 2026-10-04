# 相机、命令声明、窗口策略与代码生成统一调查

## 0. 范围与阅读方式

审阅基线：`126f1b4df14316df30957208787ded9be9f9f461`，分支 `codex/editor-redesign-v4`。本文件是调查和设计讨论，不是获授权的下一阶段施工账本；没有修改任何仓库文件，也不改变 EC2 的 PARTIAL 或延期资格。

来源索引见文末。`[Sxx]` 是固定 Git 源码；`[Exx]` 是单独标明的库定义／外部技术资料。旧 P10/P10Q 报告只解释历史，不作为当前实现的替代证据。

本轮能够量化的是 13 个已读生产注册文件中的声明集合，不是全仓每一个字符串、注册器和生成器的穷尽统计。相机链路、窗口链路和三个核心生成链另外核对。

## 1. 结论总览

| 问题 | 源码支持的结论 | 建议方向 |
|---|---|---|
| CameraPose/CameraMotion 与 ECS 冲突吗 | 没有直接冲突；前者是原 Transform3D 和 Camera 的值组合，后者是输入增量；最终相机仍由 ECS 抽取 | 明确编辑器相机与内容相机，统一事实来源和同步有效期，不删除 ECS 路径 |
| 是否两套 Command 系统 | 一套业务命令 Registry，UI/程序化两条入口；具体接收者中还有字符串分派和 save 回执特判 | 分开声明、运行绑定与领域处理，消除多余的二次字符串路由 |
| 尺寸是否自动按显示器 | 当前正式 create 将传入尺寸直接交给 LuxWindow，后者直接交给 GLFW | 启动策略先解析用户覆盖／保存状态／显示器工作区，构造消费已解析结果 |
| 可以声明式整理多少 | 本次范围为 35 个固定命令 ID、12 个视图描述、3 个作者类型／格式描述组 | 模块内集中、跨模块汇总；不建立所有功能共同依赖的巨型常量头 |
| Python 是否违背统一生成 | Render/ScriptAbility 走共同 MetaUnit+模板，Inspector 在共同解析之后另用 Python 拼接 C++ | 保留共同解析器和已验证语义，将生产发射模板化；测试/编排脚本不按后缀误删 |

## 2. 相机：状态输入不是另一套渲染事实

### 2.1 实际类型

`CameraMotion` 保存 local_translation、angular_delta、pan_delta、dolly；它描述一次导航输入，不是场景组件，也不持有相机实体。`CameraPose` 直接组合 `simulation::ecs::Transform3D` 与 `scene::Camera`。[S01]

因此后者既含姿态又含投影参数，名称不完全精准。以后改到相关接口时可以考虑 `ViewportCameraState`，但这是命名建议，不是必须马上新增一套组件。原 Transform3D/Camera 不应复制到 Editor 命名空间另行定义。

### 2.2 当前真实链路

```text
鼠标/键盘 → CameraMotion
       → navigateCamera(Transform3D, Camera, motion)
       → 视图本地 CameraPose（期望/持久视图状态）
       → ViewportPresentation::setCameraPose
       → Registry.patch<Transform3D> 与 patch<Camera>
       → WorldTransform3D
       → CameraExtraction 按 RenderViewAssociations 读取
       → ViewCameraUpdatePayload
       → 原 Renderer
```

`ViewportPresentation` 有两个真实入口：借用既有 Camera Entity；或者创建本视口的 request Entity，并在其上 emplace Transform3D、Camera、RenderViewRequest。`setCameraPose` 限制只能更新本对象创建的 camera，不直接修改任意借用的内容相机。[S02]

`CameraExtraction` 读取的就是 Registry 中的 `Camera + WorldTransform3D`，每个活动 view association 指定自己的 camera。它不是读取 Editor 的 CameraPose 成员，更不是 Renderer 直接遍历 Editor 数据。观察者记录变化，prepare 中从 ECS 构造渲染输入；初始 dirty 为 true，首次完整同步不只依赖后续 on_construct。[S03]

“激活相机”在当前链路中主要是每个 View 的关联选择，不应简化成全局唯一 ActiveCamera。一个场景可以有多个输出、多个编辑器观察视角。

### 2.3 需要保持的界限

- 内容相机属于作者/运行世界；编辑器自由观察相机属于本视口的运行期辅助对象。两者都可以使用同一套 ECS 组件，不必共享同一个实体。
- 编辑器相机参数用于重建视口、导航和布局恢复，不应因此修改作者 History 或保存内容。
- 摄像机跟随/驾驶是显式模式。不可让自由导航、游戏脚本和内容动画同时争写同一 Entity 的姿态。
- Game Camera 预览与 Run 的自由观察窗口不是同一个功能。后者目前创建自己的视口相机，不能据此宣称自动跟随游戏摄像机。[S04]
- 本地期望状态、ECS 当前状态、实际已显示帧之间应有明确同步关系。当前拾取由 SceneView.state.camera 构造射线，而渲染读取 ECS；未来支持外部相机驱动/延迟采用时，需要验证它们不会用不同姿态解释同一个可见画面。[S04]

最后一项是需要验证的契约，不是本轮已经复现拾取错误。不要为消除两份值而让多视口共同修改作者相机。

## 3. 命令：一个 Registry，三类不同的“写死”

### 3.1 三种代码不能混为一谈

1. 固定描述：ID、名称、菜单组、默认快捷键、目标范围。存在内置常量是正常的，问题是分散、重复和混入实现。
2. 命令绑定：把一个已知命令的 query/execute 接到一个实际 receiver。它必须存在，但可以与描述分开。
3. 命令执行之后再次比较字符串以挑选业务：例如 role=="pause"、AcceptedOperation.kind=="save"。这不是描述数据，单纯搬到 static 表不会解决责任问题。

当前 `CommandRegistry` 是实际调用核心；菜单通过 CommandMenu/CommandDispatcher 进入，程序化入口经 EditorApplication::execute 进入。UI 的 Command 是输入/显示协议，不是第二个保存执行器。[S05–S08]

### 3.2 具体二次分派

`installSceneCommands` 循环八个 role，构造 ID 后又把 role string 捕获进 execute；随后用字符串区分四类 SceneTool 与 pause/resume/step/stop。`showSceneTool` 再将字符串映射到已有 ESceneTool，未知 role 默认落到 CONFIGURATION。[S11]

宜将 SceneTool 声明与 RunControl 声明分开，直接绑定已有枚举或具名 handler。四个工具和四个运行控制本来是两类 receiver；不应为了一个循环而用字符串把它们重新捏成通用处理器。

`builtinSessionCommands` 的 Undo/Redo 可以使用两个具名条目共享原算法。Save 三模式可以使用 `{描述, ESaveMode}` 的固定表。保留必要的业务枚举分支，没有必要让所有模式都变成插件接口。[S10, S17]

Application::execute 和菜单完成收集又特判 `AcceptedOperation.kind == "save"`。这是已讨论的通用入口与业务责任混合，不表示另一套 CommandRegistry。迁移声明时不能复制这项特判到新的表解释器。[S06,S08]

## 4. 声明清单：数量与计算口径

本轮人工展开有限循环，保留定义位置后对稳定 ID 去重。

| 生产文件（application/src 下，除注明） | 固定命令定义路径数 |
|---|---:|
| EditorCommands.cpp | 1 |
| EditorContent.cpp | 4 |
| EditorSaving.cpp | 5 |
| EditorSceneTools.cpp | 10 |
| EditorProjectTools.cpp | 5 |
| EditorWorkspace.cpp | 3 |
| EditorSettings.cpp | 1 |
| EditorProjectCreation.cpp | 1 |
| EditorResults.cpp | 1 |
| application/extensions/src/BuiltinContributions.cpp | 5 |
| 合计 | 36 |

`lux.editor.save` 在基础源保存与产品项目保存中各有一份定义；产品装配明确删除基础条目再装项目版本，所以唯一固定 ID 为 **35**，不是断言运行中重复注册了两份 Save。[S06,S10,S17]

另有动态规则 `lux.editor.tool/<ViewTypeId>`，依据插件的独立窗口生成，数量由实际贡献决定，不计入 35。合理的动态规则应继续保留。[S06]

另核对了 **12** 个固定 ViewFactory 描述，以及 Scene/Material/Flow 的 **3** 组 SessionKindDescriptor + SourceAuthoring。后者来自另外三个文件，总声明文件范围为 **13** 个。[S06,S12–S20]

所以本轮可以给出的量化清单为 **50 项固定元数据条目**；它们不是 50 个功能缺陷，也不是全仓总数。

逐条 ID、label、group、scope、后缀与固定源码链接分别见：

- [command_declarations.csv](command_declarations.csv)
- [view_declarations.csv](view_declarations.csv)
- [source_declarations.csv](source_declarations.csv)
- [inventory.json](inventory.json)

### 4.1 可以采取的整理

- 元数据按具体功能模块集中声明。模块组合点统一安装，不把所有模块挤进一个全局头。
- ID、显示名称、默认快捷键、scope、输入版本和参数类型一起声明；不要只抽走四个字符串却留下不一致的 scope/argument_type。
- 视图类型与 Pane 类型使用同一稳定常量；窗口标题允许用户覆盖，不强制它与菜单 label 完全一样。
- 作者种类、源规范名、版本、默认后缀放在对应内容 provider 的同一声明中。已有源类型常量能复用的直接复用，不另建事实表。
- Scene 的 discovery hint `luxscene` 和保存后缀 `.scene` 等当前差异只记录，不凭外观自动改磁盘与兼容行为。[S18–S20]
- 插件可以在自己的编译单元声明并提供条目；宿主不得因此新增中央枚举或编辑器类型 switch。

### 4.2 已经做对的声明，不计作新问题

- `sceneConfigurationInputs()` 已有五条 `static constexpr SceneProviderOption` 表。
- `cameraRenderFeatureBinding()` 已有 constexpr component observation 表。
- 原 Render 的 Description、operation/pass 标注与代码生成描述继续是可复用基建。

应以它们为参考，不能重新建立第二份 provider、反射或 Feature 目录。[S03,S11,S26]

### 4.3 不应机械声明式化的内容

- 真实运行错误、资源 ID、文件路径和结果状态需要运行期格式化。
- 捕获了服务和会话状态的 query/execute 不是 constexpr 对象。
- 固定 variant 的穷尽处理、保存模式和生命周期转换仍可以是普通 switch/visit。
- 不需要给每个日志句子、每个临时按钮创建一个公共常量。
- 不靠大宏、字符串函数名或新的配置解释器执行业务逻辑。

## 5. constexpr 应如何落地

当前 `CommandDescriptor` 拥有 CommandId 和三个 std::string。所核对 lux-cxx 定义中的拥有型 StableNameId 构造也不是 constexpr；借用型 StableNameIdView 则支持 constexpr。[S05,E01]

C++20 支持一部分编译期分配，但不能因此假定需要持久堆分配的拥有型常量可直接跨到运行期；也不能反过来错误地说所有 std::string 用法都不可能 constexpr。[E05]

有两种简单路径：

### 路径 A：保留现有类型，使用 module-local static const

```cpp
// 位于实际功能模块的 .cpp 内；不是启动时自注册。
const commands::CommandDescriptor exit_command{
    commands::CommandId{"lux.editor.exit"}, "Exit", "File", "Alt+X"
};
```

它已经分离声明和绑定，但依然有运行期拥有型对象初始化。应在明确的装配阶段调用注册器，不用静态对象构造函数偷偷注册。

### 路径 B：纯 literal 规格，装配时产生原拥有型描述

以下为目标签名示意，不是当前仓库已存在接口：

```cpp
struct CommandSpec final
{
    std::string_view id;
    std::string_view label;
    std::string_view group;
    std::string_view shortcut;
    commands::ECommandScope scope{commands::ECommandScope::APPLICATION};
    std::uint32_t input_version{1};
    cxx::TypeToken argument_type;
};

inline constexpr CommandSpec exit_command{
    "lux.editor.exit", "Exit", "File", "Alt+X"
};

// 只在注册/装配边界物化；既有 CommandEntry 保持真正的拥有与代码寿命。
auto descriptor = materializeCommand(exit_command);
auto entry = makeCommandEntry(descriptor, query_exit, execute_exit);
```

迁移时核对 TypeToken 和所在依赖版本的 constexpr 条件；也可由类型化绑定函数补齐参数契约。简例不能变成静默丢弃 input_version/argument_type 的实现。

采用这条路径的目的不是多一层转发，而是将常量规格与动态 callback/owner 分开；执行时仍是原 CommandRegistry 到实际 callable，不新增一次运行期查表转发。纯规格可以通过 consteval 校验固定集合的 ID 唯一性；插件组合仍必须执行原运行期重复/ABI/容量检查。

不能把运行期 Descriptor 的所有字段改成 string_view：插件可卸载、label 可动态生成、快照可越过发布批次。正确做法是原快照拥有值或明确保活其存储与代码。模板和常量不会替代这些契约。

## 6. 窗口初始尺寸：请求值、环境与已解析结果

### 6.1 当前实现

EditorApplicationConfig 默认 1440×900；create 校验后将配置直接交给 LuxWindow。LuxWindow 的 init 将同一 width/height 交给 glfwCreateWindow，monitor 参数为 nullptr。已读链路没有显示器工作区查询，也没有先恢复上次窗口矩形。[S21–S23]

所以当前不是自动按显示器生成尺寸。系统窗口管理器可能调整最终外观，但这不能等价为应用实现了自适应启动策略。

### 6.2 建议的政策（不是现有行为）

```text
显式启动覆盖
    → 已保存且在当前显示环境中仍合法的窗口状态
    → 选定显示器的工作区与缩放
    → 安全缺省值
    → 得到有效 WindowPlacement
    → 构造窗口
```

窗口尺寸、framebuffer 像素和字体/UI缩放是不同单位。GLFW 官方区分 screen coordinates、framebuffer pixels 与 content scale；显示器工作区还应排除任务栏等占用。[E03,E04]

因此，不建议简单把“显示器像素宽×80%”赋给所有平台上的 UI width。更应处理：可用工作区、装饰边框、DPI、多显示器、已断开的原显示器、最大化与全屏的区别，以及无窗口测试的确定尺寸。

监视器查询属于原平台窗口层；选择优先级属于启动/设置政策；构造函数只消费已解析结果。不需要 Window 构造时暗中读取用户配置，也不需要在每一帧重算默认宽高。

## 7. 代码生成：已读三条核心生产链

| 主题 | 当前解析与发射 | 判断 |
|---|---|---|
| Render pass/operation | lux-cxx MetaUnit，原 codegen job + 多个 .template projection | 已走共同模板机制；描述与 client/backend 等产物分开 |
| ScriptAbility | 同一 parser，validation + C++/schema/Lua/native projection | 已走共同模板机制，不需要重写 |
| Editor Inspector | 同一 parser 先导出 IR sidecar，Python Generator 再拼接 C++ | 实际发射机制未统一，不能当作纯测试脚本 |

Render 相关目录读取到七个模板文件；ScriptAbility 是四个输出投影加独立验证模板。这个统计是主题内文件/投影，不是整个仓库生成器总数。[S26,S27]

### 7.1 Inspector 的实际问题

- `Generator.body()/draw_function()` 通过 Python 字符串和 f-string 生成 C++ 控件与分支。
- 注解校验、类型归一化、控件选择、C++ 发射、格式化和产物更新混在一条脚本中。
- 已生成文本再通过 replace 改 InspectorView/namespace/InspectorComponent 来产生 Run 版本。
- CMake 先运行公共 MetaUnit 生成，然后运行这个 Python 生产步骤；所以“parser 统一了”不等于“代码生成投影也统一了”。[S24,S25]

Python 并没有重新解析整份 C++ 源码；它消费原 parser 的 JSON。修正时不要再写第二个 Clang parser，也不要抹掉脚本中已经验证过的字段语义。

### 7.2 建议的统一链路

```text
现有 C++ 解析器 / MetaUnit
    → 领域语义归一化与完整验证
    → 明确的模板输入
    → inja 模板及复用片段
    → 作者/运行 Inspector 的显式 projection
    → 格式化、内容不变不重写、明确输出和依赖
```

复杂的图遍历、类型解析、范围计算放在规范化/验证实现中；模板负责输出结构。不将算法全部改写成复杂模板语言，也不在 C++ helper 中先拼好完整函数、再让 inja 只打印 `{{body}}`，那只是形式统一。

作者和 Run 的差别应当是明确的 binding mode / fields / namespace 参数；不依赖对已生成 C++ 进行全文替换。

### 7.3 统一不等于删除所有 Python

测试、验收哈希、依赖负例、结果比较和构建编排可以继续使用 Python。本轮用户要求应理解为生产语义发射的一致性，不应删除旧冻结证据或把测试语言当成生产架构违规。

若决定不再允许 Python 参与生产语义归一化，则将必要算法迁入现有 C++ codegen 设施的准确扩展位置，不额外创造一套通用生成框架。公共 parser/MetaUnit 不能因一个 Editor 投影而依赖 UI 或 Runtime。[E02,S28]

### 7.4 不能丢的工程语义

现有 Inspector 脚本先完成语义校验才开始输出、内容未变不重写，并处理 sidecar 删除后的恢复。CMake 还追踪公共输入、compile_commands、support headers 和 formatter。[S24,S25]

其中广泛递归依赖 modules/engine public headers，说明增量依赖仍有收敛空间。改成模板后应由原生成器显式声明 JSON sidecar/outputs/byproducts 和可用的真实传递依赖，而不是简单删掉依赖来“加快构建”。

必须保留嵌套结构、容器、optional/variant、Eigen、自定义控件、只读限制、作者提交和暂停 Run 编辑的区别。生成成功仍需真实编译、安装消费者以及删除重建/无工作第二轮检查；不能只比输出文本。

## 8. AGENTS 对本轮判断的实际影响

- 相机组件继续只持数据；行为依赖数据，不反向在 Camera/Transform 头引入 Editor。[S28]
- 对 ECS 的可观察修改继续使用 patch；不要为了同步本地 CameraPose 直接写 get<T>().field。
- constexpr 规格仍遵守类型 CamelCase、成员 snake_case；不会为集中声明新增兼容 namespace alias。
- 静态元数据不是启动期自注册器，不隐式引入全局顺序和析构问题。
- parser/模板验证失败属于生成工具错误，不把 Python 的工具层 exception 与生产 Runtime 的 semantic throw 混为一谈。
- 历史文档、旧失败证据与延期范围不变；本次调查不另建唯一施工状态的竞争账本。

## 9. 推荐的决策顺序

1. 先确认相机的两种使用模式与同步边界；现有 ECS 路径不需推倒重来。
2. 用固定清单完成命令/视图/作者格式声明归组；动态回调仍绑定实际能力。
3. 对八个 Scene role 的二次字符串分派，以及两处 save 回执特判，分别做准确的语义收敛；不能用“抽常量”冒充完成。
4. 在启动设置工作中增加 monitor-aware placement，而不是在构造里继续追加隐式政策。
5. 统一 Inspector 发射到共同模板管线，并保留其所有实际行为和构建输出契约。

以上是可独立裁定的工作，不要求再重排五层，也不假定已经获得实施授权。

## 10. 验证边界

本轮只核查固定源码、构建脚本和声明关系。没有实际编译 CommandSpec 示例，没有在当前 SDK 编译用户的 constexpr 表达式，没有执行显示器/原生输入/GPU/性能测试，也没有生成全仓 AST 清单。

本包检查 CSV 唯一性与计数、JSON 读取、相对链接和 ZIP 完整性。它们是文档自检，不是引擎资格。源代码、实现分支、main、用户修改和历史验收记录均未变更。

## 来源索引

- **S01** [editor/workbench/viewport/include/lux/engine/editor/views/CameraNavigation.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/viewport/include/lux/engine/editor/views/CameraNavigation.hpp)
- **S02** [editor/workbench/viewport/src/ViewportPresentation.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/viewport/src/ViewportPresentation.cpp)
- **S03** [engine/scene/builtin_systems/render/src/CameraExtraction.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/engine/scene/builtin_systems/render/src/CameraExtraction.cpp)
- **S04** [editor/workbench/scene/src/SceneView.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/src/SceneView.cpp)
- **S05** [editor/activities/commands/include/lux/engine/editor/commands/Command.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/commands/include/lux/engine/editor/commands/Command.hpp)
- **S06** [editor/application/src/EditorCommands.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorCommands.cpp)
- **S07** [editor/workbench/desktop/src/CommandMenu.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/desktop/src/CommandMenu.cpp)
- **S08** [editor/application/src/EditorLifecycle.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorLifecycle.cpp)
- **S09** [editor/application/src/EditorContent.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorContent.cpp)
- **S10** [editor/application/src/EditorSaving.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSaving.cpp)
- **S11** [editor/application/src/EditorSceneTools.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSceneTools.cpp)
- **S12** [editor/application/src/EditorProjectTools.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorProjectTools.cpp)
- **S13** [editor/application/src/EditorWorkspace.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorWorkspace.cpp)
- **S14** [editor/application/src/EditorSettings.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorSettings.cpp)
- **S15** [editor/application/src/EditorProjectCreation.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorProjectCreation.cpp)
- **S16** [editor/application/src/EditorResults.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorResults.cpp)
- **S17** [editor/application/extensions/src/BuiltinContributions.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/extensions/src/BuiltinContributions.cpp)
- **S18** [editor/activities/scene/src/SessionFactory.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/scene/src/SessionFactory.cpp)
- **S19** [editor/activities/material/src/SessionFactory.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/material/src/SessionFactory.cpp)
- **S20** [editor/activities/flow/src/SessionFactory.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/activities/flow/src/SessionFactory.cpp)
- **S21** [editor/application/include/lux/engine/editor/application/EditorApplication.hpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/include/lux/engine/editor/application/EditorApplication.hpp)
- **S22** [editor/application/src/EditorApplication.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/application/src/EditorApplication.cpp)
- **S23** [modules/platform/window/src/LuxWindow.glfw.cpp](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/platform/window/src/LuxWindow.glfw.cpp)
- **S24** [editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake)
- **S25** [editor/workbench/scene/codegen/inspector_codegen.py](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/editor/workbench/scene/codegen/inspector_codegen.py)
- **S26** [modules/function/render/cmake/engine_render_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/function/render/cmake/engine_render_codegen.cmake)
- **S27** [modules/function/script/core/cmake/engine_script_ability_codegen.cmake](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/modules/function/script/core/cmake/engine_script_ability_codegen.cmake)
- **S28** [AGENTS.md](https://github.com/LUX-YU/lux-engine/blob/126f1b4df14316df30957208787ded9be9f9f461/AGENTS.md)
- **E01** [lux-cxx StableNameId/StableNameIdView，固定参考版本；不是当前机器安装版本资格](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/core/include/lux/cxx/core/StableNameId.hpp)
- **E02** [lux-cxx 反射生成文档：MetaUnit 与 inja](https://github.com/LUX-YU/lux-cxx/blob/bc1eab34b83b5cf8821d6319e5b2a02574dc91fd/reflection/docs/codegen.md)
- **E03** [GLFW 官方 Monitor guide：工作区、显示器与缩放](https://www.glfw.org/docs/latest/monitor_guide.html)
- **E04** [GLFW 官方 Window guide：窗口坐标、framebuffer 与内容缩放](https://www.glfw.org/docs/latest/window)
- **E05** [WG21 P0784R7：C++20 constexpr allocation 的范围](https://open-std.org/jtc1/sc22/wg21/docs/papers/2019/p0784r7.html)
