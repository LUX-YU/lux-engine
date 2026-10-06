# Framework v2 Final Convergence 交付记录

本轮完成 API 收敛及下列 Windows 验证，停在 **Framework v2 独立复审**。
没有引入 V3，没有迁入 Scene／Material／Flow 编辑工具。
`SceneToolRegistrar` 仍为 provisional；首次真实 SceneSession/SceneToolSet 设计前须重新审查选择语义。
本记录不宣布未验证平台或该 provisional 接口已经冻结。

## 版本与取证边界

| 项目 | 实际版本／位置 |
|---|---|
| 审阅基线 | `493cefe07fde6320700993c6e00298d9cbc1ae67` |
| 最终实现 | `4988357269271ae64433d97a9e783275a5ff1d66` |
| 成功生产冷构建 | `6e19e87fbb6c1bf6a8ab61b551b12e451cb1348d` |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| lux-cxx | `cf14ab1de6b2b96b56531a4de8bfa02efb4d7ca1`，本轮未修改 |
| lux-cmake-toolset | `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f` |
| imgui | `7524d14a14ab68b20ac4732db398569482a7096b` |
| 新安装 SDK | `E:/SyncForder/CodeRepos/install/Framework-v2-convergence` |
| 隔离依赖前缀 | `E:/SyncForder/CodeRepos/install/Framework-v2-dependencies` |

`6e19e87f` 到最终实现仅修改 `cmake/installed-consumers/object-core/main.cpp`，
把消费者的旧 parent 构造迁为显式 `addChild`。生产源码、CMake 和公共头完全相同；
差异核验见外部 `final-convergence/production-equivalence.json`。
不将前者的首轮冷构建宣称为后者的新运行。最终 HEAD 重新执行 clean tracked 检查、
Editor 全量／二次无工作／完整 CTest、SDK 构建／测试／重装；PLAYER 冷构建绑定最终 HEAD。
本记录另行提交，记录提交没有生产修改。

## 完成的职责与删除项

| 范围 | 最终合同及唯一责任 | 删除／收紧 |
|---|---|---|
| Error | 模块声明 constexpr ID，装配登记完整描述；失败只构造 ID＋参数 | `makeError(descriptor)` 和失败点重复登记；Render 在原注册准入处固定稳定身份 |
| OOM | 普通 heap 耗尽终止；expected 表达语义、明确容量或外部失败 | Object/UI 伪可恢复分配错误；未重写其他领域的既有本地错误体系 |
| Object／Element | 默认离树构造；父链非拥有；受保护的 UI reparent | `LuxObject(parent)`、`ElementId`、强制字符串控件身份及 sibling 字符串查重 |
| Root ownership | 原稀疏容器唯一拥有 Pane；枚举只借用 `Pane&` | public Pane ID 查询／聚焦入口、`withPane`、`panes()` ownership span、`showPanes` |
| Docking | 唯一 DockTree；同步传 Pane 引用，内部转运行身份；完整准备后提交 | DockLayout、公开 PreparedDockTree 与两套提交协议 |
| 菜单 | MenuItem 自有 CommandId／文字；保留实际目标锁定 | `menu_source`、source pointer/index 生命周期协议 |
| Root frame | update 自动采用上一批结构动作；维护与绘制重载明确区分 | 公开 applyPendingChanges/hasPendingChanges；宿主漏调用风险 |
| Root 实现 | 一份 Impl，按菜单、焦点、布局、停靠、输入、变更分组 | 约 2k 行单文件拆为七个实现单元；没有新增 Manager |
| Context／工厂 | create 完成装配再私有冻结；准确 const 访问；原 vector Registrar | public freeze、Context inline Runtime 检查、跨组件 create 声明／实现 |
| 项目值 | EditorLayout.hpp 提供 PaneDescription／EditorLayout | Registrar 头对布局 DTO 的所有权 |
| 产品循环 | frame 返回 EFrameStatus；保留实际 embedding 使用的 window/engine/context | LuxEngine.uiRoot/capturedFrames 重复或测试入口 |
| UI transport | EditorUiScene 归 app 私有，借用 EngineContext；publishFrame | 已安装 EditorUiScene／UiFrame／UiRenderSyncStage；公开 WindowInput |

`createPane` 的声明与实现都由 `lux_editor_ui` 提供；`lux_editor_context` 只登记工厂。
冻结后的可变工厂查找为私有，UI 执行入口通过窄 friend 使用。
FrameworkResult 只包含 Error 值头。Context、UI、app 保留三个真实 STATIC target，未新增库。

Pane 单内容根及 Element 子链仍由实际成员／智能指针管理。
移动内容先完成验证与原交互结束，失败保留原树；提交统一更新 Pane 缓存、布局与输入目标。
清理名称区分 `clearContent`、`clearElements` 和 LuxObject 的 `clearChildren`。
控件 ImGui 身份改用 ObjectId；PaneId 留在内部注册账务，不用于产品持久布局。

Window、Root、EngineContext 在项目切换时保持原实例；UI 先于服务释放。
Process、SceneRuntime、共享 allocation 回收、CodeLease 和 GPU 退休算法保留原 owner。
没有恢复 legacy 构建、兼容 alias、旧 ABI 回落或全局禁异常。

## 实际验证

使用 MSVC、C++20、RelWithDebInfo、无 RTTI；完整构建均为 `all -j 4 -- -k 0`。
构建和 GPU 串行执行。测试可执行文件保留断言；以下不是只按测试数判定的资格。

| 验证 | 实际结果 | 外部记录 |
|---|---|---|
| 真实旧 SDK 负例 | Context-only create 链接失败；error 首次失败才登记；UI reparent 被拒绝 | `fc0-before-all/behavior`，原输出未改写 |
| 独立生产冷构建 | 1059 个动作成功，第二轮无工作；绑定 `6e19e87f` | `fc7-release-editor-*` |
| 最终 HEAD Editor | 全量无新增工作；完整 42/42 | `fc7-closeout-editor-*` |
| 最终 HEAD PLAYER | 冷构建 1012 个动作；二次无工作；33/33 | `fc7-player-*` |
| 安装后框架／基础消费者 | 8/8；最终 HEAD 再次构建、运行、重装 | `fc7-sdk-*`、`fc7-closeout-sdk-*` |
| 六组独立 SDK 消费者 | 9/9，真实 DLL／任务／生成消费者 | `fc7-independent-*-tests` |
| 外部 render 插件 | 安装 SDK 构建、加载真实 DLL，GPU 1/1 | `fc7-plugin-*` |
| 安装公共头 | 43 个单独 C++20／无 RTTI 编译通过 | `fc7-public-headers` |
| 编译／链接／安装闭包 | 无 legacy、旧 Engine SDK 回退或消费者私有头补齐 | `fc7-closure` |
| 公共头同步 | 112 条同步记录逐一字节核验；包括规定三个前缀 | `header-sync.json`、`fc7-final-inventory` |
| 证据搬迁／负例 | 中文空格路径可验证；真实输出缺失／篡改均被拒绝 | `fc7-evidence-validation` |

六组独立消费者为 object-core、object-ownership、ui-composition、services-core、services-tasks、spatial。
SDK 使用安装公共头和安装库，依赖前缀本身不含 Engine 头／库。
闭包检查覆盖 Editor 552、PLAYER 525、SDK 35、外部插件 3 个编译单元及六组独立消费者。
新 SDK 只安装一个 `lux_editor.exe`，私有 transport 头不再安装。

### 危险行为的对应证据

- `error.registry`、`framework.error_registration_conflict`、SDK Error DLL：预登记、幂等／冲突、
  稳定查询和卸载后解释；失败构造不调用登记 API。源码检查与测试均指向原 Registry。
- `ui.composition/root/structure_contract`：跨父移动、失败保留、单内容根、嵌套 Layout、
  焦点／capture 撤销、回调结构冻结、真实菜单目标和 DockTree 失败原子性。
- `ui.context`、`framework.input_conversion`：既有输入顺序和 Context 隔离；自动采用延迟结构批次，
  维护不会重复触发控件输入。
- `framework.context_component`：只链接 Context 的真实消费者可使用它声明的公共功能。
  SDK 同时编译 API absence、const 和构造约束；不存在为了通过测试保留旧接口的兼容路径。
- `framework.context/ui/desktop`：惰性服务、失败不缓存、冻结登记、项目 A→B、工厂中途失败及
  UI→服务析构顺序。SceneTool 的 dummy 资格没有扩大为真实工具选择资格。
- `framework.gpu` 的原像素回读、背压和退休断言迁至 app 私有 transport 测试；
  `framework.desktop` 使用公开 API 执行真实显示、resize、最小化／恢复和在途关闭。
  transport 不因测试而继续作为 SDK。安装后桌面测试验证正式公开接线。
- Object、services、Scene、Render 和 Flow 的受影响既有回归随完整矩阵执行。
  DLL 清理尾部、worker 末引用、固定消息批次和 SceneRuntime 唯一驱动未被新抽象取代。

## 成本、失败记录与保留范围

这轮只作有限结构验证，没有新跑全局性能基准。
稳定错误构造是数值操作；冷装配才登记描述。Registrar 保持小 vector。
Root 仍一次维护内容树，不重建全局 Element 表；菜单改为冷路径拥有值。
这些结论不等同于整个应用零分配或未经测量的速度提升。

早期独立冷构建暴露了旧 SDK 隐藏的 GPU fixture 直接依赖缺失；修正 include 与真实 target
依赖后，使用不含 Engine 的依赖前缀完成上述冷构建。
首次独立 Object 消费者仍使用 parent 构造的失败也保留，并在最终提交迁移。
迁移期编译失败、过宽回调保护导致的测试失败、一次错误使用旧 DLL 的超时都保留原结果，
不计作最终通过。没有删除失败证据以制造首次全绿。

普通 heap OOM fatal 合同已明确。本次清理针对改动的 Object／UI／Editor 合同，
没有宣称全仓历史领域枚举、GPU 分配或外部后端错误已统一删除。
原生输入接管仍为 **NOT_RUN_USER_DEFERRED**；自动桌面测试不替代它。
Linux、系统 IME、sanitizer、历史性能延期保持原判定。Android 仅同步公共头，没有构建资格。

## 证据、用户差异与清理

外部交付目录：`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2`。

- `runs/commands.json`：实际命令、实施 SHA、工作树状态、退出码与原始输出哈希。
- `final-convergence/files-49883572.txt`：100 个文件的 rename-aware 增删改清单。
- `final-convergence/qualification-49883572/manifest.json`：467 份实际构建／链接／安装／测试元数据。
- `final-convergence/verified-evidence/`：固定修前与修后原始输出；搬迁验证依赖相对路径。
- `final-convergence/production-equivalence.json`：冷构建和最终实现的精确差异。
- `final-convergence/protection-final-49883572.json`：用户差异与公共头同步核验。
- `final-convergence/qualification-cleanup.json`：验证冻结字节及 clean HEAD 后，删除唯一临时
  qualification clone／build；开发工作区、SDK、依赖前缀和外部证据保留。

原两份历史 ZIP 再次核验，哈希不变；原 v2 交付记录没有改写。
ProjectBuilder 用户文件 SHA256 仍为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，
保存在 `protection/ProjectBuilder/`，对应 legacy 路径，**未应用**。
`Pane.hpp` 用户注释缩进修正逐行比对保持原样，仍为本地未提交差异。
历史分支、main、PARTIAL／FAIL 和免验结论不变。
