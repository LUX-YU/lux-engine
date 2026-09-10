# ER-1 q8 续行交审：实际结果与边界

ER-1仍未通过。本轮代码候选 `e6252e503670bf7037c1f275a0adae63276ae62f`；本报告后续提交只更新 `.internal/editor-er1/`，与编译候选分开记录。已完成当前候选的正常/诊断 clean 资格、安装迁移、原相关回归及有限成本。没有推送、合并main、发布、冻结或进入ER-2。

原118行保持完整：60 PASS_ER1、5历史 PASS_INTERMEDIATE、28 PARTIAL、1 BLOCKED_DELETE_GATE、24 DEFERRED_STAGE。R01、R09按本轮真实新增证据转为PASS_ER1；其余未完成门槛未删除或改名。M01—M30仍保留本阶段旧入口删除条件。

## 实施范围

准确授权diff映射见 [Q8-CONTINUATION.zh-CN.md](Q8-CONTINUATION.zh-CN.md)。G01—G04、EX01—03、metadata和默认相机协议保留；本轮没有重写总体架构。

- EX04：通用UI冷工厂拥有font bytes/ranges，face/尺寸/范围/atlas限额校验；Impl负责构造未完成时的context释放，先销毁context再释放backing。EditorWindow显式读取配置文件；`--font`不做系统目录搜索，指定字体失败不静默fallback。
- EX05：UiTextInputAnchor只输出成功捕获帧的主viewport相对逻辑坐标、line height、frame及有效性；失焦/隐藏/新帧/失败使其无效。Windows IMM位于EditorWindow.cpp私有实现，同时设置composition与candidate，配对取得/释放IME context。按实际client extent转换一次，不额外乘framebufferScale。未增加字符解码或第二条commit入口。
- Q7-UI-CONSTRUCTION-1：移除会吞掉标准分配失败的Impl构造noexcept，失败边界保留输入、恢复此前current context，UI A继续可用并可重试UI B。Atlas CPU复制在UI DLL内转换为结构化错误，Renderer保留原错误分类与Window owner。
- P02：一个有界current-request关联索引；完整Entity代次定位请求，mesh/material来源身份仍保存在权威request key。旧请求继续原pending-change、重试、晚到和退役协议。没有每帧复制全量资源，没有删除GPU保护。
- R01：Scene接纳前拒绝冲突的既存ResolvedMeshResources来源，错误为STALE_CONTENT且带Session身份。
- R09：专用诊断在真实consumer tick边界暂停；容量2的实际Control/Upload分别拒绝，恢复、关闭是两个独立真实GPU变体。暂停/统计接点不安装，不进入正常产物。

## 修前与修后原始证据

工作区勘察及附件CRC/hash见 `development/workspace-before.json`、`attachment-audit.json`。实际工作区是E:/lux-er1/src，初始ca9a918d；过期9905路径不作为源码身份。主检出/并行脚本受保护文件及两个未知ZIP最终hash不变。公共UI头按AGENTS同步三前缀仅是头同步，不代表Debug或Android构建。

|真实边界|修前|修后|证据|
|---|---|---|---|
|新context建立后UI准备分配失败，index7—10|真实bad_alloc后ImGui live38→56，残留18；此前context恢复、A可用仍不能证明无泄漏|同点live38→38，原context恢复、A及重试B可用、最终分配释放相等|development/raw/construction-before-*.log、construction-after-*.log；最终qualified/diagnostic-raw/ctest-details.log|
|字体工厂真实UI DLL分配扫描|本轮新功能无旧通过记录|indices0—14实际结构化分配失败；15—17走成功分支；同一输入保留、A可用、重试成功、ImGui剩余0|qualified/diagnostic-raw/ui-font-failure-0..17.log|
|UI atlas复制分配失败|新增故障测试|UI DLL真实分配拒绝，Renderer返回ALLOCATION_FAILURE，Window UI保留且可用，重试后真实GPU及关闭通过|qualified/diagnostic-raw/gpu-ui_atlas_failure.log；ui_atlas_capture_failure|
|R01冲突来源接纳|admitted1、输入被移动、lease1→2；以exit1报告失败且全部owner显式关闭|admitted0、输入/选择保留、error8=STALE_CONTENT/session1、lease1→1；修正调用方输入后真实渲染及关闭成功|development/raw/r01-before.log、qualified/raw/gpu-resolved_source.log及诊断同名日志|
|R09真实资源背压/恢复|原矩阵缺失，未伪称审阅方跑过|实际control2/upload1拒绝，三行UPLOADING且无资源失败；恢复后同一request keys全部READY|qualified/diagnostic-raw/gpu-resource_backpressure.log|
|R09背压/关闭|原矩阵缺失|消费者暂停时8次PENDING且同owner保留；恢复后COMPLETE、runtime及资源两份lease归零，独立后续Scene在同Renderer渲染并关闭|qualified/diagnostic-raw/gpu-resource_backpressure_close.log|

修前诊断工作构建不是新候选的clean资格，DLL/EXE/PDB身份另存development/before-artifacts与原日志；最终结果只绑定e6252e50。历史q7原件保留，没有换标签为本轮结果。

无效探索完整列于development/invalid-attempts.json：原noexcept异常退出不能算准确负例；缺开发者shell的编译负例环境错误；辅助Pane焦点/宽度错误；空Scene唤醒包未结束tick；关闭测试错误预期一份lease；源风格门拒绝；新atlas指针单位越界；取消的ddafb8eb资格；第一次font成本未进入font模式。Atlas越界由调试器定位到复制语句，改为字节指针后全量CTest及新clean资格重跑。所有失败日志保留，不把任意崩溃算通过。

## Unicode、glyph与真实IME

系统现有 `C:/Windows/Fonts/msyh.ttc`：SHA256 `d79c55e68b1131eea0cc1c47be4f572d964f28c682e143db2ad09c1e4cb07a3f`，19,704,352 bytes，face0，18 UI逻辑像素；ranges 0020—007E、3000—303F、4E00—9FFF、FF00—FFEF。没有下载/升级依赖，也不打包字体文件。默认字体保持旧行为；需要字体的示例和消费者通过显式外部输入装配。

本轮真实辅助UI测试确认：UiText的你好。，A保存为 `E4BDA0E5A5BDE38082EFBC8C41`；默认atlas512×64对四个选定汉字/标点使用fallback，配置atlas4096×2048的四个glyph实际存在且不使用fallback。释放调用方font/range后继续输入有效；frame递增、失焦与隐藏失效、另一个UI会话的anchor隔离成立。公开安装消费者也通过相同外部字体和UTF-8检查。辅助事件不是Windows IME。

真实n→i→数字1确认及内部三层Unicode、实际候选位置、另字/标点/取消组合、水平滚动与窗口/Pane移动、非100%DPI、本地文字Undo不穿透场景/其他文档，**本轮均未完成真实桌面验收**。q7看到“?”的原失败仍保留，不能凭本轮辅助glyph测试认定原问题仅缺字体。新原生Unicode转换路径没有因猜测而改写。

本轮Computer Use `@oai/sky`实际返回：`Computer Use native pipe is unavailable: failed to connect native pipe: 系统找不到指定的文件。 (os error 2)`；轻量重试及重建JS会话仍失败。按computer-use SKILL及其guidance恢复/停止规则停止桌面输入，未另造PowerShell/PInvoke/helper输入工具。手动输入请求未获得结果；自有诊断进程2636后来强制清理，空日志不算IME/正常关闭证据。`development/ime-before-incomplete.json`明确记录。UI通用输入测试不升级为真实IME通过。

IMM坐标策略核对的是COMPOSITIONFORM/CANDIDATEFORM的client-relative规则（[COMPOSITIONFORM](https://learn.microsoft.com/en-us/windows/win32/api/imm/ns-imm-compositionform)、[CANDIDATEFORM](https://learn.microsoft.com/en-us/windows/win32/api/imm/ns-imm-candidateform)）；实际OS候选定位和DPI行为仍待实测，不能凭调用代码宣布EX05完成。初始化限制：当前可信TrueType/TTC envelope校验不是任意恶意字体的完整sanitizer；ImGui/stb内部C malloc故障未注入，UI DLL C++分配扫描不冒称覆盖第三方所有分配。

## 固定候选资格

资格根E:/lux-er1/q8b；仅RelWithDebInfo。先验证tracked snapshot，再从固定提交独立clone配置。正常与诊断各自all -j4 -k0及第二轮no-op；构建后重新核对源内容hash，防止mtime掩盖改动。取消的E:/lux-er1/q8不是资格根。

|组|本轮实际结果|
|---|---|
|正常全量/CTest|构建通过、第二轮no-op；135/135|
|诊断全量/CTest|构建通过、第二轮no-op；152/152；断言显式启用|
|真实GPU矩阵|正常13变体、诊断31变体；另有foreign、application lifecycle和带字体100次UI循环，各自exit0|
|原相关回归|G01—G04、两View/FIF/顺序/resize关闭、旧图像池/晚到换源、实际seal及budget/历史/Context/Inspector/Graph相关已注册测试重跑；不替代下列剩余细分门槛|
|安装/迁移|五消费者在两个新前缀各自configure/all/no-op/test；UI各含默认和显式字体两个测试；读者meta重建/恢复、实际Toolset DLL加载路径验证通过|
|SDK闭包|消费者编译/链接输入排除源树、工作build、旧依赖安装目录，迁移后排除原新SDK前缀；公开头与候选完全匹配|
|隔离|7 DLL×正常/诊断共14身份；正常CRT分配来源、诊断replacement对象/PDB/exports分开；UI及EditorWindow正常DLL无selected-text诊断字符串，诊断DLL有；正常SDK无故障对象或字体文件|

正常主EXE SHA256：`8681ACD4753F8CEB608611E3A3D254FC5F9F8B781AFA9E232770F302ED397FA1`；诊断：`F530BC523D8CB78E4922C542C4B6CBAE0AB7C247F8E7BC4DF52A1E4F28CF9C08`。正常ui.dll：`BB65F5322DDBF050CC995E0735277FC37218C36AA4F91648D6EE6F4920443C25`；诊断ui.dll：`0DEE61E907CCF25DD2069C8ECFD44B182B2758999BF719F1F1615BB5EAE15B50`。其余DLL/EXE/PDB完整身份见qualification.json、diagnostic-qualification.json和qualified-isolation/dll-isolation.json。

构建驱动非零后禁止测试，真实style失败日志证明即使EXE已链接也没有继续启动。SceneGpuTest全局PASS只在全部业务判断、显式关闭及registry释放后输出；中间正常渲染/退役只称子检查完成。

## 有限成本

详见 [COST-q8.zh-CN.md](COST-q8.zh-CN.md) 和全部逐进程JSON/log/PPM。五组同量旧/新场景配对：work wall均值约-2.85%，wait cycles约+7.95%且五组全增，close wall约-3.36%。不宣布整体更快或性能等价，不把跨批q7/q8百分比相减作因果归因。

P02以同机新跑的q7固定正常产物与q8固定正常产物配对：64 Mesh/500 owner循环均值2.99454→3.00456 ms，约+0.335%；1024 Mesh为109.57966→42.4196 ms，约-61.29%。零View、空引用、相同checksum、稳定snapshot，不代表READY GPU吞吐量。索引payload下界另列，不冒称完整内存账目。

经实际font模式校验的五个独立进程：默认冷UI工厂均值0.68568 ms，配置字体146.98796 ms；A会话先存在，计量B的owning copy/atlas，排除文件读取/Renderer上传。第一次错误font模式样本原样保留且排除。全部构建、GPU、CTest、消费者和成本串行，无诊断计时混入。

## 仍未通过的原阶段门槛

- 实际thread/device/attach分阶段启动失败；已有owner分配/容量/UI atlas失败不能替代。
- 独立material上传失败；shader失败不替代。set_stopped与value各自真实完成路径仍缺。
- 专门shared-import/preserve实际GPU用例；两View与CPU barrier检查不升级为该资格。
- 真实IME及持续RMB/MMB保持跨失焦/隐藏/关闭；辅助UiText与轮式输入不替代。
- 对应关闭借用、部分owner访问器线程契约、卡住请求、完整资源内存账目和匹配resize/retry尾部。
- 原删除门槛不齐，旧editor_application/lux_editor/editor_scene/SceneWorkbench/UiVulkanPresentation/SceneViewRenderPort正式路径保留；新栈没有调用旧栈、runtime fallback或新增安装转发头。保留旧入口是未完成替代，不是完成资格。

ER-1续行候选交付并停止，等待独立审阅；不能把本轮核心/字体/资源测试通过称为ER-1完成。

## 交付身份

`source-and-reports.zip`来自固定报告提交的完整tracked源码；生产内容与e6252e50一致，diff限定报告目录。Git archive保存Git blob换行；source-files.sha256.csv记录实际clean Windows checkout的原始字节，二者身份不能混作同一原始SHA。

`normal-sdk.zip`、`diagnostic-artifacts.zip`、`evidence.zip`分开；evidence中的qualified/*对应本轮e6252e50，development/*包含修前/修后及失败探索。完整本地Git bundle以已上传dee90468为基线；完整源码ZIP无需该Git基线。每份归档、代码候选和报告提交分别在交付manifest.json记录；不以本机路径代替源码/证据文件。
