# P06 验收说明

本次只交付 P06。实现与验收分别提交，阶段门禁固定为 `LUX_EDITOR_MIGRATION_STAGE=P06`。
实现 SHA、命令、退出码、归档相对路径和文件哈希见 receipt.json；不能用当前机器上的日志路径代替归档。

## 实现与唯一所有权

| 事实或责任 | 唯一 owner / 入口 |
|---|---|
| Registry、实例、时钟、SceneDriver、单步队列 | 原 SceneRuntime |
| 产品逐帧驱动 | EditorLoop 或 Launcher 的宿主循环；RunStore、SceneElement 不调用 driveFrame |
| 实例退休请求责任 | 一个 move-only SceneInstanceLease；预分配退休状态，不在析构中分配、等待或执行系统回调 |
| 实际回收 | Runtime 安全点，停止维护完成后移除记录，析构完成后设置 InstanceRetirement |
| 运行身份、来源、运行调试历史 | RunStore 唯一拥有私有 RunSession |
| 后台准备及完成 | 原 ExecutionRuntime；任务只保留冻结 SceneSnapshot/SceneCapture、环境及结果 |
| 作者数据、History、保存基线 | 原 SceneSession / SessionStore / SessionState；Run 不写回 |

Builder 返回 lease。ID 只寻址，退休凭据可在 slot 回收后继续查询。退休意图立即阻止新的 Registry 借用；
借用有效期止于下一次 driveFrame、owner wait 或结构变更。请求退休允许发生在驱动回调内，但不会清除外层保护。
Runtime 最终析构可以在宿主边界排空停止维护；此时 ExecutionRuntime、RenderResources 仍须存在。

暂停只停止仿真，维护、读取完成接收、Transform 同步和发布继续。恢复重设原 FixedStepClock 的墙钟基准。
每实例固定 32 个未确认单步记录，FIFO，每次获准有独立 simulation_completed 目标。
实际发布完成才是成功；执行错误保留原因，退休取消未完成步。确认终态释放容量，进行中确认仍拒绝。

StartRunId 表示准备请求，RunId 只在实际实例构造成功后发布。内层完成接收只存 owning 结果，不调用 UI、
不采用实例、不释放外层 dispatch 保护。准备被丢弃时取消，晚到结果释放自己的资源，不重跑构建。
StopTicket 在实际实例回收后完成；停止不更改作者源、observed、dirty、binding 或历史。

## 迁出、删除和暂留

删除 SceneRuntime::valid/invalid/getSceneRegistry/getClock/tick/TickResult/destroy 及原调用，
对应 pauseSimulation/resumeSimulation/borrowInstance/borrowClock/driveFrame/DriveResult/retireInstance。
未保留同义兼容入口。旧 scenes_guard_、destroyScene、run_source/run_assets/run_receipt/run_delta/run_stop、
run_prepared_/run_preparation/next_run/step_baseline_/single_step 和旧运行观察/驱动算法已迁出或删除。

SceneCodec 中冻结对象组装成分区、保留未知 payload 的唯一算法移至 SceneSnapshot.cpp；保存和运行共用，
运行不再 encode pak 后立即 decode。SceneEditing 解除 ProjectStorage 依赖，资产验证由宿主显式提供谓词。

`editor/transition/SceneRunCaptureAccess.hpp` 是本阶段唯一新增私有桥，仅 editor_scene 消费、SDK 不安装、P12 删除。
它转换旧 Registry 的一次冻结捕获；没有同步的影子 SceneSession。新 execution 不 include 或链接此桥。
旧 SceneEditor 仅保留窗口接线、选择和状态投影、暂停编辑借用与 StartRunOperation；P12 删除旧壳。
旧按钮保持原“有一个未完成步时拒绝再次点击”的行为和断言；独立 Run API 使用完整 32 项 FIFO，不能混淆两层准入。

新增 scene_execution_api 头边界和 scene_execution 静态库，未增加 DLL。为真实依赖闭包检查，收窄 task/pinclude
和 ScriptRuntimeAccess 的传递可见性，仅实际脚本实现私有使用；未重写脚本或执行器算法。

## 验证范围

| 验收 | 真实行为与证据 |
|---|---|
| X06-01 | 两个实际 Run、两个读取消费者；维护计数只增加一次。旧多场景/双视口 GPU 回归继续运行 |
| X06-02 | 暂停后读取完成被采用、仿真 step 为零；暂停字段修改和 Undo/Redo 触发 Transform；恢复 dt=16ms |
| X06-03 | 32 FIFO、容量拒绝、逐次完成、确认、实际发布失败、RunId slot 复用及失败构建不再采用 |
| X06-04 | 维护回调内退休、有在途待办、外层仍 BUSY、最终仅析构一次；嵌套准备完成可靠吸收 |
| X06-05 | 冻结 S10、作者改为 S12、运行改为 S20；Stop 后完整源、current/observed/dirty/binding 不变 |
| X06-06 | PLAYER 单独配置、all/no-work/CTest、实际目标及编译单元无 Editor；实际 headless/driver/runtime/world_loading |

Q22/Q24/Q47 本阶段完整覆盖。Q23/Q29 保留暂停资源、背压和 GPU 退休回归；完整新投影/双窗口产品仍归 P07/P10。
Q25 沿 P00 盘点未新增 ApplyRunChanges，实际验证停止不回写，界面不展示已实现假入口。
Q43 覆盖本次运行、任务、实例及资源退休；完整应用退出/插件卸载资格仍归 P12，C03 不改判。

最终 Editor CTest 121/121、PLAYER CTest 11/11 通过；两套 all -j 4 -- -k 0 后的第二轮均无新增工作。
原 116 项测试保留；新增四项实际 Run 测试和一个真实 CMake 依赖负例组。
`evidence/tests-preserved.json` 记录原测试源 SHA，`assertion-review.json` 和 `test-migrations.patch` 逐项解释 API 迁移。
原 History、SessionState、三模型核心及持久化/R1/R2 未重写。依赖负例必须失败于预期规则、同夹具修复后通过。
P06 归档门禁在生产日志路径不可用时通过；缺失或损坏完成日志均被准确拒绝，见 receipt-portability.json。
SDK 重装后运行原九组消费者和新 Run 消费者；新消费者直接复用实际模型测试，不通过隔离 shim。

开发失败保留在 logs/development：包括中间编译错误、旧编辑目标在退休请求后仍可借用、依赖私有头泄漏，以及
新增测试最初把 TaskInfo 诊断历史误认为活动任务。外部 GPU 插件消费者还暴露了手动渲染泵在 Runtime 析构前停止的问题；修正为沿原泵等待退休回执后析构，保留原像素和验证层断言。
对应修复是立即撤销 Registry 借用、收窄 include 可见性、
测试关闭任务历史以检查真实活动记录；未删除原断言。

## 边界和已知失败

用户 ProjectBuilder.cpp 修改始终未入提交，SHA256 为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
独立检出绑定实现 SHA；原历史快照按各自实现 SHA 核验，不改写旧收据。

C01 保持 FAIL，责任 P09/P12；C03 保持 FAIL，责任 P11；C04 保持 FAIL，责任 P12。
历史 Physics2DDescription 冷构建顺序失败保留原证据。P06 的首次 PLAYER 冷构建实际复现该问题：
physics2d_description 依赖了空的 codegen 标签，缺少对实际生成目标的依赖。按启动补充授权，
仅将依赖改为 physics2d_component_codegen_generate / physics2d_configuration_codegen_generate。
最终 all 构建前移走该生成头、元信息和描述程序对象文件，验证缺失输入会先生成、再编译；记录在
physics-generation-inputs.json 和 player-build.log。历史失败和本次首次失败都不删除，不挂入 C01/C03/C04。
Editor 使用原独立构建树增量资格，未宣称全新首次冷 Editor 构建通过；窄依赖修复不代表全仓冷构建审计完成。
PLAYER 初次目标清单检查误把 CMake 头文件变更检测的 phony 输入当成了 Editor target，失败保留。
最终核验 CMake 实际 TargetDirectories、全部编译单元及头依赖参数、无 Editor 的 CTest 清单，
并保留完整 Ninja 清单与 headless 二进制依赖记录；未放宽实际 Engine→Editor 依赖规则。
没有 Android 构建，没有人工全新产品体验验收，没有自动进入 P07。

本轮未修改 modules 公共头；SDK 已按接口变化重装。三个 modules 安装前缀同步条款本轮未触发。

运行热路径保持原 Runtime 顺序遍历；单步记录是每实例预留的 32 项数组，不新增通用任务队列。
RunStore.update 读取事实，不重复推动场景。准备、快照捕获和暂停历史创建是冷路径；旧 Runtime 的
10000 次暂停维护计时仍保留在 runtime-detail.log，不把该数字当作完整产品性能验收。
