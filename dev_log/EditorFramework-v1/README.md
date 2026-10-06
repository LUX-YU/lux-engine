# Editor Framework v1 验收记录

状态：**框架 P00–P06 已实现，本轮 Windows 适用验证通过，停在独立复审。**
未迁移具体工具。原 EC4 仍未完成；历史 PARTIAL、FAIL 和免验记录均未改写。

## 工作区与版本

- 正式工作区：`E:/SyncForder/CodeRepos/lux-engine`。
- 分支：`codex/editor-framework-v1`；实现 SHA：`84e38bd7415e58817812c7ce9c11cb60b30c6a51`。
- 最新基线：`de315d48602c5155b919e9a698a9d9306f12decf`，不是原目录的 f7c27f93。
- 实现提交：1b1d790b6（隔离）、34a8a25f0（框架）、1d40785bb（生成目标）、84e38bd74（生成头安装隔离）。
- lux-cxx 主目录：`f5447a763fd042f4f08819c8c88f1676f9a2a3cb`；toolset 主目录：`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
- main、旧实施分支与历史快照未修改；辅助检出的独有 Git 对象已收回各自仓库的 `refs/archive/framework-v1/`。
- 新 SDK：`E:/SyncForder/CodeRepos/install/EditorFramework-v1`。中间安装和历史前缀保留，不拿它们补足最终 SDK。

## 实际交付

| 阶段 | 结果与唯一责任 |
|---|---|
| Framework/P00 | 原 editor 整体迁入 editor_legacy；默认 OFF，EXCLUDE_FROM_ALL，程序/库/生成名字隔离；根安装不包含旧包、旧二进制、旧 ABI/visibility 头。 |
| Framework/P01 | 新 Context/UI/App 三个 STATIC target 和唯一 lux_editor 产品；不创建空工具库。 |
| Framework/P02 | EditorWindow 隐藏 GlfwRuntime；一轮只采样一次 Input；Root 复用原窗口、焦点、停靠和输入机制。 |
| Framework/P03 | 独立项目值与 AssetVfs；三个冻结 Registrar；服务惰性唯一实例、逆构造顺序释放，工具匹配歧义拒绝。 |
| Framework/P04 | AutoSparseSet 唯一持有顶层 Pane，父链为 EXTERNAL；准备/提交失败保留候选，先撤销路由和查询，再通知并销毁。 |
| Framework/P05 | A→B 保留窗口、EngineContext、Root；纯值拒绝保留 A；进入切换后工厂失败清完候选并回到无项目。 |
| Framework/P06 | 唯一 EditorUiScene 传输实现，新旧宿主共同消费；固定 DrawData 资源、背压保留、单个 driveFrame 调用、原 Runtime 退休。 |

具体所有权：LuxEngine 持消息环境/Window/EngineContext/UiScene/可选项目 Context；Window 持 Input/Root；
Root 持顶层 Pane；Context 持项目值/VFS/Registrar；ServiceRegistrar 唯一持已创建服务。
关闭顺序为项目 UI→项目服务/Context→UI Scene→Engine/渲染退休→原生窗口→消息环境。

旧 Presentation 中的 UI Scene/帧槽/捕获/发布算法已删除并使用 EditorUiScene；WindowInput 及 UiFrame/UiRenderSyncStage 原定义一并迁走，无转发头。
AssetVfs 与原 View 共用私有查询算法，没有复制一套读取或 provider 寿命规则。
实际文件迁移/增删见 `files.tsv`；生产 driveFrame 唯一调用位置及旧测试保留检查见 `logs/final-source-audit-closure.log`。

## 实际运行证据

| 验证 | 结果 | 证据 |
|---|---|---|
| 独立 clean tracked 冷构建 | 1d40785b 完整 1030 步成功，二次无工作 | `logs/1d40785b-final-cold-build.log`、`1d40785b-final-second-build.log` |
| 最终实现全量构建 | 84e38bd74 clean 检出重配置、all -j4 -- -k0 成功，二次无工作 | `logs/closure-tracked.log`、`closure-all.log`、`closure-second.log` |
| 最终默认 Editor CTest | 31/31，通过真实 VFS/对象/UI/场景/资源及框架测试 | `logs/closure-ctest.log` |
| SDK 公共接口 | 三消费者构建；11 个公共头各自 C++20 独立编译 | `logs/closure-sdk-build.log` |
| SDK 运行 | Context、UI、offscreen GPU、native lifecycle 4/4 | `logs/closure-sdk-test.log` |
| 实际安装产品 | 安装 exe 创建窗口、渲染后 WM_CLOSE 正常退出；实际装载无 build/旧 SDK/legacy DLL | `logs/closure-installed-product.log` |
| Legacy ON | 两个产品、桌面 GPU 和实际扩展 DLL 构建；相关测试 8/8（其中 3 个旧路径） | `logs/closure-legacy-second.log`、`closure-legacy-regression.log` |
| Legacy ON 安装隔离 | 新前缀仍只有 11 个框架头、一个框架包；无旧接口和 DLL | `logs/closure-legacy-delivery.log` |
| PLAYER | 同一独立 clean 源重配为 PLAYER，实际图无新旧 Editor；all、二次无工作、26/26 | `logs/closure-player-all.log`、`closure-player-second.log`、`closure-player-test.log` |
| 删除生成输出后重建 | UI 真实生成输出重新生成，all 成功，二次无工作；UI 回归通过 | `generation-rebuild.json`、`logs/generator-rebuild.log`、`generator-second.log`、`generator-ui-regression.log` |
| 头文件同步 | AssetVfs/InputEvent 同步 Debug、RelWithDebInfo、Android 三前缀，按字节验证 | `logs/closure-delivery.log` |

冷构建准确绑定 **1d40785b**；最后的 **84e38bd74** 仅改 editor_legacy/CMakeLists.txt 的生成目录，默认产品源码不变。
最终实现重新检出、验证 clean、重配置、执行 all 和完整适用 CTest/SDK/实际安装产品；没有将增量运行宣称为第二次冷构建。
生成删除重建证据属于 1d40785b；这条未修改路径按其 SHA 继承，不声称在 84e38bd74 又执行了一次删除。
PLAYER 使用同一独立构建目录重新生成目标图，复用基础编译产物，没有复用 Editor 目标或声明全新 PLAYER 冷构建。

框架断言实际覆盖：惰性/递归/重复/失败服务；typed tool 无匹配/歧义；VFS priority/limit/enumerate/provider 生命周期/项目隔离；
批量 Pane 第 N 项失败、旧 handle、焦点撤销、回调 BUSY；输入 sequence/trickle、无帧不重复消费、组合输入捕获和 ActionMapper 门控；
A→B 析构顺序、失败 C 清理后重开 D、resize/minimize、GPU 在途退出；真实 UI 前后图像回读差异及验证层错误为零。
窗口自动生命周期和合成输入测试不等于系统 IME 或人工原生输入资格。

原 Editor 70 个 C++ 测试源逐字保留，共 6099 处断言位置。**这不是运行了全部旧工具测试的声明**；
本轮只运行受影响路径，未改动工具行为不借此次通过扩大资格。

## 保留的失败与修正

- 原 Surface ResizeTarget 被 offscreen-only 检查拒绝：在现有 render control 安全点使用 PresentContext 重建，随后 native resize/minimize 通过。
- 预验收 UI DLL 缺 imgui_core.dll：ui 明确链接实际依赖，产品安装 runtime DLL；隔离 PATH 消费运行通过。
- Legacy EXCLUDE_FROM_ALL 后动态加载库未自动构建：补真实依赖，保留首次加载失败，实际 V10 扩展通过。
- Legacy 生成目标名称遗漏：生成器输入、消费者和规则同步更名，无同义 alias。
- Legacy ON 安装泄漏：`legacy-on-install-closure.log` 保留失败；生成头改为 legacy 私有目录，新前缀检查通过。
- 初始文件 API 查询时机、GPU readback 测试需要继续 tick、一次 runner 参数顺序错误及 loader 对话框中止均保留原日志；
  不把夹具错误列为成功运行，不用重试生产算法掩盖失败。

## 用户修改、目录清理与支持范围

ProjectBuilder 原字节 SHA256：`CCAC49618140D014236C9F23B55FC15D31C96E94B44395DEE39FD67D35AB1F5C`。
原始字节、原 blob、binary diff 和可应用的路径映射补丁见 `protection/`。
新路径：`editor_legacy/authoring/project/src/ProjectBuilder.cpp`。**用户补丁未应用、未混入实现源。**

18 个历史阶段源码目录已逐项审计并移除，路径、独有 refs、未跟踪文件处理见 `cleanup-inventory.json`。
两份本轮临时 qualification 检出也在完成后校验 clean/祖先关系并删除，见 `logs/qualification-cleanup.log`。
其他项目、历史安装前缀、历史验收目录未删除；未删除任何历史分支。

本轮产品仅有框架示例 Pane、内存项目描述与布局。真实项目文件加载、资产浏览、Scene/Material/Flow 工具、
插件装载、保存、历史、试玩均未进入新产品；旧实现通过显式 legacy 目标保留。

未测/延期：源码与 SDK 原生输入接管 `NOT_RUN_USER_DEFERRED`；系统 IME、Linux、sanitizer、旧性能长测保持原状态。
Android 仅同步头，不作为构建资格。旧 EC4 及其他历史 PARTIAL/FAIL 不改判定。

## 核验

`commands.json` 记录每个实际命令的退出码、时间、源码状态、SHA 和相对日志路径；失败记录仍在。
`manifest.json` 为本快照所有材料提供相对路径和 SHA256；`verify.py` 只读取归档自身，不依赖生产机器绝对路径。
日志中的绝对路径是运行事实，不是归档核验所需路径。实现和本验收快照分别提交，正常推送新实施分支后停止复审。
