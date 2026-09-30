# P07 — 作者投影、视口资源、材质预览与 Flow 编译

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P02, P03, P04, P05, P06。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M09；FlowForge 编译子模块。**关联问题：** A02、A06、T04、C05。**V3 回归：** Q23, Q27, Q28, Q29, Q30, Q50。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `SceneProjection / ScenePresentationHub / ProjectionVersion` | `editor/tools/scene/projection/include/lux/engine/editor/scene/SceneProjection.hpp` | 共享派生 owner；不拥有作者源 | 按 ContentStamp/增量更新一个作者投影；多视图共享场景，不共享可写作者事实。 |
| `ViewportPresentation / ViewportConfiguration` | `editor/tools/scene/projection/include/lux/engine/editor/scene/ViewportPresentation.hpp` | 每视口组合资源 | 独立相机/输出尺寸/overlay；以渲染视口身份寻址，不依赖具体 SceneView。 |
| `HighlightRenderer / HighlightKey / PreparedHighlight / SubmitOutcome` | `editor/tools/scene/projection/src/HighlightRenderer.hpp` | 私有分阶段派生组件 | desired/prepared/accepted；背压保留 pin；拒绝分类；后端 GPU 退休独立。 |
| `MaterialCompileId / MaterialCompileOperation / MaterialCompileSettings` | `editor/tools/material/preview/include/lux/engine/editor/material/MaterialCompilation.hpp` | 实际异步编译业务；TaskId 仅关联进度 | 捕获快照+编译配置+环境版本；过期结果不覆盖新预览。 |
| `MaterialPreviewStore / CompiledMaterial` | `editor/tools/material/preview/include/lux/engine/editor/material/MaterialPreviewStore.hpp` | 拥有编译结果和派生预览 | 失败保留作者内容；资源采用与 GPU 退休准确。 |
| `FlowCompileId / FlowCompilationService / FlowCompileOperation / LinkSettings` | `editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/FlowCompilationService.hpp` | 编译与链接的具体阶段 | 输入 FlowSnapshot；重试链接固定原产物和来源，不捕获 FlowSession*。 |
| `PublishCompiledMaterialOperation / PublishFlowArtifactOperation` | `editor/tools/material/preview/include/lux/engine/editor/material/PublishCompiledMaterial.hpp；editor/tools/flowforge/compilation/include/lux/engine/editor/flowforge/PublishFlowArtifact.hpp` | 具体派生产物发布用例 | 使用 P05 同一写入协调器；不更新作者源保存基线。 |
| `ResourceStatusSnapshot / ResourceRetryRequest` | `editor/tools/scene/projection/include/lux/engine/editor/scene/ResourceStatus.hpp` | 投影资源状态与明确重试值 | 与作者结构修改错误分开；不从旧 SceneEditor 直接查 GPU 私有字段。 |

## C. 先证明共享投影，再做每视图资源

SceneProjection 的输入是 SceneSnapshot 或受限 owner 读取的增量，输出是运行宿主中的派生场景实例/渲染资源。它不能向 SceneSource 提供修改通道。ScenePresentationHub 按会话及有效历史/投影版本管理共享记录，不能因创建第二视图复制第二份完整作者世界。

ViewportPresentation 的输出目标、相机、尺寸、交互 overlay、提交序列和退休票据独立。Hub 的缓存保留策略必须有容量/无使用者释放规则，不能因为隐藏视图曾经访问就永久保留所有投影。

### C1. 特别检查高亮是否实际按视口隔离

仅仅把 HighlightRenderer 放到每个 View 中并不足以正确。旧 HighlightReplaceTargetsPayload 使用场景/feature 身份，实施者必须核对后端状态是否实际是 scene-global。若两个视图向同一 feature 写选择，它们仍会互相覆盖。

目标是共享模型/mesh 场景数据，视图本地的 highlight/overlay 状态独立。已有后端支持 view-local feature 就使用它；否则只在对应 render feature/overlay 路径增补明确的视口实例绑定，不能通过复制整个场景回避，也不能只给 CPU key 增一个 ViewId 就宣称隔离。P10 的真实双视图不同选择测试必须证明后端行为。

## D. 高亮提交完整协议

| 状态/事件 | 应当做什么 | 禁止行为 |
| --- | --- | --- |
| 选择/投影/feature/视口版本变化 | 更新 desired key，准备新候选 | 先写 accepted key |
| 资源暂不可用或 capture 失败 | 保持 accepted 不变；等待/重试，并分类诊断 | 同条件下永久不再尝试 |
| Prepared 已完成但 backend Backpressured | 保留完整 program 与 pins，按后端重试条件推进 | 清空 program 或丢 pin 后只保留 bool |
| 旧 Prepared 未提交，新 desired 到来 | 释放未使用旧候选，准备新键 | 把旧结果当最新内容提交 |
| Accepted | 推进 accepted，将需要的所有 owner/pin 移交到 in-flight 记录 | 把“被接收”当“GPU 已完成” |
| 视图关闭 | 不再准入新提交；已提交资源按实际完成退休 | Pane 析构直接释放 GPU 仍在使用的资源 |
| 永久拒绝 | 保留结构化诊断；依赖版本变化或用户 retry 再试 | 每帧刷同一错误或当作临时背压死循环 |

HighlightKey 至少绑定投影、选择、feature 版本以及实际作用域；共享场景/独立视口应在类型和后端寻址上统一。`highlighted_selection/structure/feature/instance`、`highlight_pending` 不再作为几个独立的已完成真相。

增量丢失时消费 `ResetRequired` 重建。空间索引、父子闭包和 mesh 查询可复用现有算法，但必须明确投影版本；不能把旧 Registry 的 raw Entity 长期保存为作者对象身份。

## E. Material 编译和预览

MaterialCompileOperation 固定 MaterialSnapshot、CompileSettings、编译器/扩展环境版本；后台只输出拥有数据的 CompiledMaterial/诊断。采用时同时校验内容、配置、环境与目标 Preview 世代。

S10 结果迟于 S12 完成时可缓存但不能覆盖 S12；S12 编译失败也不让迟到 S10 自动冒充当前正确效果。需要保留最近可用预览时，UI 明确显示“旧预览/当前编译失败”，并在状态中保存其真实 stamp。

旧 `Compilation::{history,state,revision,preview_assets,status,output}` 合并成语义准确的编译记录；`compile_result_/compile_task_` 不保存在 MaterialView 中。旧 `preview_failure_` 只是展示诊断，不驱动领域状态。预览相机由视图设置值，实际预览实例与资源退休归 PreviewStore/Viewport。

PublishCompiledMaterialOperation 发布的是派生产物，不是保存 MaterialSource。即使编译/产物发布成功，也不把当前未保存的源标为 clean。所有写同一目标的来源都共用 P05 的目标发布顺序。

## F. Flow 编译/链接/发布

复用现有 FlowCompilation 算法和环境；FlowCompileId 与 TaskId 分开，后者只用于进度/执行器关联。CompileSettings 和 LinkSettings 明确来源，找不到所需 linker 时是 toolchain 条件失败，不把整个 Flow 作者工具隐藏。

`retryLink(compile_id, settings)` 针对原编译输出和捕获版本，不能读取“当前 graph”悄悄重新编译。每次链接尝试有可观察的状态，保留原失败诊断和可重试输入；超出保留预算必须明确拒绝重试或要求重新编译。

PublishFlowArtifactOperation 固定所选择的编译产物及格式/目标版本，采用同一写协调器；发布后不触碰 FlowSession 的源码保存基线。已有 ScriptArtifact ABI/模块寿命按引擎协议保留，不能以编辑器重构顺带修改 Lua 运行层。

## F2. 阶段出口

实际运行 CPU 编译路径与可用后端的预览/资源测试。Fake render 证明提交顺序和寿命；真实 GPU 在 P10/P13再验证视口隔离、布局与资源退休。两者结论分别记录。编译器需要额外工具的测试属于 toolchain 组，未执行不能记作通过，但不会阻断无相应工具依赖的作者域阶段。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 新派生路径中的 bool submit() 和提前更新 highlighted_* | 改为互斥 SubmitOutcome 与三阶段缓存 | HighlightRenderer / PreparedHighlight；P07 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧 SceneEditor::Impl::updateHighlight/updateWorkPlane/renderFor/submit 业务实现 | 抽出纯派生实现；旧壳暂留到切换 | scene/projection；WorkPlaneRenderer 为视口本地；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| MaterialEditor::Impl::Compilation/compile_task_/compile_result_/Preview | 新路径分别移到编译记录/PreviewStore，不放 View | material/preview；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| FlowForgeEditor::Impl::Compilation/acceptCompilation/retryLink/requestPublish | 迁到独立编译、链接和发布服务 | flowforge/compilation 与 persistence；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 所有派生结果对作者 checkpoint 的更新 | 禁止；只源码保存回执可改基线 | Session persistence role；P07 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X07-01 / `highlight_capture_failure_retries_same_key` | 相同选择首次捕获失败，下一次可用 | accepted 不变且下一次真正重试 |
| X07-02 / `backpressure_pins_survive_view_retirement` | 准备→背压→接受→关闭→GPU 结束 | 每步资源/代码 lease 在最后使用前不释放 |
| X07-03 / `two_view_overlay_state_is_not_scene_global` | 共享场景两个不同视口提交不同选择 | 实际后端绑定分离；不得只比较 CPU cache key |
| X07-04 / `material_out_of_order_compile_and_failure` | S10 晚、S12 早，另测 S12 失败 | 采用与陈旧标识准确；作者内容/dirty 不变 |
| X07-05 / `flow_retry_link_uses_original_artifact` | 编译后修改 graph，再 retryLink 旧结果 | 固定旧编译 stamp；不隐式重编译当前源 |
| X07-06 / `compiled_publish_does_not_clean_source` | 未保存源编译并发布产物 | 文件发布准确但作者 dirty 仍保持 |
| X07-07 / `projection_reset_and_capacity` | 裁剪增量并关闭最后使用者，持续创建/释放投影 | 可重建，有界缓存，无多视图复制作者世界 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P07.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
