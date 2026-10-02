# P13 有限工程收尾交接

本轮按 2026-10-02 用户授权完成有限工程收尾，**本轮限定范围完成**；不宣称原 V4 P13 的全平台、全性能最终资格通过。P12 仍为 **PARTIAL_USER_WAIVER**，原记录不修改，免验项目没有补跑。

前置交付：`32500d90e5e27cfd22be1ef6b29a5e3010efcb3c`。实现：`9390a6ba5b977ed1e52c9947a98439a7c169c5a9`。本记录为单独后继提交。

## P13-A：功能映射与前置

核对 P12 的 18 行功能映射，关联当前命令定义、真实调用及原语义迁移记录，见 `feature-review.json`。
普通保存、Save As、Save All、Undo/Redo、关闭窗口/内容、退出、项目创建/打开、三类内容、布局/恢复等没有重新实现。
原 cut/copy/paste/delete/select-all 空菜单占位的历史判定没有扩大；具体图内容的实际编辑能力仍在其领域中。
Help 版本项是只读、禁用的信息标签，不是可执行的 About 操作。

原 P12 的测试数、结果和未测范围没有改写。`inherited-evidence.json` 逐条列出 25 份已有证据的原实现 SHA、记录提交、路径与哈希；**全部标为 INHERITED_NOT_RERUN**。它们只支持未修改的 provider，不覆盖本轮命令入口修正。

## P13-B：引用、安装、生成与测试支撑

- 168 个 P12 删除路径及九个旧根仍不存在。正式 Editor 仍为既定五层，未新增框架或生产库。
- 核对实际 CMake 目标图中的 50 个 Editor library target，未发现 test/tests 下的实现 TU 编入这些生产库。
- 生产源码中的同名 `ax::NodeEditor::EditorContext` 是第三方画布类型；历史 inventory 中的旧 owner 名称用于固定 Git 调查。均保留，不用关键词删除替代责任核对。
- `lux/engine/editor/project`、`storage` 是仍有效的公开逻辑 include，不能按物理目录迁移误删。
- 正式 AssetImporter、ProjectStorage、运行实例 SceneEditing/SceneEdit/FieldEdit、纯 ProjectBuilder、ProjectManifest 和只读 LegacyWorkspaceImporter 保持基线 Git blob。详见 `preserved-algorithms.json`。
- 八份现行 Inspector 生成器/support 文件与原 P12 安装文件一致；本轮没有修改生成算法，未以新成绩代替 P12 的真实重生成记录。现有 `editor.inspector_codegen` 支撑测试重新运行通过。
- 私有 TestAccess 头未被安装。Application 仍使用既有受编译开关限制的测试 friend；没有加入新的产品写后门。
- 新增一个真实 Application SDK 消费者，只链接已安装 `editor_bootstrap`。编译命令没有源码私有 include，链接闭包来自新安装前缀；exe 和三份外部 DLL 均来自正式安装清单。

清单/文本检查不是 AST 或所有权证明；本轮另外运行现有实际 compiler-provider 和依赖正负例。没有发现需要额外删除的原体，因此不进行无依据的目录或类型整理。

## P13-C：错误线程分类补正

原 `EditorApplication::execute()` 把 `admission()` 的所有拒绝都映射为 `BUSY`，使错误线程看起来可以稍后重试。
修正只在该入口映射：线程准入的 `INVALID_STATE` → `WRONG_THREAD`；重入 → `BUSY`；其他未知错误保留领域失败分类及拥有型诊断。仍先准入，再查找和执行命令，没有新的同步机制或状态。

真实已安装 P12 SDK 负例：外线程调用新建 Material、退出和未知命令都返回 code=2（BUSY）、domain=`application.thread`，测试退出失败。修正后同一消费者返回 code=1（WRONG_THREAD）并通过。
源码回归还验证同线程外层 dispatch 内的重入为 BUSY，外线程优先被拒绝，不解除外层保护；作者 current/observed/dirty/binding/admission、SessionId 集合、命令 revision 和 ViewId 均保持，owner 后续正常调用、NOT_FOUND 和 DISABLED 分类不退化。

负例夹具首轮把只读 About 信息项误当成可执行命令，因此先在该断言失败。原输出保留；按真实菜单契约修正夹具后再取得上述错误分类的完整失败输出。没有修改 About 行为来满足测试。
审计脚本初次使用 raw working bytes 对比 Git blob，遇到仓库 CRLF checkout；随后改用 Git clean-filter 后的 blob 比较，未修改被保护源码。

## P13-D：实际运行与继承范围

显式配置 **P13 + STRICT**，Windows x64 / MSVC 19.44 / RelWithDebInfo / C++20：

| 本轮执行 | 结果与证据 |
| --- | --- |
| 全量 `all -j 4 -- -k 0` | PASS；`logs/all-build.log` |
| 第二轮和实现提交后的全量构建 | 均无新增工作；`all-no-work`、`final-no-work` |
| 13 项受影响回归 | PASS；应用真实 offscreen/GPU 组合、commands、五项 contributions/R1、Inspector codegen、架构正负例、compiler-provider、扩展及产品依赖负例 |
| 已安装 SDK 修复前真实负例 | 预期 FAIL；`logs/sdk-before-real-regression.log` |
| 当前新 SDK configure/build/test | PASS；`logs/sdk-after-regression.log`，真实 Application 的公共入口，没有隔离 shim |
| 当前 SDK 第二次构建 | 无新增工作；`logs/sdk-after-no-work.log` |

生产代码与测试在实现提交前执行，`validation-source-binding.json` 证明当时 patch 与实现提交中的相应字节完全相同；提交后全量构建无新增工作。安装后回归绑定 clean 实现 HEAD。新 SDK 为现有增量全量构建的正式安装，**不声称本轮完成新的 cold clone 或全平台资格**。

P12 已有 PLAYER、三模型/保存/Run、完整插件、双视口、原生输入、跨 ABI、公共头和生成记录，按原 SHA 引用；本轮没有重新制造这些运行成绩。C01/C04 的应用回归及 C03 的命令/贡献回归继续通过，历史 FAIL 快照保持不变。

**保持免验：** P12 最后手工菜单操作观察链、最终归档迁移/缺失/篡改探针与严格验收验证器。安装测试进程 exit 0 不能替代菜单观察。
**保持未测：** Linux、系统 IME、sanitizer。旧 50k 性能长测维持原 PARTIAL。不启动相关环境建设或长测。

## P13-E：交付位置与停止边界

- 新工作区：`E:/SyncForder/CodeRepos/lux-engine-p12`，本地分支 `codex/p12-closeout`；目录名沿用，不代表停在旧实现。
- 实现 HEAD：`9390a6ba5b977ed1e52c9947a98439a7c169c5a9`；记录提交在其后，正常推送 `codex/editor-redesign-v4`。
- 本轮 SDK：`E:/SyncForder/CodeRepos/install/P13-closeout`。它的实际安装和链接证据见 `sdk-install-closure.json`；原 P12 验证 SDK 不被覆盖。
- 原工作区仍在 `f7c27f9375cbf8dd8af37b30a6027a460de26213`，main 仍为 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`。
- ProjectBuilder 用户修改 SHA256 仍为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，**未应用**到新工作区；映射目标是 `editor/authoring/project/src/ProjectBuilder.cpp`。补丁保留在 P12 历史材料中。
- `implementation-files.json` 给出本轮八个文件的修改/新增内容身份：生产行为只改一个 CPP，新增独立安装消费者及回归，更新职责/支持和工程文档。没有删除生产文件、增加生产 target 或改变磁盘格式。

唯一可变状态仍在原工作区 `.internal/editor-redesign/`；本目录只是冻结记录。P13 有限收尾完成后等待复审，不合并 main，不发布 release，不扩大此前免验和未测项的含义。
