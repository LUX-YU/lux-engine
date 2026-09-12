# ER-1 启动与关闭续行（2026-09-12）

承接 `430d7b7d59c06d1573bc063da4b77409b9e53f8c`，保留阴影依赖、数组范围、
EX-01—05、字体和输入锚点修正。本轮只继续 ER-1；独立 Scene 叠加不列为本 UI 波次阻塞。
主线未知修改受保护；没有修改并行脚本实现、依赖版本或恢复旧 Context。

## 生产修改

1. Vulkan Instance 创建失败后，禁止安装 debug callback 或进入物理设备枚举。
   返回原 Render `device::VulkanObjectCreationFailed`，未建立逻辑设备时析构不执行设备操作。
   已有 Instance/Device RAII 释放各自实际建立的资源；没有 waitIdle 掩盖失败或重试。
2. Renderer/View 提供固定容量的只读关闭状态；Session 提供按需 owning 快照；
   Application 只组合这些值。信息包括请求身份、原始子错误、资源句柄、CPU 版本引用、
   实际 GPU 完成水位、在途 release 和 TaskScope 完成状态。查询不推进任务、不释放 owner。
   Scene 快照的两处分配失败在查询边界转换为 ALLOCATION_FAILURE，原关闭继续可重试。
   无资源的 inspection Session 也返回实际生命周期状态，不能默认报告 CLOSED。
3. 示例在关闭等待 1 秒后、随后每 5 秒输出上述诊断。有限帧驱动的原 timeout 仍是失败，
   不计作正常关闭。没有增加超时强制销毁策略。
4. SceneView 和 Workspace 工厂在借用 Session/View 的无错误 getter 之前核对其 dispatcher。
   仅传入当前线程 dispatcher 不能授权读取另一线程的 Session。

## 线程与生命周期审计

| 路径 | 检查与边界 |
|---|---|
| Application start/run/shutdownStatus | Impl::check 先验 owner；Application 私有 Impl 内组合具体 owner，不共享完整 Impl |
| Renderer factory | 首先检查 UISession dispatcher，之后才读取 Window/config；运行方法先 Impl::check |
| Renderer state/statistics | 无错误查询要求 owner；worker 发布原有原子统计，外部线程不得借此读 owner 容器 |
| View closeStatus | ViewResources::check 先行；复制请求 ID 和状态；版本引用数与 last_submission 分开报告 |
| Session closeStatus | owner → busy → 快照准备；仅快照值可越过 Session 生命周期 |
| SceneView factory | 同时核对传入 dispatcher 和 Session dispatcher；随后 attachView/openView |
| Workspace factory | 同时核对 Window/Session/View dispatcher；成功前保留传入 unique_ptr |
| Pane 更新与 getter | 从 Window owner 的 draw/update 调用；Camera 值与 owning snapshot 不等同于 live Session |
| 资源完成回调 | 保有结果与原子完成信号；不借用 Session/Pane；TaskScope 完成前保留任务 owner |
| runtime lease | 原 owner 线程释放约束保持；shared_ptr 不证明协议或 GPU 已完成 |

## 修前、修后与证据口径

修前独立诊断构建绑定上述基线。真实 Instance 初始化负例在 Loader 物理设备枚举处
报告 `VUID-vkEnumeratePhysicalDevices-instance-parameter`，进程退出 -1073740791。
这是失败复现，不是负例 PASS；崩溃时没有可证明的终态资源计数。

修后 INITIALIZE（validation on/off）、线程启动、设备初始化后、窗口附着后共 5 个场景，
准确错误、Window/UI 保留、同 owner 重试、实际 GPU 进展和显式 close/join 均已验证。
Instance 原错误 type/args、worker/server 配对见原始日志。未独立声称自然 logical-device
创建失败已复现；对应析构分支完成源码审计。

关闭阻塞测试定位实际 held asset/request；释放前 Session 为 CLOSING，32 步仍保有 owner。
两个快照分配点逐一失败后仍可查询和关闭，快照可在 Session 销毁后读取。
跨线程、外来 Renderer、部分 Application 启动和原 G04 错误保持有实际定向测试。
完整阶段结果以交付包 `manifest.json` 与对应原始日志为准，不拿本文充当最终 SDK PASS。

两次驱动输入失误保留原日志：partial_late_close 误用完整 pak 导致超时；资源完成测试
参数未正确传入触发 argc 断言。均不属于产品复现或准确负例。GPU 驱动增加缺失 pak
前提检查，错误输入在建立 Window/执行 owner 前明确退出 2。诊断 CTest 首轮缺失 MSVC
开发环境，15 个编译负例未触及契约；加载开发环境重跑 156/156 通过。

## 未关闭门槛

真实 Windows IME 保持：修复接线已实施、辅助测试通过、真实桌面验收受环境阻塞、
q7 已知失败尚未获得关闭证据。持续 OS 鼠标捕获亦未获桌面结果；UiInputEvent 辅助测试
不能替代它们。未重试已证实不可用的 Computer Use 通道。

旧正式入口删除条件因此仍未全部满足。保留旧入口，新的 Application/Workspace 不调用它；
旧栈仅为既有基线消费者保留。没有进入 ER-2/3。正常/诊断产物分别绑定身份，只有正常
clean clone 可用于最终 SDK 和成本。静态同量成本不替代 resize/retry 尾部成本资格。
