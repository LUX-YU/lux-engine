# P06 R1 验收说明

仅补正 B01：实例回收后，未确认 Run 的单步结果不能消失。停在 P06，不进入 P07。
前置验收 cc3f455095f52e9a2740bf0a31a55e6672240916；实现 SHA、准确命令和相对归档路径见 receipt.json。

## 真实修复前复现

先在原实现 26cca1c47adabdbf52346e149b44f7264479bf40 上叠加本次实际测试（生产代码与前置验收相同），
执行 all 构建，再运行真实 RunStore/SceneRuntime。before/baseline.json 固定唯一测试覆盖文件及哈希。
三场景均退出 1：StopTicket.complete=1，RunInfo 为 STOPPED/FAILED，实例槽位已经消失，
首次读取原票据全部得到 INVALID_ID。复审包草图只作为输入材料保存，不计运行证据。

## 唯一结果 owner 与释放

原 InstanceLifetime 现在拥有唯一的 32 项 Step 数组和 next_step。Record 内原 Step/steps/next_step
已删除，所有发号、FIFO 执行、完成／失败／取消结算仍由原 SceneRuntime 完成。
RunStore 的 ticket 列表只标识它提交的操作；没有第二份结果、时钟、队列或 History。

重 Registry/Simulation/系统/任务/GPU 资源仍在原退休安全点析构，退休回执随后完成。
Run 记录通过其 InstanceRetirement 访问同一轻量结果表，不需要及时在 erase 之前轮询。
Runtime 查询/确认增加可选的匹配退休凭据；未提供时仍只查询活动槽位。
所有路径仍检查 owner 线程、Runtime 域、实例代际、完整票据和外层驱动保护。

acknowledgeStep 清除单个终态槽；进行中仍 BUSY，已确认票据明确失效。
acknowledgeStop 是明确的聚合确认：先清除该 Run 剩余结果，再删除 Run 槽位。
外部保存 StopTicket 副本也不能使已确认结果复活。未确认的 STOPPED Run 继续占用既有 Store 容量。
结果不引用 Registry、SceneInstance 或 Renderer，不通过保活重对象保留结果。

错误的 std::any 可能含插件代码。创建阶段收集组件／实际安装系统的代码 pin；失败结果持有窄代码 owner，
值析构先于代码释放。SceneStepStatus 的值副本也持 pin，替换赋值先完成旧 payload 析构再释放旧 owner。
退休时释放创建阶段的 pin，仅仍存在的失败值保留必要代码。原单帧 DriveResult 的借用失败数组也保持代码
到下一帧清空该数组为止；这只是原借用结果的代码寿命，不是另一套结果调度或墓碑。
错误副本读取与确认的 payload 清理处保留 Runtime/RunStore 外层保护，不因嵌套复制或清理开放重入。
真实 Run 自定义错误复制回调会尝试递归 acknowledgeStep/acknowledgeStop，两者均被现有 BUSY 保护拒绝。

## 验证矩阵

| 场景 | 实际检查 |
|---|---|
| R06-R1-01 / r1-queued | 两步获准后直接停止；回收后首次读 CANCELLED/STOPPED；无仿真补跑；逐项确认 |
| R06-R1-02 / r1-mixed | 先完成一张但不查询，再排两张并停止；晚读 COMPLETED/CANCELLED/CANCELLED；作者源和 current/observed/dirty/binding 不变 |
| R06-R1-03 / r1-failed | 真实 publication failure 731；实例回收后首次读 FAILED，PUBLICATION 阶段及原 payload 保留 |
| R06-R1-04 / r1-callback | 维护回调 stop；外部可控 pending 端点；嵌套 drive 仍 BUSY；在途结清后仅析构一次，结果可晚读 |
| R06-R1-05 / r1-capacity | 64 轮 Run/实例槽位复用；32 票据容量；未确认 Run 占容量；单项/聚合确认、重复确认、旧代际拒绝；销毁次数与恢复分别检查 |

原 Runtime 测试还覆盖退休凭据的线程／域约束，以及复制含自定义析构 payload 的失败结果后，实例已销毁、
槽已确认、原借用 DriveResult 已结束，代码仍存活到最后结果值清理。没有使用失效 Registry 指针控制待办。

原 121 项 Editor 测试和断言保留，增加上述五项；PLAYER 原 11 项保留。
原 P01/P02/P03/P04/P05 与 R1/R2、真实文件 IO、依赖负例及十组 SDK 消费者重新执行。
SDK Run 消费者直接编译真实测试，包含五项晚读／确认路径；不借私有头，不用隔离 shim。
最终 Editor CTest **126/126**、PLAYER CTest **11/11**、十组安装消费者 **42/42** 通过。
两套 all -j 4 -- -k 0 后第二轮均无新增工作。原受修改文件中的 146 条断言逐条保留。
门禁显式 LUX_EDITOR_MIGRATION_STAGE=P06；准确命令与每个退出码见 receipt.json。

新增容量夹具最初误以为 Store 超容量会到 adopt 才被拒绝，真实准入在 prepare 已拒绝。
开发失败输出保留，修正夹具在既有准入边界检查 CAPACITY；生产准入算法未修改。

## 范围和继承事实

只改原 Runtime、退休结果契约、Run 查询确认及相关测试／SDK CMake。未改目录、target、包名、执行器算法、
三作者模型、History、SessionState 或保存链。原私有冻结输入桥不扩权，仍仅 editor_scene、SDK 不安装、P12 删除。
原 P06 和之前快照逐字保持，按各自实现 SHA 核验。Physics2D 冷构建依赖修复及原失败证据仍在 P06/P05-R1；
本轮沿用独立构建树，不宣称新增全仓冷构建资格，也未重新修改 Physics2D。

C01 仍 FAIL（P09/P12）、C03 仍 FAIL（P11）、C04 仍 FAIL（P12），实际探针再次退出 1。
用户 ProjectBuilder.cpp SHA256 保持 ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c，未入提交。
未修改 main。modules 公共头未改，三个 modules 安装 include 同步条件未触发；引擎 SDK 已重装。
没有 Android 或新增人工桌面流程验证；本次不改变显示行为，原 GPU/旧产品及安装消费者回归保留。

归档检查器只读取仓库归档及固定 Git 实现，不依赖生产机器绝对日志路径。
实际模拟生产路径不可用时通过；缺失或损坏 run-r1-failed.log 均被拒绝，见 evidence/receipt-portability.json。
