# Lifecycle LR06：语义身份撤销与私有物理退休

最终生产实现 `94a7ddcb1469ea7f5296f595f144e35534c3352a`，lux-cxx
`0a0e7419fc7229df6e372cd35a540249f92250ef`。原 LR00 清单的 27 条责任、四组提供者已逐项核对。
LR06 实施及本阶段 Windows/安装/GPU 验证完成；不代表整个 LR/MA 计划完成。

## 所有权与删除

| 提供者 | 唯一责任与退休边界 | 删除及明确保留 |
|---|---|---|
| PresentContext | 唯一拥有完整、固定地址的 PresentBacking；析构将其转入预备好的原 PendingResourceRelease 记录 | 删除公开 close/acknowledgeDeviceLoss、closed 标记及废弃 surface 释放 helper；保留原 GPU 等待、失败重试、目标收据与背压 |
| SceneRuntime | 活跃表唯一拥有 SceneInstance；安全点撤销代际身份，再把同一 owner 转入私有退休表 | 退休期间不再提供原实例业务访问；原有界 InstanceLifetime 保留真实 COMPLETED/FAILED/CANCELLED 原因与代码 pin，直到单项或整体确认 |
| RenderRuntime | RenderContext 拥有应用级 Runtime；原请求/回复队列、回调槽结清已接受工作 | 删除公开 beginClose/advanceClose/joinStopped、ERenderClose 及 joined 哨兵；保留真实 ACTIVE/STOPPING/RETIRED 状态和最终物理同步屏障 |
| GpuTransferPipeline | LR03 已建立完整 State 和叶子 owner；原 worker/结果队列负责停止与排空 | 删除忽略 waitIdle 错误的路径；仅 SUCCESS/DEVICE_LOST 允许释放 native backing，其余失败在原安全边界终止；真实线程/槽位/时间线同步状态保留 |

四个实现提交依次为 `9ad9aaf59`、`c9b3e2c1`、`c1addc78`、`94a7ddcb`，合计 24 个文件。
没有第二个退休管理器或结果表。Scene 收据不为保留轻量结果而保活整图/GPU；原生窗口仍必须覆盖
surface 的真实退休。普通 Scene/Project/Pane 析构不能借用应用级 Runtime 屏障等待整个后端。

RenderRuntime 正常关闭先结清接受的 Control/Program/Upload 再停止后端。后端提前终止时，join 后
再次消费原回复队列，随后按剩余预算结清无回复请求，保存原终止错误；没有后端错误时使用
ChannelStopping。每个终态回调占用一次预算，递归收取仍返回 BUSY。工厂失败不发布半成品 Runtime。

## 行为证据

- 真实修前输出保留：Present owner 直接释放失败；Scene 在维护尚未结清时仍暴露退休实例；
  Runtime 析构丢失已接受请求；Transfer State 忽略注入的 idle 失败。
- Present：阻塞 GPU 时 owner 消失、构造前缀失败、native 释放顺序、等待失败重试和设备丢失原因。
- Scene：回调期间请求退休、安全点后旧身份拒绝、新代复用、退休表增长与乱序回收、三种单步终态晚读。
- Runtime：正常关闭、真实后端提前停止、八个请求按预算 1 精确结清、递归 BUSY；本地排队和已转交上传
  在公开 owner 消失后仍结清，旧客户端拒绝新业务；启动失败后可重新创建。
- Transfer：实际 Pipeline/DeviceContext 成功、设备丢失原因及其它失败三条最终释放路径。
  注入先等待真实 GPU 安全，再返回指定错误，**不是物理 GPU 设备丢失实测**。

原纹理、native 句柄、Scene 资源、FIFO、容量及背压断言保留。所有夹具首次编译/运行失败保留；
VMA 实现缺失、原混合换行源码哈希等问题按真实原因修正，没有降低生产断言。
原收据源码按各自实现 SHA 核验；混合换行恢复同时验证原字节哈希与规范化后的 Git 对象内容。

## 最终固定提交资格

ValidateTrackedSnapshot 后，从独立 clean tracked 检出配置，全量构建固定使用 `-j 4 -- -k 0`。
全部构建与实机/GPU 验证串行，用户六处差异没有进入资格源码。

| 验证 | 实际结果 |
|---|---|
| Editor 冷构建/第二轮/完整 CTest | 1234 步 PASS / no work / 113/113 |
| PLAYER 冷构建/第二轮/完整 CTest | 1137 步 PASS / no work / 54/54 |
| 原安装 SDK 消费者 | 192 步 all、no work、16/16 |
| 本阶段真实安装消费者 | 29 步 all、no work、Scene/Runtime 2/2 |
| 公共头 | 七个独立 C++20/noRTTI 编译；全新 SDK 与 clean source 字节一致 |
| 实际 source/include/link/install 闭包 | Editor 653、PLAYER 597、原 SDK 59、本阶段 SDK 9 个 TU；无 legacy、开发 build DLL 或源码私有头回退 |
| 桌面/GPU | Editor 完整 CTest 中 36 个 gpu、8 个 desktop 标签项通过；不等于系统原生输入/IME 资格 |

六个修改的 modules 公共头与 Debug、RelWithDebInfo、Android 三个规定 include 前缀一致；Android
仅同步头，未执行构建。旧公开关闭接口编译负例按原 Runtime 收据继承，未冒充本次重新执行。
Present/Scene/Runtime/Transfer 的 scoped MSVC ASan 同样按四份原收据继承；依赖 DLL 没有全部插桩，
不宣称本次全后端 sanitizer 资格。

## 归档及剩余范围

外部归档：
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/lifecycle-lr06/verified-evidence/`。
1405 个文件、113 条命令、19 条最终必需命令；Manifest SHA256：
`1e93df8e7d05023697266df7d596ea69e443ac25a1c25d17b9b2001c3e93739a`。
原始失败、消费者输入、逐符号范围、构建/链接/安装材料和原收据均包含在相对归档中。
搬迁至中文/空格路径验证通过；缺失及篡改真实 SDK 输出均被拒绝，没有生产机绝对路径回退。
最终核验输出位于同级运行记录 `runs/logs/lr06-qualified-verify.log`。

`Q-LR03-HOST-MINIMIZE` 保持 OPEN，本次通过不证明其根因已修；已知蒙皮跨帧 WAR 责任不变。
原生输入继续 `NOT_RUN_USER_DEFERRED`，IME 未测；Linux 和支持的平台 sanitizer 仍为 LR08 必测。
不新增免验，不补旧 50k 长测。

工作区 `E:/SyncForder/CodeRepos/lux-engine`，分支 `codex/editor-framework-v2`。六处用户差异哈希未变，
ProjectBuilder 补丁未应用，main 与历史记录未改。之后按规范推进 LR07 的类型状态收拢，继而 LR08
及 MA00–MA11；用户已授权阶段间持续推进，不等待单阶段复审。
