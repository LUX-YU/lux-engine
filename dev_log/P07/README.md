# P07 验收与交接

只实施 P07，停在 P07 等待复审；不进入 P08。前置验收为 `38fd05d551e4556f9d713b793de05f98db1bd156`。
准确实现 SHA、命令、退出码和归档校验和见 [receipt.json](receipt.json)。本文件与同目录账本是阶段末冻结快照；唯一可变施工材料仍是 `.internal/editor-redesign/`。

## 实现与唯一 owner

| 范围 | owner 与实际职责 |
|---|---|
| 作者源 | 原 SceneSession、MaterialSession、FlowSession，以及原 History、SessionState、SessionStore；本轮未修改 |
| 作者投影 | ScenePresentationHub 按会话／历史／配置／环境版本复用有界记录；多个使用者共享一个 SceneProjection，SceneRuntime 仍拥有重实例 |
| 视口 | ViewportPresentation 独立拥有相机与输出需求、输出引用、关闭回执；只借用 Runtime，不驱动另一轮 tick |
| 高亮与工作平面 | 私有 HighlightRenderer 保存 desired/prepared/accepted 和拒绝信息；实际后端以完整 ViewHandle（含代际）保存每视口选择及平面参数 |
| Material | MaterialCompileOperation 拥有冻结源、配置与完成事实；MaterialPreviewStore 拥有候选／已采用资源及其真实版本 |
| Flow | FlowCompilationService 有界拥有冻结源、编译对象、链接尝试和产物；retryLink 不重新读取作者图 |
| 派生发布 | 两种发布操作借用调用者给出的同一 P05 WriteCoordinator，并由 SaveExecution 执行 IO；不持作者保存角色，不采用作者 checkpoint |
| 驱动与退休 | 原宿主统一调用原 SceneRuntime；各实例沿原 lease/retirement 释放，不另建执行器、Runtime 或全局结果管理器 |

三个主题各一个 STATIC 编译边界：`scene_projection`、`material_preview`、`flowforge_compilation`。新增的纯派生模块不引用旧 Editor、EditorContext、ProjectStorage 实现或过渡桥。

共享投影复用既有 `buildSceneSnapshotPackage`，不重复实现分区打包。当前对任何已变化的内容（包括 DELTA）采用完整不可变快照重建；游标未变化时不工作。RESET_REQUIRED 明确重建，不能卡在旧 Registry。每条记录最多当前实例与一个退休实例；退休未完成时不继续堆积替换。无使用者记录仍计入 Hub 容量，完成退休后回收。默认容量 16，测试以容量 2 持续复用。这里没有宣称已经实现组件级增量优化。

## 提交、预览及完成交付

高亮捕获失败不会前移 accepted，相同 key 可以重试。背压保留整个 program 及提交寿命；新 desired 释放未提交旧候选，包括切回已经 accepted 的选择。永久拒绝与背压分开，依赖版本改变或显式 retry 后再尝试。后端接收与 GPU 完成独立。

HighlightFeature 的 target 表和 Grid3DPassFeature 的参数表实际按完整 ViewHandle 查找；录制 kernel 从当前 view 读取对应记录。删除视口同时清除该视口记录，重用 slot 的不同 generation 不继承旧选择。两个视口继续共享场景和 mesh 数据，没有复制整图来获得隔离。

Material 后台仅持有 MaterialSnapshot 的拥有型来源，真实调用 material compiler 并编码一次。MaterialCompileId 与 TaskId 分离；采用必须匹配 content、settings、environment 和 preview target。迟到 S10 不覆盖 S12；较新的失败不使迟到旧结果冒充当前效果。最后成功效果保留原 stamp，状态明确标记 stale／失败。预览使用真实球体、灯光、相机和现有 RenderResources 路径。缓存身份来自 Runtime 的完整实例域、slot、generation，避免 DLL 私有计数器导致不同预览共享错误资产命名空间。

Flow 在 CPU 生成真实 FlowForgeObject，经 blocking scheduler 调用 linker，再回 CPU 编码。记录持有固定源和元信息环境的代码寿命。链接失败仍保存原编译对象；修改作者图后 retryLink 只改变链接设置与尝试记录，原对象和捕获 stamp 不变。默认最多 16 个记录，每条最多 8 次链接尝试，输入／产物字节上限默认 64 MiB，超限明确失败。

Material/Flow 的已准入完成只写入自己的操作记录；不会因为新业务准入 BUSY 丢失，也不会解除外层 dispatch。旧窗口的 notified ID 仅区分已经采用通知的 UI 事实，不另存编译结果或工作状态机。销毁操作取消请求并释放自己持有的 Task handle，避免 executor record 与完成 state 相互持有；执行器继续保有晚到完成所需数据。

派生发布使用已有协调器目标 lane、冲突与 Unknown 协议。与源码保存不共享可继承的保存来源身份。真实 IO 测试验证同目标冲突、实际落盘及 Material/Flow 作者 current、dirty 不变。

## 删除与有期限暂留

完整新增／修改／删除列表见 [FILES.md](FILES.md)，逐文件 Git 内容哈希见 [files.json](files.json)。逐成员处置写入冻结的 migration-ledger.json，而不是另建第二份活动账本。

已删除：

- SceneEditor::Impl 的 updateHighlight、updateWorkPlane、submit 原算法，以及 highlighted_selection/structure/feature/instance、highlight_pending、highlight_program、work_plane_pending、work_plane_program。
- MaterialEditor::Impl::Compilation/Preview、compile_task_/compile_result_、旧 CompileWork 与旧私有 MaterialCompilation.hpp/MaterialPreview.hpp。
- FlowForgeEditor::Impl::Compilation、acceptCompilation、compile_task_/compile_result_，旧 CompileWork/LinkWork/PackageWork 及私有 FlowCompilation.hpp。
- 旧 SceneElement 内的视口资源/输出/关闭实现，统一迁到 ViewportPresentation；SceneElement 保留 UI 显示与输入。
- AssetSave 中已无消费者的 TCompiledAsset/encodeCompiledAsset 重编码辅助函数。

暂留到 **P12**：

- `editor/transition/MaterialCompilationAccess.hpp`：只供旧 Material 窗口固定输入、源 IO 编码和结果转换；不安装。
- `editor/transition/FlowCompilationAccess.hpp`：只供旧 Flow 窗口固定输入／环境和结果转换；不安装。
- SceneEditor 的 renderFor/inspectedRender 查找、打开输入转换、资源面板身份／版本显示；资源状态与重试调用新窄接口。
- Material/Flow 窗口的 TaskId 公共 UI 接口、已通知身份、编译展示名称与固定资产读取输入，以及调用新服务的相机／预览薄适配。
- 原窗口 `requestPublish` 的 **源码 + pak 组合保存** IO 适配继续遵循旧源码保存协议；这不是新派生发布接口。新 PublishCompiledMaterialOperation/PublishFlowArtifactOperation 只发布派生产物，绝不清理作者基线。

以上适配都有实际旧窗口消费者；纯编译、链接、视口和高亮算法不保留第二份。三作者模型、History、SessionState、保存服务、WriteCoordinator、SceneRuntime 和 RunStore 的算法均未改动。

## 实际验证

| 门槛 | 执行内容与证据 |
|---|---|
| X07-01 | 同 key 捕获失败后真正重试，accepted 不提前变化；`logs/highlight-detail.log` |
| X07-02 | 生产 HighlightRenderer + RenderSubmissionState，控制传输背压与 GPU serial 完成；候选／在途寿命、替换与拒绝；`logs/highlight-detail.log` |
| X07-03 | 实际 HighlightFeature/Grid3DPassFeature 后端记录、完整 view 代际、释放和参数隔离；`logs/backend-binding-detail.log`；录制路径随源码审查 |
| X07-04 | 真实 Material 编译，阻塞独立 CPU scheduler 安排乱序，S12 失败、内容／配置／环境／目标失配；`logs/compilation-detail.log` |
| X07-05 | 真实 Flow compiler、缺 linker 失败、随后真实 lld-link 重试固定旧产物；同一日志 |
| X07-06 | 真实 ProjectArtifactStore/SaveExecution/WriteCoordinator 文件发布、冲突和作者基线不变；同一日志 |
| X07-07 | 实际 SceneSession/SceneRuntime，RESET_REQUIRED、共享投影、暂停时钟、真实退休、32 轮有界复用；`logs/projection-detail.log` |

原 126 项测试及原 C++ 测试体／断言逐字保留，增加 5 个 CTest 入口。最终 Editor **131/131**；PLAYER 原 **11/11**；原十组 SDK 消费者加新独立派生服务消费者，共 **11 组、44/44**。最终 all 构建使用 `-j 4 -- -k 0`，第二轮无工作。旧产品 GPU、Material/Flow 窗口、工厂失败收尾及安装消费者均重新运行。

新增 17 个真实 CMake 依赖夹具覆盖直接／传递旧 UI、Context、桥、旧 storage 依赖及作者模型反向依赖；禁止边失败在指定规则，同夹具去除非法边后通过。此前的全部依赖负例继续执行。配置、架构和原 V4 包审计显式使用 **P07**，日志、实际依赖图及 PLAYER 无 Editor 编译输入证明均归档。

Q23、Q29 是 P07 协议和既有 GPU 路径范围通过；Q27、Q28、Q30 的本阶段行为通过。Q50 通过原断言验证共享投影和容量 2；额外五次真实进程运行记录进程耗时、Windows 峰值工作集和采样私有内存（见 evidence/projection-measurements.json）。计时包含 DLL 加载和进程启动，不等于预热后的内部循环耗时，采样可能漏过短暂峰值，也未测量分配次数。Flow 容量／字节／重试上限仍由行为断言验证，不宣称提速或全面分配优化。完整新产品双视口像素、布局与 GPU 退休资格仍按规范留在 **P10/P13**。受控 serial 不冒充实际 GPU 完成；真实后端状态测试也不冒充新产品像素输出。

## 保留的失败及边界

开发与早期资格中的失败原始日志在 `development/`；它们不替代最终 SHA 的 `logs/`。包括编译错误、缺少 owner completion dispatch 的新测试夹具、旧 Scene provider 名称／Flow 错误域回归、编译终态与窗口采用时序、析构前 Task 循环持有，以及 SDK 导出时新依赖声明过晚的问题。均修正后重跑；原断言未删改。首次归档门禁还纠正了报告误写的性能输出；随后执行独立进程测量并明确记录测量边界，没有把容量断言冒充峰值分配测量。第一份 debugger 输出因重复使用已修改 fixture 而触发测试断言，第二份 fresh-fixture 堆栈才定位 TaskRuntime 析构问题，不能混记。

独立验收从准确 tracked commit 检出并通过 ValidateTrackedSnapshot；使用已有独立构建树，**不声明全仓首次冷构建通过**。原 P05-R1 冷构建失败与 P06 Physics2D 窄修复记录原样保留。

C01 仍 FAIL（P09/P12）、C03 仍 FAIL（P11）、C04 仍 FAIL（P12），真实探针各退出 1。没有把本轮问题挂到这些旧编号。C05 的提交和后端隔离归本轮修正，后续新产品 GPU 资格仍按上述责任。

用户 `ProjectBuilder.cpp` 哈希保持 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`，未入提交；main 未修改。SDK 已重装；两个变更的 modules 公共操作头已同步 Debug、RelWithDebInfo、Android 三个 include 前缀并比对哈希。没有 Android 构建或新增人工桌面流程。

归档检查器只读归档路径和固定 Git blobs；模拟生产机器路径不可用时仍通过，缺失／损坏必需日志明确失败，结果见 `evidence/receipt-portability.json`。原 P00–P06-R1 记录与原规范保持冻结，各按自己的 implementation_sha 校验。
