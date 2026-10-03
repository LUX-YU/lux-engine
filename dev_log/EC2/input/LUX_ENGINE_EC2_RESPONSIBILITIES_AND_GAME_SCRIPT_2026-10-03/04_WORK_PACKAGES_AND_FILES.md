# 04　逐批施工、类型处置与实际文件范围

## 1. R0：必须先形成可实施的真实清单

读取当前 AGENTS、docs/editor-quality.md、EC1 receipt/report、责任调查和本包。只检查新基线，不还原旧 HEAD。

必须输出到唯一 ec2 账本：

- 当前 tracked SHA、实现父提交、worktree 与用户 diff 状态。
- RD01–RD08 的现有符号/方法/成员/文件/直接和传递消费者；已在 EC1 消除者标 REMOVED，不重复施工。
- 脚本链：来源挂载 → ScriptSystem 构造 → capability 准备 → method bind → invoke/start → completion → stable resume → 实例退役；标每步真实 owner。
- 资产链：原 AssetReadPort/loadAsset、VFS 挂载来源、typed codec、read budget 和 native result 寿命；有没有现存等价 ScriptAbility。
- per-instance capability prepare/release 是否存在；handle 的数据运输方式是否已覆盖固定结构；对不足只给一个窄补正方案。
- Native/Editor-only/Script-exportable/已绑定/本轮绑定/未绑定功能表，按语义不是按函数名 grep 决定。
- 真实 target 图及 include/link 闭包，明确拟新增 assets 叶子不会造成 domain→scene/process 反向边。

R0 不以“环境中全部测试通过”代替上表，也不拖成一次全仓穷尽 AST 工程。出现已定位的阻塞先修其前置；不得拿旧未测项作为理由扩大本轮。

## 2. 逐文件施工范围

下表中的新名称/新文件均为目标。能在原语义文件内表达的，不为每个小值新建文件。迁移旧 public 名称时同步全部消费者，不保留 shim。

| 当前文件/类型 | 施工动作 | 最终责任 |
|---|---|---|
| activities/project/.../ProjectPublication.hpp | ProjectPublication 分为固定 plan + PreparedProjectPublication；receipt 合同与之对应 | 计划/准备权限 |
| activities/project/src/ProjectStorage.cpp | 只用新 prepared 访问器，保留 publishing_ 唯一权威、mount/目录采用 | 项目 owner |
| activities/project/src/ProjectPublicationOperation.cpp | 复用原步骤，消除 Application 的重复执行；必要输入支持只加这里 | 发布执行 |
| activities/project/.../ProjectOpenData.hpp | 实际持 lease 时改 PreparedProjectOpen，旧名删除 | 拥有型打开准备 |
| activities/project/src/AssetImporter.cpp | 模型 recipe 纯值/codec 提取；领域命名与消费者更新 | 模型导入活动 |
| authoring/scene 新/原配置值头 | draft/稳定关系/来源值，准确头依赖 | 纯作者配置 |
| activities/scene 新/原配置准备源 | 无 Root 的默认值、固定描述准备；复用原 builders/facts | 配置准备算法 |
| workbench/scene/.../SceneConfigurationElement.hpp/.cpp | 移出正式描述构建/预设/领域错误；保留 UI scratch/捕获/控件寿命 | 配置界面 |
| workbench/scene/SceneCreationView.cpp / SceneConfigurationView.cpp | 使用同一 draft/prepare/commit 流程 | 用户动作 |
| application/src/EditorArtifacts.cpp | 删除打包/Task/文件/manifest 核心顺序，保留发起/观察薄组合 | 产品接线 |
| application/.../EditorApplicationImpl.hpp | 执行字段归活动，删除冗余 flags/回调记录；保持其他用例原 owner | 装配/有限 UI 状态 |
| application/src/EditorResults.cpp / EditorWorkspace.cpp | 真实面板构造迁 workbench 主题；不再访问 Impl | 通用视图 |
| workbench 对应 result/workspace 主题（目标，可合并相邻） | 单一 View+必要视图值/请求；不建全局 ResultsManager | 展示和动作 |
| activities/material/.../MaterialCompilation.hpp / CPP | input key 与 target 分域，封装 CompiledMaterial；迁错误 | 编译结果 |
| activities/flow/.../FlowCompilationService.hpp / CPP | 按实际关联封装 CompiledFlow，不修改固定 object 重链策略 | 编译结果 |
| activities/material/.../MaterialPreviewStore.hpp/.cpp | 最终语义为 MaterialPreview；接收完成值；配方移同主题源 | 活预览 |
| activities/workspace/.../WorkspaceStore.hpp/.cpp | status 与目录 IO 分离，去掉误名 layout receipt | 文件存储 |
| activities/workspace/src/LegacyWorkspaceImporter.cpp | 纯转换与读取/推进分开，保留原指纹、来源、marker | 迁移 |
| engine/process/asset_loading/ 原文件 | 优先不改；确有可复用低层缺口才补，不 include script/Editor | 原生资产读取 |
| engine/scene/scripting/assets/（目标） | ScriptAssetAccess/scope/请求结果与能力声明，两个可分离 native/Lua target | 游戏能力组合 |
| modules/function/script/core/lua 的原能力与生成设施 | 原样复用；固定值 marshal 缺口只补通用值协议，不加 asset 类型 switch | 通用语言机制 |
| engine/domain/simulation/scripting/core 及 builtin script | 只在有证据需要时补 per-instance 准备/撤销钩子，保留唯一生命周期 | 运行脚本机制 |
| PLAYER 和 Scene runtime 的正式组合点 | 注入同一个 runtime assets provider，关闭时结清 | 正式运行宿主 |
| editor/activities/scene Run 构造路径 | 借运行资产输入，不把 Editor ProjectCapabilities 传到脚本 | Editor Play 接线 |
| cmake/installed-consumers 及相关 tests | 原用例迁移，新增真实无 Editor script/asset consumer | 资格消费者 |
| 生成/安装 CMake、架构 rules、文档 | 删除旧入口和失效规则，新增真实禁止边 | 工程闭包 |

省略号不是允许实施者任意发明路径：R0 必须从 repo 真实路径展开并写入 file-actions。没有可靠路径前不得进行批量文本替换。

## 3. R1：固定计划/准备权限

入口：ProjectPublication 全部消费者已列出。先记录原行为测试与计划可变性编译测试，再实现新类型和原 Operation 的消费。

出口：无 public 可改的 prepared 计划；worker 只能取固定拥有值；同项目第二发布仍被原占用保护；移动/丢弃唯一释放；真实写入后事实保持。所有原调用和测试改为唯一新入口，旧符号不安装。

不顺手改写 VFS、文件替换和 WriteCoordinator 顺序。

## 4. R2：无界面配置准备

入口：声明相关所有权与动态 codec 寿命。先用固定输入构造与原 UI 等价的 draft/正式描述。

出口：无 Root/ImGui 的消费者能够完成原配置/预设/拒绝场景；UI 和非 UI 返回同样的正式配置与错误；Unknown 值不丢；实际 partitioner 输入明确。运行脚本不会因此得到 Editor draft。

## 5. R3：发布活动与正式面板

入口：R1 prepared 可由原发布者消费。迁走 Application 中真正的业务状态，保留泛化的 DerivedArtifact。

出口：无 Application 的正式活动消费者可打包/发布/采用；两个面板能独立构造；内置与外部调用同入口。文件成功但登记失败、源后续保存、关闭中的完成、Unknown 均由唯一活动保持。

不得将全部 Application 字段搬到一个新的 ApplicationService。迁移前后逐项标记“谁拥有、谁只观察”。

## 6. R4：预览、Workspace 和 recipe

三个子工作按独立变化闭包提交：

- 预览：input identity 不含 destination；独立 recipe 与 live owner；两目标共享产物，目标分别采用。
- Workspace：纯 import 函数、明确 IO/状态；原幂等和恢复选中范围保留。
- 模型：recipe codec 与 IO/cook 分开，明确 ModelImport 名称。

出口：没有旧控件/发布/preview 旁路；小型纯算法不新建 Controller；原 Snapshot、History 和 Run 机制不复制。

## 7. R5：运行资产能力与脚本作用域

入口：R0 对 ScriptInstance 生命周期和已有 provider 已作真实核对。

先做无 Lua 的能力实现与测试：真实 loadAsset/AssetReadPort、可运输句柄/结果、完整域/代次、原生 scope owner、预算、显式 release、实例结束与晚到完成。

如果缺少 per-instance provider 释放关联，先在原准备/生命周期补窄钩子。不能完成 Lua 绑定后再以“现在没有 caller_id”为理由留下泄漏或访问任意世界。

出口：C++ consumer 不依赖 Lua/Editor/GPU/LLVM；read/decode/查询不造第二算法；所有接受请求在停止后仍回收，handle 不复活。

## 8. R6：真实 ScriptAbility 和 Lua 投影

使用原 `lux_script_abilities`、semantic traits、LuaValueOperation 与 backend 配置。新叶子只组合能力实现、声明和投影。脚本实例申请的 capability/schema 不匹配时在调用前拒绝。

检查 native static/erased 入口和 Lua 调用的参数、状态码、可恢复错误一致。ordinary asset failure 作为可检查结果，而不是默认 fault 整实例；真正协议错误仍走原错误通道。

出口：真实脚本调用、挂起、恢复、值使用、release 都通过正式 VM/ScriptSystem；不能只调用 C++ dispatch lambda 就声称 Lua 已支持。现有 Delay、同步 Hook、事件、local async 等回归保留，资产不使用 timer 特权。

## 9. R7：正式 PLAYER 与 Editor Run

至少三个运行者：

1. 无 Editor 安装 SDK consumer：原生资产读取 + ScriptSystem/Lua 能力测试。
2. 正式 PLAYER：加载包、运行真实 Lua、请求资产、使用结果、执行一次原命令屏障、正常退出。
3. Editor Run：同一运行逻辑，作者 history/current/dirty 不受运行修改污染；关视图不擅自停止其他 owner。

保留 EC1 骨骼 DLL 的 HEADLESS/WINDOW/APP 三路线，尤其本轮修改 public prepared/preview/能力头后。游戏资产能力不是新的骨骼编辑器，不要求重做 EC1 内容路由。

## 10. R8：代码与工程收敛

- 删除改名后的旧 public 头、alias、旧编译列表、旧安装文件和 unused source。
- 将真实 source provider/target/生成输入对应到架构规则，不给整个 engine 或 script_core 新白名单。
- 修 AGENTS 修改闭包：命名、V 前缀、具名 bool、短路、private 字段、120 列、include 边界、异常/observer。
- 性能检查按本轮新路径：稳定查询、典型 IO/解码、脚本边界、容量和复制；不补旧深链计时。
- 发布质量评价使用函数/责任表、实际依赖和可独立消费者，不用文件数、类数或新 concept 数量。

## 11. R9：最终交付

全量 `all -j 4 -- -k 0`；第二轮无新增工作。修改闭包、PLAYER、SDK、真实脚本/原生与受影响 GPU 顺序运行。最终 `EC2 + STRICT`，实现和证据分别提交。源绑定清楚，不能将旧 SHA 运行成绩记成最终新 SHA 重跑。

报告分四栏：本轮运行通过／准确继承证据／未测或免验／仍未完成。历史 EC1/P13 不重写。

交接必须给出用户应打开的新工作区和 HEAD、原 ProjectBuilder 补丁是否已应用（默认没有）、新旧 ABI 指纹与安装 prefix、删除清单、剩余明确范围。停在 EC2，不自动开启下一阶段。
