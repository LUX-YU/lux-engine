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
