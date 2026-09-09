# ER-1 工作记录（未交付、未完成资格）

本文件记录工作树中的中间验证，不是 ER-1 验收通过声明。最终仍须独立 clean clone、安装迁移和逐项证据。

## 输入与隔离

- 工作树：`E:/lux-er1/src`，分支 `codex/editor-rebuild-er1`，输入 HEAD `9684a393122388f5b43f0f518e144380871416c9`。
- 主检出及其脚本优化修改、旧 SV1 工作树的未知 `engine.zip` 保留。未合并、推送、发布或进入 ER-2。
- 附件与 source-binding 原始审计位于 `E:/lux-er1/evidence/`；归档是文件身份，不冒充 Git commit。
- 本轮配置和运行均为 RelWithDebInfo。新路径不调用旧 Workbench/Context/Presenter；旧正式入口尚未满足删除门槛。

## 当前代码进展

- 已编译并运行真实 EditorWindow / EditorRenderer / SceneSession / SceneView / SceneWorkspace / 四个具体 Pane。
- Session 在转移 Scene 前准备初始查询；只读组件是 typed value copy；选择属于 Session；同一 owner cycle 不重复推进。
- Renderer 使用实际 Render channel、Vulkan offscreen target 和 FIF descriptor。图像引用保留真实记录；显式关闭保留 owner。
- 局部 HistoryActions 固定业务 target。HistoryMenuActions 在菜单打开时捕获 registration token，切换 active 不改变它。
- 当前 SceneResources 已验证真实缺失文件失败、保留原 AssetLoadFailure、显式替换序号重试及旧 token 拒绝。
- UI 实际检查发现“未就绪提示多占一行导致请求高度交替”的问题，已改为固定一行提示，并加入连续帧非空图像断言。
- Outliner 提供 Flat / hierarchy，缓存按快照身份重用，父子查找使用索引，环只在显示缓存中断边并说明，不修改业务数据。

## 已取得的中间证据

原始记录根：`E:/lux-er1/evidence/raw/`。任何后续改动都需要重新绑定最终源码及产物身份。

| 记录 | 实际结果与限制 |
|---|---|
| build-08 / build-08-noop | 首次新可执行程序完整 all 构建成功，第二轮 no work |
| ctest-01 | 15 个负例失败：未加载 VS 环境，缺 STL/CRT 头；保留失败日志，不算预期负例通过 |
| ctest-02 | 在 VS 环境中 128/128 通过，包含初版 SceneSession 协议测试 |
| build-10 / build-11 | 诊断程序的 meta 生成 job/输出重复，已拆至 test 子目录 |
| build-12 | 新诊断目标缺显式 Window 依赖，已补依赖 |
| build-13 | all 构建成功；GPU 诊断初版编译成功 |
| gpu-{base,dim,no_mesh,empty,alternate}-01 | 五种真实 Scene 数据、实际资源和 GPU 读回通过；每组两个 FIF slot、descriptor 2/2、零 validation 错误 |
| gui-01 | 真实窗口选择灯光、关闭和恢复 Inspector、保留选择、Alt-F4 关闭；发现上述视口不稳定问题 |
| build-14 | 菜单接入测试缺 Meta include、string_view 拼接和一处 121 列问题，已修复 |
| build-15 / ctest-protocol-03 | all 成功，新 HistoryActions 和 SceneSession 协议 2/2 通过 |
| gpu-{base,dim,no_mesh,empty,alternate}-02 | 固定提示高度后五组通过；图像 1014×593；稳定阶段每个 UI 帧非空，封帧拒绝保留输入，持有图像时关闭保留 owner |
| build-16 / gpu-retry-01 | 缺失 ground 包失败后挂完整包并显式重试成功；清空选择不影响资源接纳；恢复图像与 base-02 校验和一致 |
| build-17 / build-17-noop | all 成功，style / architecture 检查通过，第二轮 no work |
| ctest-04 | 129/129，57–58 秒量级的工作树验证；后续源码已变化 |
| gpu-dynamic-01 / gpu-cleanup-02 | 实际运行中变换/灯光/相机改变、移除 Mesh、清空 Scene；外来 Pane bad_alloc 后同窗口重试成功 |
| build-21 | 全量失败：GPU 测试一行 121 列；修正后 build-22 成功，未用失败构建作资格 |
| gpu-retirement-01 | 真实 RenderProgram 消费回执接入后动态场景、关闭、零 RenderErrorEvent 通过 |
| gpu-churn-01 | 测试在读取窗口外检查旧 token，触发 BUSY；这是测试时序错误，已保留失败 |
| build-24 / gpu-churn-02 | 6 槽预算下轮换 12 次，过期记录回收，原快照不变，序号不复用，图像每三次回到相同 checksum |
| build-25 / gpu-multiple-01 | 同 Session 第二 Workspace 注册、局部关闭、第一 Workspace 保留历史注册通过 |
| gpu-record-failure-01/02 | 接纳后注入 record preparation 错误，真实终态关闭暴露循环槽旧帧引用未释放，超时失败 |
| gpu-record-failure-03 | 旧帧引用修复后 Workspace/Session 已退出；Renderer 等待终态后不可能回来的 deferred release，超时失败 |
| build-29 / gpu-record-failure-04 | 所有循环槽在 worker joined 后清理；终态按真实停止事实完成；原 RenderError 17:1、accepted packet 349 保留，descriptor 2/2，所有 owner 释放 |
| build-30 / ctest-06 | 共享公共头同步后 all no work；完整 CTest 129/129，57.54 秒 |
| build-31 / gpu-frame-protocol-01 | 伪造代次/extent、同 token 不同 record、真实队列满、原 packet 重试、移动覆盖通过；base 图像校验和保持一致 |
| consumer-readers-build-01 | 测试使用了错误的 LUX_SKIP 宏；Windows.h 先于 Engine 头引入 VOID 宏冲突；均为消费者测试错误，修复 |
| consumer-readers-build-02 / consumer-readers-test-01 | 新 preinstall-01 独立消费者成功编译，1/1；11 个生成字段、真实 Inspector draw、Session/Pane 保留实际 LoadLibrary DLL、最终 FreeLibrary 恰好一次 |

`preinstall-01` 为工作树的预安装验证，不是 clean candidate 的最终安装资格。此后消费者导出签名、
最后 Pane 注册冲突测试和读者 TypeToken 校验又有修改，需要重编重测。

## 公共头同步

按 AGENTS 对三个前缀完成 15 个文件的核对与同步：UISession、UiFrameSnapshot，以及所需的已跟踪
Layout、UiInputEvent、TextureHandle。现存头先用 Git blob 证明对应仓库历史版本；不存在未知差异。
原件保存在 `evidence/install-header-backup/`，前后 SHA256 在 `evidence/install-header-sync.csv`。
未替换共享安装 DLL、未构建 Android、未改动主检出或脚本优化分支。

`images-stable` 下 base 两张图的校验和分别为 `13891510814548032513`、`17726626006466800328`；
`images-retry` 恢复后两张图与其相等。初版 `images` 分辨率不同，不能混作同量成本比较。

## 尚待完成（不能标 PASS）

- 完成逐项 A/C/U/H/F/R/X/I/P 映射；当前 CSV 尚未填最终结果。
- Renderer 真实错误身份、accept 后失败、终态释放、资源分配/计数边界注入及精确的有界 poll 账目。
- 运行中 Mesh/AssetId 改变、Entity 代次复用、旧结果和待提交 Scene update 的 CPU 寿命证明。
- 实际 MeshResources 已有 instance_refcounts 和 fence 后 retired_ranges；不能只以此推定“尚未消费的 upsert”安全。
  需证明程序发布/消费与资源销毁的次序，必要时在 Editor 内用真实 RenderProgram attachment 消费回执，不能凭帧号加常数。
- 资源容量回收、失败后状态不被迟到取消覆盖、UNREFERENCED、并行子失败等定向覆盖。
- 真实局部 context 路由、菜单 GUI、IME/文本焦点/捕获、持续 resize/隐藏/最小化与恢复、同 Session 多 View。
- 生成读者的独立插件消费者、通用 UI / Editing SDK 实际闭包、所有安装迁移与共享库身份。
- 满足迁移/删除门槛后切换正式 executable 并删除旧 Workbench/混合 Presenter/RenderPort。
- tracked candidate、ValidateTrackedSnapshot、独立 clean clone、all/no-op/CTest、安装迁移、同量成本至少五次独立进程。
- 三前缀公共头同步已完成（见上）；后续若再改 modules 公共头，继续备份及同步。

## 2026-09-09：窗口路由、安装消费者与真实分配失败

- build-32—36：增加实际 AssetProvider 延迟门，late_entity / late_close / late_source 分别验证 Entity 代次复用、关闭期间完成、同 Entity 换源；首次 late_entity 使用错误示例 AssetId 的测试失败保留。改为从实际 Mesh3D 取得源后通过。imageEvidence 依据实际提交 serial 与 Vulkan 完成水位，原 descriptor 保持不可变。
- gui-02（build-36）：实际窗口菜单显示只读 Undo/Redo 禁用；过滤输入 `k` 后无匹配，清除后列表恢复；切换 Flat 后无层级缩进，Cube 选择和 Inspector 保持。拖动分栏从 224 到 330 后真实图像恢复；最小化后恢复；滚轮改变相机投影；Alt-F4 正常关闭。原始日志 descriptor 6/6、view/lease/accepted 均零、validation/errors/dropped 均零。输入法合成与 Enter 提交已看到，未验证中文候选选择，也未验证持续 RMB/MMB 拖动失焦，不将这些标 PASS。
- gui-02 同时暴露 602.9 秒内 15,829,813 次 UI owner 循环。应用改为独立 8ms owner 节拍、最小化 16ms，无 GPU waitIdle、无积累追帧。此改动尚需实机重新绑定，不算已证明性能提升。
- build-37：将 Ctrl+Z/Y 的公共分派从 SceneWorkspace 移到 EditorWindow 的当帧 Pane 绘制完成后；固定目标仍由各 Pane 的 HistoryActions 持有。
- ctest-local-history-01：新增测试首次失败于初次窗口尚未全部创建就请求焦点。修正测试建立窗口的时序后 build-38 / ctest-local-history-02 两项通过：真实 Pane 焦点、Text/Record 两业务、空栈不回退、真实 InputText 空局部 Undo 栈不穿透内容历史。
- preinstall-02 与 consumer-{editor-ui,editor-scene,editor-scene-readers}-{config,build,test}-02：三个独立消费者全部配置、编译并各 1/1 通过。Scene consumer 已去掉旧 Workbench 依赖，直接构造不同的非视觉 Scene；插件消费者仍动态加载真实 DLL，生成 11 字段读者并实际绘制。
- build-39 / ctest-scene-allocation-01：诊断 allocator 链接在 Scene Session DLL 内，逐项拦截其实际普通/对齐 C++ 分配。12 个失败点均保留原 Scene 地址、标签、初选及组件值；同一输入在解除失败后成功交接。原始 stdout 单独保存为 ctest-scene-allocation-01-output.log。该计数不覆盖其他 DLL 的内部 malloc/STL，也不冒充 GPU 或全进程分配证明。
- build-40 正在加入 Window/Renderer DLL 的同类构造失败扫描，尚未验收。旧正式入口删除门槛、完整最终资格/安装迁移/成本仍未满足；不将中间结果标最终通过。

## 2026-09-09：实际失败边界与中间证据归档

- build-40/41 与 ctest-owner-allocation-01、gpu-factory-failure-02：Window DLL 8 点、Scene DLL 12 点、Renderer DLL 20 个 owner-thread 工厂分配点扫描通过。Renderer 空启动/关闭探针改用 10 秒实际 deadline，替代没有时间保证的固定紧循环次数。
- build-42 编译失败于测试 expected 错误类型不匹配；修正只提取已有 snapshot 值后 build-43/44 成功。gpu-factory-failure-03/04 均在真实 seal 分配失败中中止；stderr 明确索引 2，定位 AlignedAllocator 的 nothrow new/null→abort。F06 失败，补丁未应用，已请求 §3.3 白名单例外。
- build-45 / gpu-view-failure-01：RenderView 保留实际 RenderError、request 和 raw Target status；两个实际不存在的 RenderScene 产生两个不同 View 错误，diagnostic 容量 1、dropped 1；关闭不依赖日志容量。已成功创建目标的幂等释放 status 1 按 backend 原契约处理。
- app-paced-01（build-45）：2.628125 秒、240 UI iterations、252 render frames；FIF mask 3，descriptor 2/2，末尾 leases/views/accepted/events/dropped/validation 均零。仅单次 pacing 样本，不是同量成本比较。
- build-46 / gpu-multiple-02：测试把额外 View 的无 UI 引用图片误当成 UI snapshot 必须包含的纹理；修正负例触发条件后 build-47 成功。gpu-multiple-03 发现真实 Vulkan layout 错误，两个不同目标不能继承同一 Scene 的上一 View final layout。两 View 相机/extent 校验和独立，但 C09/H07/F15 不能通过。单文件候选补丁未应用，已独立请求白名单例外。
- ctest-07：遗漏 MSVC 环境导致 15 个 compile-negative 找不到标准头，未匹配预期错误，明确失败；不修改测试。补齐开发环境后 ctest-08 131/131 通过。
- build-48 / gpu-viewport-error-01：具体 SceneViewport 保存拥有数据的 action failure；失败取消 capture，成功的显式相机操作可清除旧错误。实际 closing View 的 extent 请求被拒绝，原 Session/Renderer 错误保留，owner 直到 COMPLETE 才释放；随后实际 Mesh/Light 图像与相机校验和和基线一致。
- build-noop-48：ninja: no work to do；ctest-09 完整 131/131 通过，42.07 秒。
- consumer-regeneration 独立复制中间 SDK 和消费者源码，未改旧前缀或原消费者。头、模板、编译宏分别触发生成；非法 widget 返回具体语义错误且不改旧输出字节/mtime；恢复后输出等同原始，CTest 1/1 和第二轮 no-op 通过。它不替代最终 clean/迁移资格。
- 更新全部验收/迁移行，修正 migration CSV 重复列；补齐 owner、能力、成本限制、源码/产物身份与实施中间报告。源码仍未形成最终候选提交，旧正式入口仍被删除门槛保护，Toolset 迁移和完整预算/失败/GPU矩阵/成本资格尚未完成。

## 独立审阅后的 ER-1 续作（未完成）

- 已读取两份 2026-09-09 review 附件，核对此前 2843 文件清单与受保护主工作区；审阅输入存于 evidence/continuation/input-audit.json。用户批准 EX-01/02/03，权限不扩展到 ER-2 或 main。
- build-49 新增预算测试首次编译失败（误用 ring beginWrite）；改用实际 tryBeginWrite 后 build-50 全量成功。ctest-50-diagnostic 的 Window/Session 分配及真实回复预算测试 3/3 通过。
- gpu-ex01-01：实际 Renderer DLL 工厂 20 点；seal 的 index 0—5 分配失败均结构化返回并保留输入，index 6 首次成功；原 index 2 已实际执行。仍需加强完整 snapshot/引用和临时资源计数，不能把旧薄断言写成所有权全面证明。
- gpu-ex02-01：原双 View 不同尺寸的实际 Vulkan 路径通过。build-51 扩展测试首次错误地把 target 分配顺序等同 SceneView 创建顺序；gpu-ex02-02-multiple_views 失败日志保留。初始 extent 明确在 startup poll 前提交后，build-52 与 gpu-ex02-03 四组通过：different/equal extent、reverse、resize/close-one。第一 View checksum 在第二 View resize/close 前后均 17726626006466800328；第二 View resize 到 384x192。尚不能代替共享 import/preserving layer 的 barrier-plan 回归。
- EX-03 实际 ring envelope 共用预算；7 envelope/6 callback/3 failure/1 unmatched 的 FIFO 序列为 0,10,20,1,11,21。预算单位不是 callback 数；仍需持续热 lane 和多 record envelope 覆盖。
- LUX_EDITOR_DIAGNOSTICS 默认 OFF；ON 仅允许 RelWithDebInfo+BUILD_TESTING。现 build 目录显式 ON，SDK/成本不得使用它。正常 OFF 全量和产物隔离证据待完成。
- EX-01 扩展审查修复 callback 登记后 payload 分配失败的残留及 cancel 分配风险；内部 slot/free-list 在登记前准备容量，不改 RequestId 编码。Program rebase、显式 Lease close、deferred release 准备返回分配错误；已发布 release 立即去除，重试只处理未发布部分。build-53 正在验证这些改动，尚未标通过。
- 既有被动 Lease 析构的 deferred vector 入队仍需检查预留保证；不得将 explicit close 修复写成全部 destructor OOM 保证。
- build-53 diagnostic 全量通过；ctest-53-client 两项通过。build-54 的新 barrier 测试访问 optional 编译错误，修正后 build-55 和 no-op 通过；ctest-55-diagnostic-all 134/134，57.13秒。DLL cleanup 10 实际分配失败点；compiler barrier 8组；预算 sustained 60次各lane20，加64record/1envelope。
- gpu-55 共11组实际通过：factory_failure、multiple_views/equal/reverse/lifecycle、late_entity/source/close、dynamic、churn、record_failure。保留每组原始日志与图像，不能用这些中间身份替代 final qualification。
- Toolset 移入 application/tooling，namespace lux::editor::application、唯一editor_tooling共享组件；新Application冷期借用、启动freeze、Session/Renderer结束后且Process/Asset释放前停止并销毁。Context/Inspector及测试消费者同步真实头/类型，没有alias。
- build-56/57 是新增生命周期测试的meta任务名/生成输出冲突；使用独立lifecycle构建子目录修复，不改generator。build-58 仅两行代码超120列；build-59全量和no-op通过，ctest-59三项通过。gpu-59测试漏配asset_read容量使失败阶段不符；显式配置64后build-60、gpu-60三阶段真实失败及显式关闭通过，工具stop/destruction各一次。
- normal-build 为新的OFF诊断构建，正在从源码全量编译；它是隔离检查中间产物，尚非tracked clean clone最终资格。

- normal-04 all/no-op、133/133 CTest 实际通过；normal SDK Toolset DLL 消费者通过并核对已加载模块。isolation-02 的 dumpbin/PDB module 证明正常 CRT 分配对象与诊断 replacement 对象分离。
- normal-05 部分资源迟到测试失败于测试假设 provider 只调用一次；实际 entered=2。保留失败日志；normal-06 要求全部 entered==returned，且关闭保持32步，实际通过。
- normal-07 新 pool 测试错误使用无渲染 feature 的空 RenderScene，未获得采样 descriptor，超时断言失败。迁至实际 Mesh/Light 场景后 normal-08 image_lifetime 通过：旧packet32步、零尺寸32步、backing 1/3、generation 1/5、最终descriptor 6/6，无验证错误。
- build-61 全量通过，gpu-61 factory: 20 factory + 6 seal 实际分配点；失败后全texture序列与每个image引用数恢复；索引2不跳过。gpu-61 lifecycle四阶段含冷关闭后拒绝start，工具停止/析构各一次。全部CTest结果见独立原始日志，不能用旧131计数重标。
- GUI持续按住RMB/MMB失焦与中文IME候选仍未实际验证；已向用户询问是否可手工协助。当前自动化接口不支持持续右/中键拖动。
- 最终正常SDK clean clone、相同输入五组配对成本、完整删除门槛仍未完成。保留旧入口，不作ER-1完成声明。
- ctest-61-all漏载MSVC环境，15个编译负例因缺少vector/stdint.h等失败；重载同一工具链后ctest-61-env-all 134/134，43.76秒。失败日志保留。

2026-09-09 续行：clean q1 的 image_lifetime 真实崩溃经 CDB + ASan 定位为新 DevelopmentScene 重复整组反射注册，03f7996e 修复增量队列入口并添加100次 metadata 构建回归。保留全部故障日志与 dump。q2 编译器误选 GNU模式 clang++，全量失败，没有运行程序；资格驱动改为显式 MSVC cl.exe，并在消费者构建继续固定现有 libclang 路径。

build-65：专用 RelWithDebInfo 分配诊断已恢复（ASan 调查产物身份另存），all/no-op及CTest135/135通过；真实迟到resize及仅GPU完成水位阻止关闭各五个进程通过。完整GPU变体和factory20/seal6实际分配点重跑，包含原seal index=2。仍未宣布ER-1通过。

续作 build-66：工作区仍绑定 a39bc827，受保护主检出仍为 c77bb41e，六个并行 Script 文件哈希复核一致。修正旧 editor_scene/lux_editor 的诊断编译开关，普通 BUILD_TESTING 不再打开 LUX_SV1_DIAGNOSTICS。专用诊断构建 all 与第二轮 no-op 通过；没有修改生产 C++ 实现。
q3 的 a39bc827 clean clone 实际完成 all/no-op、134/134 CTest、GPU矩阵、两个安装位置的五种消费者。随后扩大隔离审计发现旧 SceneWorkbench 诊断仍在普通 DLL，isolation-review.log 明确失败；因此 q3 仅保留原始通过/失败证据，不作为最终 SDK 资格。新资格驱动在安装前拒绝该污染，并对消费者编译及链接规则检查旧源码、构建及原安装前缀，实际断言消费者第二次构建 no-op。等待新 tracked candidate 的独立 clone 验证。

2026-09-09 续行：q4 的 bae70cde 独立 clean clone 正常构建 all/no-op、CTest134/134（80.11秒）、原/迁移SDK五种消费者均通过。扩展隔离审计包括旧 editor_scene.dll，普通 CRT 与专用 replacement 对象、导出和PDB来源区分通过。q4/installed-loaded-modules.json 验证两位置的实际 Toolset DLL；首次脚本误用exe名称的失败日志保留，修正后通过。q4/regeneration 的头/模板/宏/非法widget输出保留、恢复及no-op均通过。
新回归在 normal-11 实际执行：Packet move覆盖释放旧引用且不提交；资源读取阻塞32周期时先选择B再清空，原A仍以原请求身份READY；selection回调中Window/Workspace只登记关闭，Session close返回BUSY且owner保留；alternate实体顺序和数据真实GPU上屏。Session通知期间Undo/Redo错误优先级修正为BUSY。
同量成本首次完成时发现旧/新像素差异，追溯到新SceneView把网格原点当相机位置发送。修正新SceneView以匹配现有Render wire语义，未改modules。cost-smoke-05两边500计量帧+8验证帧、100预热、1024x576、单View、同seed，完整PPM一致，sha256=3683a239eb811dafa3d1faeee13910396bac8da77c03a1c693f3c2f03d98bd58。成本试跑的配置门禁、Windows宏污染编译失败和两次读回推进失败全部保留；没有把失败或中间计时算作最终五组。
build-67 all/no-op、135/135 CTest（60.78秒）、19个Scene GPU变体、foreign和四阶段application lifecycle均通过。原seal index=2实际重跑；factory20/seal6；image_lifetime同时验证Packet移动覆盖、迟到resize及真实GPU完成水位。normal-16 all/no-op包含工作/等待owner cycles计数的正常成本消费者。新的tracked候选与五组独立进程资格仍待执行；旧正式入口删除门槛和未覆盖失败/GUI项保持未完成。

q5 正常资格绑定 a6a1e0f6705beb0b434ed334b7d0bdc5a3ffcc36：独立 clean clone all/no-op、134/134 CTest（74.30秒）、九个Scene GPU变体、foreign、application lifecycle及两个安装位置五种消费者均通过。原始记录在 E:/lux-er1/q5/raw；qualified source-files.sha256.csv 来自该 clean clone 的实际 tracked 字节。Toolset实际加载模块路径审计、普通CRT/专用fault DLL隔离审计通过。
q5/cost-pairs 保存五组独立配对，全部500计量帧、8验证帧、100预热、1024x576、单View，checksum均11872603163728535614。主动work wall均值旧0.08250632秒/新0.07963108秒；等待cycles旧84251041.4/新471711888.2。源码核对发现新驱动逐帧等待录制，旧驱动只等待CPU接纳，因此这组数据保留为初步配对，不能用来声称相同等待策略下性能等价。后续仅统一两侧真实frame计数的等待边界并加work/wait轮询次数；这是第一项有限测量归因假设，未修改生产Renderer调度。
线程边界补查发现SceneViewport/Workspace的图像借用/释放以及Viewport输入辅助方法未检查外线程。入口现按dispatcher先拒绝，void调用不工作、图像借用返回空，actionFailure返回WRONG_THREAD且不覆盖owner错误；新增真实帧图像不被外线程释放、21个owner方法拒绝及七种owner复制/移动编译期检查。编译和实机结果待后续实际运行记录，不能沿用q5标签。
normal-17 all/no-op通过；base/image_lifetime/reentrant_close实际通过新的线程边界回归，21个有结果owner调用均返回准确WRONG_THREAD，窗口/Session/history/View注册保持；实际Frame图像和Pane错误未被外线程清除。cost-smoke-06统一per-frame-recorded边界，两边500个work poll、500计量帧、8验证帧、相同像素；单组仅作驱动验证，等待新tracked候选五组，不据此报告收益。
q5的同一a6a1e0f6 clean clone另建专用diagnostic-build，all/no-op、135/135 CTest（76.23秒）、19个Scene GPU变体及foreign/application lifecycle通过；factory20、seal6（含index=2）、Window8、Scene12、client10真实分配点均重跑。qualified-isolation对同一源码身份的两类DLL/PDB分别取证。该资格不含后续线程辅助方法修复。
