# Framework v2 P0–P2 收敛记录

本轮按 `lux-engine-framework-v2-next-implementation.md` 完成 P0–P2，主题初始化一并收敛。
不实施 SceneSession vertical slice，不扩大 SceneToolRegistrar 的 provisional 合同。

## 实现与责任

| 项目 | 版本／位置 |
|---|---|
| 基线 | `31c0a3495fd51f957d9bd880d62c12ed2c753eec` |
| 完整实现及本轮资格 | `6e2ecb2fd52b66a717749072cad46f6dfde86b3f` |
| lux-cxx | `0a0e7419fc7229df6e372cd35a540249f92250ef`，本轮未修改 |
| 工作区／分支 | `E:/SyncForder/CodeRepos/lux-engine`；`codex/editor-framework-v2` |
| 全新 SDK | `E:/SyncForder/CodeRepos/install/Framework-v2-pacing` |
| 纯依赖前缀 | `E:/SyncForder/CodeRepos/install/Framework-v2-dependencies` |

- 删除 `frame_interval_ms`、`next_frame_` 和其等待分支。有可写帧槽立即构帧，否则复用原生、
  Process、Object、渲染完成及 Scene timer 唤醒。Root 的后批结构／菜单工作复用原生唤醒。
  UI 回调提出关闭后，等待前再检查关闭意图。输出创建失败返回原错误，不当作尚未就绪无限等待。
- `enable_vsync` 通过 EngineRendering／RendererConfig 转交已有后端 present policy；没有新增软件
  帧率限制器。SceneRuntime 每轮仍只推进一次，输入只采样一次，已捕获帧沿原资源固定与退休链处理。
- Root 删除 TextEdit／NumericEdit 友元；控件使用 `Element::menuActive()`。Root 不识别 Undo／Redo
  名称或键位，产品 main 提供菜单配置。原命令查询、延迟执行和目标重验保留。
  删除没有活动消费者的 `EMenuAction`／`MenuRequest` 及派发原体，没有新增全局命令目录。
- `UpdateStatistics`／`FrameStatistics` 只保存数值快照；Root 统计当前更新，宿主累计阶段耗时与次数。
  Scene 数量从原记录容器 O(1) 读取。没有采样库、观察者、历史记录器或新增维护列表。
- Theme 在 Context 创建时应用一次；原配置缩放跨帧保持。未增加动态换肤接口。

ObjectRuntime、Root 的 Pane 容器、Registrar、ExecutionRuntime、SceneRuntime、UI 帧槽及渲染资源
继续分别承担原有唯一责任。Event 沿层级传递本地交互；Signal 从业务状态 owner 传播事实。
PaneHandle 不作为兄弟 Pane 之间的业务依赖注入。相关合同写入框架及 UI README。

## 修前证据与有限观察

先使用真实旧安装 SDK 构建最小化窗口夹具。初次宽松 `<100` 次边界通过，实测 77 次／约一秒；
收紧到用于检验安静等待的 `<20` 次后，保留 `po-before-idle-bound` 的真实非零退出：
**75 次／1.007625 秒**。没有把规范推算的约 1 kHz 当作本机实测，也没有覆盖第一次结果。
旧结果证明原循环仍定时醒来，不作为严格同条件的 CPU 提速百分比基线。

最终实现、独立 RelWithDebInfo 检出上的 `po-clean-measurements`（MSVC 19.44、i7-13700KF、
RTX 4070 Ti；设备清单另存 machine.json）：

| 场景 | 观察时长 ms | 轮次 | 捕获帧 | 等待 ms | 活动阶段 ms |
|---|---:|---:|---:|---:|---:|
| 最小化，Process 完成再投递 Object 消息 | 700.243 | 7 | 0 | 699.961 | 0.253 |
| 最小化，原生关闭消息 | 700.449 | 7 | 0 | 700.143 | 0.241 |
| 可见，VSync ON | 700.341 | 276 | 114 | 690.855 | 9.279 |
| 可见，VSync OFF | 700.949 | 14768 | 5197 | 449.217 | 246.642 |

两种可见模式分别经过 91／4561 次真实背压等待。高吞吐模式的完成唤醒仍会推进循环；
结果不表示应限制轮次，也不承诺特定显示帧率。活动阶段是 steady_clock 墙钟耗时，
不是进程 CPU 利用率；没有据此宣称零分配或全局 CPU 降幅。

UI 固定树测试每种规模预热 5 次、采样 30 次。以下单位为微秒，顺序为 p50／p95／max：

| Pane × 每 Pane Label 数 | 维护 | 布局与绘制 | 捕获 |
|---|---|---|---|
| 1 × 100 | 1.6／1.8／1.8 | 38.1／39.2／39.9 | 0.9／1.0／1.0 |
| 1 × 1000 | 16.1／23.1／25.5 | 316.7／395.5／401.8 | 1.1／2.7／3.6 |
| 3 × 100 | 4.6／4.8／4.9 | 110.4／120.7／120.8 | 2.8／3.0／3.2 |
| 3 × 1000 | 45.0／50.1／51.2 | 923.7／950.8／964.2 | 3.0／3.7／5.6 |

每个 Pane 另含一个 Layout，计数断言包含它。捕获计时覆盖 DrawData 复制与同步固定回调；
该纯 UI 规模夹具没有额外 GPU 固定成本，实际宿主路径另由 pacing／transport 测试覆盖。
维护包含隐藏对象和后批工作；无绘制更新清空上一帧绘制统计。没有引入新场景调度、
全局 Element 登记、零复制 DrawData 或 Registrar 散列表。

## 验证矩阵

所有资格绑定上面的完整实现 SHA；Editor／PLAYER 独立配置，构建与 GPU 串行。
全量命令使用 `all -j 4 -- -k 0`。

| 验证 | 结果／实际记录 |
|---|---|
| Editor clean tracked | 冷构建 1064 个动作，二次无工作；45/45；`po-tracked`、`po-clean-*` |
| PLAYER | 独立冷构建 1014 个动作，二次无工作；34/34；`po-player-*` |
| 安装 SDK | 新前缀全量构建、二次无工作；10/10；`po-sdk-*` |
| 六组独立安装消费者 | object-core、object-ownership、ui-composition、services-core、services-tasks、spatial，9/9 |
| 外部插件 | 安装示例真实 DLL／GPU，1/1；`po-plugin-gpu` |
| 公共头 | 50 个安装头逐个 C++20／无 RTTI 编译；`po-public-headers` |
| 回调负例 | 源码与 SDK 同头正例，以及六类可抛回调编译拒绝，保留原约束 |
| UI 行为 | 外部控件使用受保护通用能力；无配置 Ctrl+Z 不生成隐含命令；配置快捷键下一安全点只执行一次 |
| 边界回归 | 缩放样式跨帧、无帧维护、无效输出准确失败、维护回调关闭不进入无限等待 |
| 闭包 | Editor 555、PLAYER 526、SDK 41 个编译单元及外部消费者，均无 legacy／旧 SDK 或源码私有头补齐；`po-closure` |
| 证据 | 471 份元数据冻结，60 份当前原始输出核验；中文空格路径搬迁通过，缺失／篡改真实测试输出被拒绝；`po-freeze`、`po-evidence-verification` |

开发期间缺失测试头、信号返回值检查写法及焦点夹具的首次失败保留在 `po-*` 输出中。
快捷键夹具修正为菜单首次构帧后经公开 requestFocus 恢复目标并先断言焦点；没有放宽命令
执行一次、维护次数、失效目标、资源固定或退出断言。正式冷构建和最终测试不依赖这些失败构建。

## 范围与交付

原生键鼠接管仍为 **NOT_RUN_USER_DEFERRED**。本轮原生关闭消息、自动 resize／最小化／在途关闭
及 GPU 运行不替代人工输入资格。Linux、系统 IME、sanitizer、历史性能延期和既有免验范围不变。
Android 只同步公共头，不作配置／构建。SceneToolRegistrar、持久名称绑定和全局产品命令
归未来真实业务切片；本轮不自动实施。

原始输出在 `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2`：

- `runs/commands.json`、`runs/logs/po-*.log`：命令、实现 SHA、工作树状态、退出码及输出哈希。
- `pacing-ocp/before/`：旧 SDK 实际负例源码。
- `pacing-ocp/verified-evidence/`：相对路径取证及必要输出。
- `pacing-ocp/qualification-6e2ecb2f/`：冻结构建、安装、File API 与测试元数据。
- `pacing-ocp/files-6e2ecb2f.txt`：全部 37 个改动文件；新增仅两份统计值头及两份测试源。
- `pacing-ocp/protection.json`、`final-header-sync.json`：用户差异和三前缀公共头核验。

EditorContext.hpp 的原成员对齐和 Pane.hpp 原注释缩进均保持原字节、未提交。
ProjectBuilder 用户补丁仍独立保存、未应用，原用户字节 SHA256 为
`ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。
五个改动过的 modules 公共头在三个规定前缀中的 15 条哈希逐项一致；全新 SDK 单独由干净
检出安装。两份历史 ZIP 哈希不变。临时 qualification 检出及旧 SDK 探针构建在冻结核验后清理，
SDK 和外部证据保留。

**本轮 P0–P2 代码与上述 Windows 适用验证完成**，停在独立复审，不自动进入 SceneSession。
历史快照不改写。实现与本记录分别提交，正常推送原实施分支；不修改 main、不删除历史分支或发布。
