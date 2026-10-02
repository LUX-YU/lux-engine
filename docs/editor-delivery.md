# Editor 交付范围与使用位置

本文件描述 P13 有限工程收尾的交付边界，不是完整平台认证或 release 声明。
P12 的历史状态仍为 **PARTIAL_USER_WAIVER**，原收据和失败记录保持不变。

## 工作区和产品

- 当前实现工作区：`E:/SyncForder/CodeRepos/lux-engine-p12`。目录名沿用 P12，但后续 P13 修改也在此处。
- 推送目标：`codex/editor-redesign-v4`；具体实现和记录 SHA 见 `dev_log/P13/` 的交接记录。
- 原工作区：`E:/SyncForder/CodeRepos/lux-engine`。其分支位置、main 和用户修改保持原样。
- 原 `editor/project/src/ProjectBuilder.cpp` 用户补丁**未应用**到新工作区，也未混入实现提交。
  对应现行路径是 `editor/authoring/project/src/ProjectBuilder.cpp`；原字节和映射补丁保存在
  `dev_log/P12/protected/`。

实际安装产品是 `bin/lux_editor.exe`，通过 `--project <Project.luxproject>` 打开项目；
不传项目时进入同一正式 Launcher 装配。`lux_launcher.exe` 使用该装配创建或选择项目，
再启动同一 Editor。没有旧产品回落或第二套内容 owner。
应保留完整安装前缀，不能只复制 exe；插件、依赖 DLL、资源及生成器都属于安装闭包。

## 已有能力与边界

正式五层为 editing、authoring、activities、workbench、application。三类作者会话、唯一 History、
保存基线、文件发布、Run、编译预览、交互和工作台装配沿用既有 owner。
窗口关闭与内容关闭分别处理；停止 Run 不隐式回写作者内容。

正式导入、暂停运行实例的 Registry 编辑、纯 ProjectBuilder、World/Scene codec 和只读旧布局迁移
继续保留。物理目录收拢不改变公共逻辑 include 的 project/storage 名称；node-editor 的
`ax::NodeEditor::EditorContext` 也不属于已删除的 Lux EditorContext。

`EditorApplication::execute` 必须在构造该对象的线程调用。外线程返回 `WRONG_THREAD`，
同线程 dispatch 重入返回 `BUSY`；两者都不准入命令。命令未知和禁用仍分别返回 `NOT_FOUND`
及 `DISABLED`。Help 中的版本文字是信息项，不能作为可执行 About 命令使用。

## 证据口径

P13 只为实际修改路径新增运行记录：命令准入、应用组合、相关贡献/依赖门禁及安装后公共 API 消费者。
未修改的模型、保存、Run、实际插件、PLAYER、GPU 双视口、原生输入、生成及跨 ABI 证据，
引用 P12 的固定实现 `85d2ed7ec6f9f4b39679a0d1bb6ed17b2888a616`；不计作 P13 新运行成绩。

本轮使用 Windows x64、MSVC、RelWithDebInfo、C++20。GPU 路径需要兼容 Vulkan 的运行环境。
Linux、系统 IME 和 sanitizer 仍为 **NOT_RUN**；旧 50k 性能长测保持原 **PARTIAL**。
这些限制既不证明平台不支持，也不证明已取得对应资格。

P12 最终手工菜单打开/编辑/保存/运行观察，以及其归档迁移、缺失/篡改和最终严格验收探针，
已由用户免验，本轮不补跑、不改成通过。安装产品进程正常退出不能代替这些交互观察。
P13 的有限收尾结果不覆盖这一免验记录，也不声称满足原 V4 全平台、全性能最终资格。

实现与记录分别提交并正常推送后，停在 P13 等待复审；不自动合并 main 或发布 release。
