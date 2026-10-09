# Lifecycle LR07：结果与完整对象取代重复有效标记

最终实现 `2619986ddf9b61529dcacd169b41aac77c92f45a`；lux-cxx
`0a0e7419fc7229df6e372cd35a540249f92250ef`。本阶段对应原 LR00 清单的 26 条责任、11 组提供者。
LR07 实施及固定提交的 Windows/安装/GPU 资格完成；整个 LR/MA 计划未完成。

## 实际迁移

| 提供者 | 唯一表示与责任 | 保留的真实协议 |
|---|---|---|
| RenderRequest | 原 shared State 中一个 optional Expected 表达待完成/成功/失败；结果回调传递原错误 | 原请求身份、观察取消、后台完成和容量；重复完成不覆盖首个终态，回调可销毁当前外壳 |
| Flow LastLink | 没有非空生产者，删除类型、五处虚接口参数/原体及实际编译器调用 | 原图链接冲突、显式 unlink/relink 和作者身份；不新增无用 optional |
| Renderer completion watermark | 一个 optional serial 区分尚未观察与真实序号零 | 原 fence/serial 退休和 driverless fallback |
| Detour portal | 原 BFS 前驱表保存 optional traversal，起点无前驱 | 区域/查询实际状态、路径顺序和退休；无额外容器或分配 |
| ScriptAsset result | 原有界记录保存 monostate/raw/decoded 的 inline variant | 完成接收、预算、撤销和代码 pin；先取出 owner，再从槽位删除，清理回调不进入容器修改中途 |
| Scene timer | inline optional ActiveTimer 拥有 deadline、stop source 和连接 operation | 原完成原子量、唤醒及取消等待；不移动活 operation、不新增独立分配 |
| Shadow feature | 原 ATTACHING 阶段和所属 RenderScene 是安装权威 | 原质量替换、失败清理、feature 事务与 GPU 退休；重复安装准确拒绝 |
| HookPoint | 构造时取得容量完整端点，删除 prepare/prepared_ | 原 SlotMap 代际、dispatch guard 和脚本授权；普通堆 OOM 保持 fatal |
| HookChannel | fallible factory 发布完整、固定地址的 unique owner；inline optional Composition 合并所属 owner/system/hook | 原 lane、seal、failed、overflow/discard 和后批生产；生产安装器原本已有同一唯一分配 |
| Swapchain acquisition | expected optional 完整 image payload；pending extent 为一个 optional | 原 SUBOPTIMAL/rebuild/native failure/信号量轮转和物理退休；fallback extent 只在需要时查询 |
| Compiled graph | expected 成功图或 owning failure(cause,candidate) | 原资源有效表、条件、队列及退休；失败保留诊断和候选所有权，cache 保留最后成功图/布局/视图 |

没有新增通用 manager、第二退休队列或结果表。HookChannel 的独立调用方式从可默认构造的栈对象
改为稳定唯一 owner；完整 fallible construction 和 producer 地址寿命是明确依据，不宣称所有调用零分配。
RenderRequest 实际对照为同一次分配 120→104 字节、一次释放与一次回调；这只描述该夹具，不扩大到全局性能。

编译器删除 `RGCompiledGraph::valid/compile_error`、无失败分支的 bool 返回及 producerless
`CompiledGraphInvalid`。失败传播使用原精确错误；QFOT 仍明确拒绝，不把清掉不可达代码描述成多队列新功能。
SceneGraphCache 仅采用成功图，失败和成功两条路径都保留原诊断输出。原有效资源/跨度不是构造标记，未删除。

## 行为依据

真实修前产品负例包括 RenderRequest 伪造成功值，以及 Shadow 在已安装状态接受再次安装。
其余正向基线用于证明原功能保留，不伪称修前失败。夹具的错误 enum、未指定 attachment format、错误 buffer role、
LogicalDevice 复制、安装前缀假设等首次失败均保留，并按夹具问题说明，未降低生产断言。

原 native compiler/cache 夹具覆盖循环、缺失 import/producer、缺失/过期 compute pipeline、跨队列 QFOT、
条件生产/消费、attachment plan、全条件组及条件 CLEAR/local-read 拒绝；候选移动后的内部指针、名称、警告与
回调 owner 清理仍正确。实际 cache 拒绝新图后原图和布局保留，后续成功能采用。该夹具自身不执行 draw；
实际呈现由原 source/SDK GPU 路径验证。

原生 WSI 夹具使用真实 Win32/Vulkan device 和 acquire/barrier/submit/present；注入 TIMEOUT/NOT_READY/错误、
OUT_OF_DATE/SURFACE_LOST 与实际 acquire 后 SUBOPTIMAL，验证 image、cursor、extent coalescing、零尺寸恢复和
GPU 阻塞下原退休顺序。错误注入不等于物理设备丢失或系统输入接管实测。

HookPoint 的容量/空回调/一万次旧代拒绝，实际 Simulation HookChannel 的三步生产/后批/溢出，Scene timer
八次 pause/cancel/rearm，真实脚本资产 DLL cleanup，Detour 多跳/循环/卸载重载和 Flow compiler 冲突/重连均保留。
这些子闭包执行结果按各自 SHA/原工作区哈希归属，最终 clean 检出结果另列。

## 最终固定提交资格

ValidateTrackedSnapshot 后独立 clean tracked 检出，构建 `all -j 4 -- -k 0`；实机与构建串行。
六处用户差异没有进入资格源码。全新 SDK 不借开发 build DLL 或源码私有 include。

| 验证 | 结果 |
|---|---|
| Editor all / no-work / CTest | 1244 步 PASS / no work / 118/118 |
| PLAYER all / no-work / CTest | 1147 步 PASS / no work / 58/58 |
| 原安装消费者 | 全新 SDK；192 步 all、no work、16/16 |
| 本阶段安装消费者及公共头 | 82 步 all、no work、10/10；12 个独立 C++20/noRTTI 公共头 |
| 实际 source/include/link/install 闭包 | Editor 658、PLAYER 602、原 SDK 59、本阶段 SDK 24 个 TU；无开发 DLL/源码私有头回退 |
| 相对归档搬迁 / 缺失 / 篡改拒绝 | 中文/空格路径通过；缺失和篡改真实 SDK 输出均拒绝 |

归档位置 `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr07/verified-evidence/`。
共 1629 个文件、157 条命令、19 条最终必需命令；Manifest SHA256：
`8ea01256a600dd4299fc5747b7d32a6c062c951848955bae86a0909f8617a0d3`。原十份收据源码按 Git 实现对象核验，224 份输入证据哈希一致；
混合换行恢复同时要求原精确哈希与 Git 内容一致，不以当前源码替代历史证据。
完整 Editor CTest 中 37 个 gpu、8 个 desktop 标签项通过，不等于系统输入/IME 资格。
修改的 modules 公共头已同步三个规定 include 前缀；Android 仅同步头。Scoped RenderRequest ASan
按原闭包 SHA 继承，未冒充本轮全后端插桩。最终取证输出为 `runs/logs/lr07-qualified-verify.log`。

Linux 按用户明确决定保留 NOT_RUN/未通过，不配置 WSL，不用 Windows 代替。LR08 最终 sanitizer 等资格和
MA00–MA11 尚未完成。`Q-LR03-HOST-MINIMIZE` 保持 OPEN；原蒙皮跨帧 WAR 责任不变。原生输入继续
NOT_RUN_USER_DEFERRED，IME 未测，不补旧 50k 性能长测。

LR08 全清单复核还发现原 LR02/LR03 组合条目中残留的 transient ring、triangle overlay、mesh shadow、
UI sampler 和同步 GPU transfer 手工清理路径。它们属于后续定向补正，不能由本次 LR07 类型状态资格
覆盖或宣称完成；graph allocator 的真实叶子释放与 pool 转移也继续逐项核对。

工作区 `E:/SyncForder/CodeRepos/lux-engine`、分支 `codex/editor-framework-v2`。六处用户差异保持原哈希，
ProjectBuilder 补丁未应用，main/历史快照未修改。完成后按授权继续 LR08 和 MA，不等待单阶段复审。
