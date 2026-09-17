# D3 桌面收尾交付

代码/测试候选：`e936aafc99e4cea80a94c3b6c39b4c8a5094c71f`，分支 `codex/editor-d3-foundation`。本报告提交与候选分开记录。起点为 `3f3407a5668174744f5c5eaccf8eea741a830f54`，原 JR、帧引用、readiness 布局和修饰键修正保持不动。

本轮补齐了两种真实 Pane 的生命周期/排队工作组合、一个明确的 FAILED View 桌面重开路径，以及有限规模与尾部观察。持续按住鼠标时取消和裁剪中的持续手势仍待人工结果，**不声明完整 D3 产品验收通过**。

## 实际代码变化

- 扩展已有 `core_protocol.cpp` 的 `pane-lifecycle`：Inspector 和 Outliner 实际退出、重新注册；没有增加新测试框架或替身 Pane。
- 复用 `LUX_EDITOR_UI_DIAGNOSTICS`，记录两种 Pane 的真实选择回调，以及 Run View 的失败、关闭和建立。仅诊断 Rendering DLL 可将一次指定 View 的 AddView 请求源改为无效值，错误由真实服务端返回。
- 在 `EditorWindow` 的既有 Impl 中增加成功 CPU snapshot 捕获计数和只读查询；计数在 capture 成功后增加，重试、失败、GPU 完成不增加。现有成本驱动据此计算实际 UI 帧，另记重试调用。
- 在既有 resize/dynamic Run 驱动中补几个 owner 观察时间点；既有结构保存流程接受显式对象数。不改 Run、Binding、GPU 退休、输入、Editing 或 Process 协议。

没有新增 Manager、Context、Adapter、事件总线、调度器、OOM 工程或运行模式，没有升级依赖。

## C19：对象退出与排队工作

真实流程为：已有五个 Scene views → 同时对 Inspector/Outliner 排入携带旧 ObjectWeakRef 的工作 → 请求关闭 → attach 准确返回 BUSY → 在前端帧结束后的正常 document owner 点推进 → 两个旧对象实际析构，剩余三个 view → 修改内容/选择 → 建立两个新 Pane → 再排入真实 document 选择请求。

检查结果：

- 旧对象销毁前队列尚未消费；之后两份 weak target 均 expired，`getOnCurrent()` 返回空，旧排队工作未接触对象。
- 两个 Pane 缺席期间的选择变化没有旧回调；重建后的同一次选择变化，实际 Outliner 和 Inspector 回调各一次。
- 文档和历史未被关闭，其他三个 view 保留；重复 attach 不增加 view，修改与 Undo/Redo 继续有效。
- 新 Pane 绘制读取当前选择；目录在必要时重建，后续静止帧不重复查找绑定/重建目录。
- 最后真实 document 退出，驱动记录 views=0、leases=0。

证据为 `diagnostic-c19-raw.log`、`diagnostic-c19-result.txt`、正常安装 CTest `editor.d2.pane-lifecycle`。具体 Pane 的订阅仍为已有直接 ScopedConnection；本测试是“排队 owner 工作触发真实通知 + 旧 weak-target 工作失效”，**不声称把 Pane 订阅改成了排队信号**。这是实际对象/owner/队列集成验证，不冒充用户点击 X 的物理销毁流程；X 原本仅隐藏。

## FAILED View 与桌面回归

最终 `e936aafc` 使用 Computer Use 经 Windows 输入路径操作正式 EXE；属于真实桌面输入，不是 Pane 方法注入，也不称为人手硬件测试。

| 产物与流程 | 实际结果 | 原始证据 |
| --- | --- | --- |
| 正常迁移 SDK：Play/Pause/Step | Pause 稳定在 607，Step 一次到 608 | `desktop-final/actions.json`、截图 |
| 正常迁移 SDK：隐藏/恢复/Resume/Stop | 点击 X 隐藏活动 Run；Restore 后图像恢复且仍为 608；Resume 到 618，随后 Stop 最终为 Run finished，1463 步 | 同上；仅隐藏/恢复，不替代 C19 对象析构 |
| 诊断 SDK：FAILED View | 显示 Renderer 7、View 1:2、request 1224736769、backend 80:1；对应真实 AddView 源 Scene 不存在 | `desktop-diagnostic-final`、`desktop-recovery-result.json` |
| 诊断 SDK：Reopen | Pause 在 440；旧 View 1:2 已关闭后才建立 1:3；Run serial=1、pins=3 保持，图像恢复；Step 到 441；Stop 最终为 Run finished | 诊断 stdout 的身份顺序、Windows 输入截图 |
| 两种 SDK：明确 native X | Run 完成后明确点击主窗口 X；均记录 native-close-flag → review-begin → review-commit → finished code=0、failure=0、joined=1 | 两份 stderr、exit-code；不是仅凭 exit0 推测用户关闭 |

错误刺激仅存在于专用诊断 Rendering DLL：一次 invalid-source AddView 经现有 Control 客户端提交，服务端 `handleAddView` 返回 `err::scene::NotFound`，随后按已有 reply、FAILED、关闭、重开路径运行。没有直接写 FAILED、伪造 GPU 完成或提前清空 owner。

覆盖边界是**首次建立 View 被服务端拒绝后，用户点击 Reopen**；不代表物理 device loss、已有图像后的所有 resize 失败、多窗口/DPI 等组合都已通过。View 局部失败没有被误写为独立 Run 的终局失败。正常/诊断 Windows 输入、隐藏窗口 GPU 驱动与接口注入分别记录。

此前 `3c82ad15` 的实际 Material `n→i→候选→Space` 输入“你”、UTF-8 保存、局部文本 Undo/Redo 且 Scene X=4.5 不变、退出取消后继续使用及保存退出，继续登记为该候选的**限定通过**。原始动作/截图/保存数据随包放在 `accepted-3c82ad15`，不把它们重贴为新二进制结果，也不再笼统称为全部未测。

## 正常 SDK 与诊断隔离

最终候选从独立干净检出 `d3q/s`、既有增量构建树 `d3q/b` 构建；使用新 `d3desktopq/qualified/sdk`，复制到新 `qualified/relocated` 后配置消费者。全部为 RelWithDebInfo。

- `all -j 4 -- -k 0` 成功，生产与消费者第二次构建均为 `ninja: no work to do.`。
- **44 项安装 CTest 全通过，99.53 秒**。包括现有保存/发布/冲突/恢复、生成组件、C19、资源关联、线程、Run 终止/首错/进度回归。
- **9 个独立消费者全通过**；实际 include/link/DLL 闭包检查通过。Core/Material/Flow 业务消费者不含 UI/window DLL；Scene 仍存在既有 editor_rendering→ImGui/GLFW 依赖，不冒称纯 CPU Scene SDK。
- 生成器缺失指定组件时准确拒绝，原生成文件逐字节保持。

44 与 9 是不同统计单位，不能相加成完整验收数。记录见 `q-*` 原始日志、`q-LastTest.log`、`dll-closure.json`、`generation-preserved.json`。

诊断版以同候选正常 SDK 为依赖底座，仅替换专用 RelWithDebInfo 构建的 `editor_scene_ui`、`editor_rendering` DLL；测试组件复制计数在另一个诊断 consumer 构建中启用。完整组成和哈希分列记录。正常 SDK 的 UI/publication diagnostics 均 OFF，二进制中不存在新增诊断 marker/故障环境变量；诊断版 UI diagnostics ON、publication diagnostics OFF。没有新增 replacement new/delete 或 OOM 注入。诊断打印/复制计数的耗时不纳入正常性能样本。

## 有限规模与尾部成本

每组五次独立进程，全部样本保留在 `cost-samples.csv`、`cost-summary.json` 与原始日志。下列中位数仅描述这组实际输入，不是加速比例、FPS 或逐操作 p95。

| 观察量（五组中位数） | 较小规模 | 较大规模 |
| --- | ---: | ---: |
| 100 个实际 UI 帧主动 draw/capture/seal | 256 对象：12.719 ms | 4096 对象：12.518 ms |
| 同批背压重试调用 / 主动耗时 | 177 次 / 0.054 ms | 179 次 / 0.054 ms |
| 结构编辑批次主动耗时 | 256 对象：2.5294 ms | 4094 对象：36.5868 ms |
| 100 次生成 Inspector 绘制 | 256 元素：9.2436 ms | 4096 元素：9.3436 ms |
| 100 次单元素预览及一次提交 | 256 元素：23.4 μs | 4096 元素：22.1 μs |

| 尾部观察（五组中位数） | 时间 |
| --- | ---: |
| Stop → 系统退出首次观察 | 1.048 ms |
| Stop → Main 收到 worker 最终结果 | 2.237 ms |
| Stop → View 退出 | 67.264 ms |
| Stop → drain 接纳 | 76.935 ms |
| Stop → Run 资源全部退出 | 91.048 ms |
| resize 中 packet 重试区间 | 2.1468 ms |
| 保留旧图像等待区间 | 45.4156 ms |
| 释放旧引用 → 新图像就绪 | 6.2506 ms |
| 新图像就绪 → View 关闭 | 12.3344 ms |

诊断观察与上述正常计时分开：256/4096 对象最后 100 个实际 Pane 帧均只提交 32 个 Outliner 行；这 100 帧 Outliner/Inspector 的目录重建均为 0，Inspector 绑定查询为 0。每组完整 126 帧中 Outliner 仅重建一次，见 `pane-work-observations.json`。行提交量不等于全流程只遍历 32 个对象；结构准备仍存在随总对象数增长的工作。

UI 对象规模为 256/4096、窗口 1600×900、一个场景 View；只有前四个对象包含组件（四个 Transform、三个 Mesh、一个 Light），额外对象为空作者对象，不能外推大型渲染场景。100 帧计时包含 draw/capture/seal 路径的主动工作，重试主动工作、整个前端回调以及 wait 分开记录，不是 Outliner 独占计时。

结构批次起点为 256/4094 个对象，均完成 76 次 history revision 变化，验证引用/循环拒绝、创建/删除/恢复、随后保存重开。4094 是为既有 4096 上限预留两个临时对象槽，不修改上限。已保留全目录/身份准备和引用安全扫描；本轮没有逐点统计全部扫描、编码字节或 allocator 次数，不声称结构操作变成只与编辑对象数相关。

容器采用相同测试组件，256/4096 元素，各做 100 次单元素预览和一次提交；仅新增一条历史，计费 retained=422 字节。另行诊断的 commit-adoption Settings copy 计数均为 0；这不代表预览过程无复制、复杂字符串不分配或全进程内存用量为零。生成控件样本均预热 20 次、实际绘制 100 次，1000×700、一个场景 View。

动态 Run 五组均为 7 步、发布 6 包、一次必要发布背压；validation 关闭，驱动有意停止消费约 140 ms。控制/退出时间是 Main 的首次观察，含约 1 ms 轮询；drain 接纳不是 GPU 完成。Resize 尾部保留 validation 开启，两个 View、九次尺寸请求（256×128→320×192），同时检查旧 CPU lease 与真实 GPU_COMPLETE，再释放旧图像并关闭；它是带验证的有限协议耗时，不与无验证样本直接比较。

### 修正的测量口径与保留的失败记录

`6f719b6` 旧驱动报告的 100 draws 实为 100 前端调用，其中可包含背压重试。诊断看到实际 Pane 绘制只有约 47/50 次，故没有将旧约 4 ms 与新 100 个真实 UI 帧的时间比较。旧样本与工具完整保留在 `pre-count-6f719b6`；`cfe0f6d` 增加只读 capture 事实；最终 `e936aafc` 仅再允许既有结构驱动接收对象数。

首次用满额 4096 对象运行“还需创建两个对象”的结构脚本，驱动在预期创建成功的断言处退出。源码中的既有上限拒绝是明确边界；此记录只作为输入不满足成功流程的驱动失败保留在 `scale-capacity-mismatch`，不算准确业务负例，也不归咎为新产品缺陷。之后使用新项目输入 4094 对象，五组通过。

另外保留工具限制：一次计数分析错误地要求旧调用计数样本拥有 100 个真实 draw，因而触发分析断言，这正是上面的口径修正来源；一次 PowerShell 变量紧邻冒号导致脚本解析失败，未运行测试；一次运行 PATH 收窄后同 shell 找不到 git，没有发生提交，恢复独立 shell 后才提交。它们都不是 Engine 缺陷，没有运行构建失败后的旧 EXE。

## G1—G6 与待验收范围

| 门槛 | 当前结果 |
| --- | --- |
| G1 归属与内聚 | 保持原架构，仅现有 owner、诊断接点和测试的窄改动 |
| G2 定向正确性 | JR 已结案；C19 两种 Pane/真实队列组合及相关回归通过 |
| G3 桌面工作流 | 指定 Run 控制、隐藏/恢复、FAILED View/Reopen 已留证；持续按住鼠标取消及裁剪中的持续手势仍待人工 |
| G4 异步/发布 | 最终候选相关 Process/保存/发布/关闭回归通过，未扩产品语义 |
| G5 工程资格 | 正常安装/迁移、44 CTest、9 消费者、生成/闭包、诊断隔离和身份已交付 |
| G6 有限成本 | 实际帧、对象/容器/结构两种规模及 Run/resize 尾部五组记录；完整大场景、深层分配总账和统计尾分位仍未证明 |

手势余项见 `D3-Desktop-Manual-Checks.zh-CN.md`。Computer Use 仅提供完整原子 drag，没有独立的持续按下接口，本轮没有用“拖完再 Escape”充当通过。历史来源未明的 native-close 保持未解释；本轮明确 X 只说明本次动作，没有将历史记录升级为当前确定缺陷。

实际交付为 `LUX_D3_Desktop_e936aafc_source-and-reports.zip` 及 SHA256 文件，含完整候选源码、报告、截图/动作、原始日志、可复用工具和逐文件 manifest。`source-identities.json` 分列 Git blob OID/blob SHA256 与 Windows checkout SHA256，`artifact-identities.json` 区分正常 SDK、消费者和诊断产物。

main 保持 `4aee84a48e235174534ca4a0fe0cc6afe39cfcd7`。仅推送当前开发分支，不合并 main、不发布或冻结，交独立审阅。
