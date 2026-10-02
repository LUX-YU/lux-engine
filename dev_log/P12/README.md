# P12：正式产品切换与旧体系删除

本次按用户“验收不必了，直接推送吧。”停止剩余验收并冻结已有记录。状态为 **PARTIAL_USER_WAIVER**，不宣称 P12 全部验收通过；实现和已完成测试不撤销，原严格验收规则不放宽。

## 来源、工作区与提交

前置验收为 `0a6644d8781dd3aeea1da549a87dfd6303a1c1dc`。实现位于 `E:/SyncForder/CodeRepos/lux-engine-p12`，分支 `codex/p12-closeout`；最终资格 SHA、逐条命令、起止时间与退出码见 `receipt.json`。实现提交和本验收提交分开；正常推送至 `codex/editor-redesign-v4`，没有进入 P13。

原 `E:/SyncForder/CodeRepos/lux-engine` 仍在 `f7c27f9375cbf8dd8af37b30a6027a460de26213`，其 `ProjectBuilder.cpp` 用户修改没有应用到新检出或提交。原字节、binary diff、新路径补丁位于 `protected/`。对应新路径为 `editor/authoring/project/src/ProjectBuilder.cpp`。`main` 与之前的 `dev_log` 全部保持原样。

## 正式路径及唯一责任

| 事实或资源 | 唯一 owner 与产品组合 |
|---|---|
| 三类作者内容、History、current、checkpoint | 原具体 Session、SessionStore；窗口只绑定身份和领域交互 |
| 打开、重载、保存全部和整组关闭 | activities/sessions 的具体操作；枚举内容，不枚举窗口代替内容 |
| 保存准入、编码完成、磁盘事实、基线采用 | 原 SaveService / SaveExecution / WriteCoordinator 分工不变；拒绝清理保持 dispatch，已接受完成仍吸收 |
| 顶层 Pane 与布局原子采用 | 原 ViewHost / Root 的短准备能力；Application 组合内容与工作台，不在 activities 引入 UI |
| Run、暂停历史、单步结果与回收 | 原 RunStore / SceneRuntime；只使用冻结源；关闭视图与停止 Run 分开 |
| 预览、投影、GPU 使用与退休 | 原投影/编译/PreviewStore、RenderResources 和 RenderRuntime |
| 项目目录、导入、设置和创建 | 正式 ProjectStorage / AssetImporter / ProjectPublicationOperation；纯 ProjectBuilder 仍在 authoring |
| 命令、贡献与代码寿命 | P11 V7 不可变目录和短批次；不能把 Batch 留到下一帧 |
| 执行与完成 | 原 ExecutionRuntime；退出与析构仍收取已准入任务、文件和 GPU 责任 |

唯一 `lux_editor` 由 `editor/application` 的正式装配构建；launcher 使用正式创建和原 `launchEditor` 进程启动。没有 Old/New 选择、旧 Context 回落或第二个可运行产品。近期项目仍使用旧 v1 数据格式，经同一个写协调器发布；初始 Scene 从项目正式描述打开，重复命令复用同一会话。

## 到期删除与保留

提交中的 Editor 一级目录仅为 `editing/authoring/activities/workbench/application/tests`。九个旧根及其原声明、定义、TestAccess、friend、生成输入和 CMake 安装入口已删除；精确逐文件信息见 `files.json`、`removal-plan.json`。

隐藏在正式路径中的旧 AssetEditing/AssetOpenRequest/AssetSave/CloseRequest/旧 EditHistoryTarget 协议、旧静态保存接线、双重 SceneCapture/Run 准备和旧生成 Inspector 通路同步退出。根 Pane 的 rooted 兼容构造不再保留。原完整算法迁出后删除原体，没有重新编译旧 Editor 测试副本。

保留 `editor_assets`、`editor_storage`、`editor_editing_scene` 与 `editor_launch`：它们分别是正式导入、项目存储、Run 暂停编辑和平台进程启动的真实实现，并非同名兼容壳。保留 World/Scene codec、纯 ProjectBuilder、唯一 History，以及只读 LegacyWorkspaceImporter 数据迁移。磁盘格式未因 C++ 清理升级。

原测试的名称数量随旧协议删除发生变化。`g-semantic-map.json` 绑定旧测试 blob、原断言及新源码观察；`regression-name-map.json` 解释每个退出名称。保留真实领域、完整作者编码、磁盘发布、History、代际、回调清理、插件代码寿命和资源退休断言，不用数量相同证明等价。

## 验证与失败记录

最终资格使用固定 clean tracked commit、独立源码 clone 和空的新 SDK 前缀，显式 `LUX_EDITOR_MIGRATION_STAGE=P12`、`LUX_EDITOR_LAYERING_MODE=STRICT`。完整 all 构建采用 `-j 4 -- -k 0`，随后验证无新增工作；CPU、PLAYER、安装消费者、实际 V7 DLL、真实 IO、公共头、依赖负例、八项 operation 编译负例、正式双视口与原生输入顺序执行。

最终实现 `85d2ed7ec6f9f4b39679a0d1bb6ed17b2888a616` 的非交互资格：主配置 219/219、CPU 201/201、PLAYER 13/13，24 组真实安装消费者（原生输入另行执行），60 个生产公共头 clang-cl 独立解析。指定生成重建后内容哈希不变，第二轮无新增工作；改变 Editor ABI 版本的隔离配置不改变 Runtime ABI 身份。最终 SHA 的源码与 SDK 原生输入均通过。安装菜单测试进程正常退出，但没有完成可核验的打开／编辑／保存／运行菜单操作观察；该项按用户要求免验，不将进程退出码解释为菜单链通过。

必须单独区分的证据：

- `editor.application` 覆盖真实产品组合；X12-01 通过实际工厂拒绝证明已发布内容不被错误回滚，恢复显示复用原会话。
- C01 是正式 Root 的坏布局拒绝且原结构保持；C04 是真正菜单安装失败使 Application 工厂失败；C03 保持原命令回调/代码寿命。历史 FAIL 不改写。
- 双视口 GPU 验证包含实际后端高亮隔离和退休；原生输入验证走新桌面，不冒充系统 IME。
- 安装产品启动、模块装载与正式菜单操作单独记录；SDK 不读取 build DLL 或源码私有头。
- 归档保存相对证据与固定 Git 对象。最终严格验收验证器及中文/空格路径迁移、真实日志缺失/篡改、错误实现 SHA 探针本次未执行，按最新用户要求停止；不填写通过结果。

开发期首次失败保留：`becc1509` 的资格因项目入口范围缺口主动终止；`740b76a1` 冷构建暴露 bootstrap 缺少直接平台依赖和公开命令依赖，保留实际失败后修复，再从后继 clean commit 重建。`3c790677` 的完整主配置与原生输入通过，但 CPU 配置的真实模型重导入暴露 261 字符源路径被误判缺失。保留 CPU 首次失败及已安装 SDK 的复现；统一文件边界路径转换，增加固定长路径发布、版本冲突与重导入回归，再绑定后继实现重新验收。开发期测试夹具作用域导致的 TaskScope 借用错误及新增测试变量重名也保留原输出，不将失败日志当最终通过。

`a5ec94d7` 的全量、CPU 和 PLAYER 资格通过，但首次真实 SDK 配置发现 `scene_execution` 未导入同包的 `scene_execution_api` 组件。补正后新前缀编译继续发现 `ModelPlacement.hpp` 未列入安装清单。随后实际模型 SDK 的原测试体已新增 P12 打开/重载行为，消费者还缺少 `session_execution` 直接链接；同步其真实依赖并登记 `installed.p12.content_operations`，没有减少断言。上述真实失败均保留；补齐提供方依赖与精确头清单，没有要求消费者预加载组件或借源码头绕过。另保留交互测试私有头条件范围的搬迁错误和桌面生成控件 COFF 节数超限；恢复原 native 条件范围，在生成器提供方仅为生成源设置 `/bigobj`。SDK 原生输入单独在构建全部结束后的保留时段运行，仍为最终强制项。候选前缀的完整 SDK 回归通过后，再固定最终实现 SHA 重新进行清洁资格。

最终提交的首轮原生输入遇到持续 OS 指针移动与前台切换，35 秒等待后失败。保留实际指针、输入序号及前台记录，重新预约静止时段后运行；不降低等待条件或删除行为断言。公共头检查脚本最初把两个 SDK 测试夹具头误认为生产安装头，按实际安装清单纠正范围；生产公共头无缺失跳过。

Linux、系统 IME、sanitizer 均为 **NOT_RUN**。旧 50k 深链长测维持原 **PARTIAL**，未补样本。此处不宣称 P13 的跨平台或完整输入法资格已完成。

阶段结束停在 **P12 等待复审**。

## 最后安装闭包补正

原生输入复测（源码与 SDK 两项）通过后，实际安装产品的完整 Initial3D 项目启动发现 `project.plugins` 拒绝。真实 SDK 回归确认安装前缀缺少 Physics2D 的 `box2d.dll`；继续完整插件加载又发现窗口依赖的 `glfw3.dll`、`nfd.dll` 缺失。原受限加载器不查 PATH，不能靠开发环境中已加载 DLL 或放宽搜索路径解决。补齐 Physics2D 的 imported runtime 安装及两产品的 TARGET_RUNTIME_DLLS 安装，并登记实际 `installed.p12.builtin_plugin_closure`。首次失败、依赖清单和旧前缀诊断补入过程单独保留；诊断副本不作为最终 clean 安装证明。最终实现重新从独立 clone 与全新前缀资格。

## 用户指定停止验收与推送

2026-10-02，用户明确要求“验收不必了，直接推送吧。”因此不再运行剩余测试。原始失败、实际成功输出和历史快照保留，免验范围见 `user-waiver.json`。最终源码及 SDK 原生输入均已通过；安装产品菜单动作未完成观察，不能从 wrapper 的 exit 0 推断通过。原严格验证器仍要求完整动作证据，不为本次免验放宽。

最终实现 SHA 为 `85d2ed7ec6f9f4b39679a0d1bb6ed17b2888a616`。24 组 SDK、PLAYER、实际 IO、GPU、安装插件闭包、clang-cl 和生成重建的完成结果见收据。三份运行 DLL 来自正式安装清单；最终新前缀未手工补 DLL。137 项过时安装输出已清除，18 份公共头同步完成。

逐文件变更（Git rename 识别）：新增 68、修改 171、删除 174、迁移 10。原工作区、main、历史快照和 ProjectBuilder 用户修改不变；用户补丁未应用到新检出，交付路径为 `E:/SyncForder/CodeRepos/lux-engine-p12`。推送后停在 P12 等待复审，不进入 P13。
