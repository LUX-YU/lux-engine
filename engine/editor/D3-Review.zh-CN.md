# D3 v4 联合波次：实现与交审记录

日期：2026-09-16。状态：**联合波次候选已实施，提交独立审阅；不宣称 D2-C／D3 全部产品验收完成。**

代码候选：`28523b4e68a4ae1c6d33659e30f96b2b752db998`，开发分支 `codex/editor-d3-foundation`。
生产实现提交为 `ecaf2082048d3290fe2b4e86950a94f48f03d9ed`；其后的三个提交只增加／修正测试。
基线报告 `51e47edd2f038ef397e2328398e5a2da368f95c8`，旧生产候选 `88bbaab013d8a64058eee0497c01968211d864e6`。

实际工作区为 `E:/lux-ed-d2/src`。开始时工作树干净，无适用的额外 AGENTS；没有覆盖未知修改。
本轮未改动 main 或并行实验分支；main 保持 `4aee84a48e235174534ca4a0fe0cc6afe39cfcd7`，D2-C 分支保持报告 HEAD。未推送、合并、发布或冻结。

## 实现边界

| 接点 | 当前唯一职责与源码 |
| --- | --- |
| Mesh／Light 关联 | `InstanceResources`／`LightResources` 分别拥有完整 64 位源键→句柄以及句柄代次→源键的关联。删除只影响本 owner，淡出先解除关联，旧实例回收不能删除新关联。匿名分配仍不需要源键。 |
| RenderScene | 移除通用 EnTT Registry、旧 binding 组件及 Vulkan target 的 EnTT 依赖；保留 `ResourceRegistry`、Scene/View、GPU 资源与既有退役设施。 |
| 值解码 | ECS schema 的 `DecodedComponent` 拥有具体组件值、安装／析构函数以及 provider code lease；`decode_value` 不需要临时 Registry。安装之后销毁暂存值，最后释放 code lease。 |
| 身份规划 | `EntityCreationPlan` 将 EnTT free-list／代次相关算法收回 ECS core。准备时规划和验证；提交按已验证计划建立所有目标实体，再安装组件。 |
| Scene 作者内容 | 私有 `SceneObjects` 拥有目录、持久身份、选择、组件版本；`SceneObjectEdits` 拥有结构修改的准备和提交。Registry 仍是已物化作者组件的权威存储。 |
| 渲染绑定 | Main 的 `SceneRenderBinding` 取得 runtime lease、创建后端 Scene、附加 Feature、推进有限消费及关闭。`SceneRenderInput` 只将配置、元数据寿命、Feature 句柄和运输存储交给真正的 Scene owner。 |
| 渲染生产／消费 | `RenderSyncPipeline` 及 stages 跟随 Scene owner，`RenderSyncConsumer` 留在 Main；共享部分仅有有界消息槽及原子事实。worker 不持有 Main 控制／程序客户端。原 `EditorRenderer` 线程检查保留。 |
| 独立运行 | `SceneRun` 从当前作者捕获，经既有 codec 创建独立 NativeScene；Process CPU worker 内构造、物化、封印、执行和销毁 Scene。复用 TaskGraph，当前 executor 在该 worker 内执行，不建立新调度器。 |
| 运行资源 | Main 的 `SceneResources` 冻结并保留已采用的资源绑定。worker 获得拥有型值；不借用作者 Registry／资源请求。Run 修改超出冻结绑定的 Mesh／Material 引用会准确失败，不隐式流送。 |
| 桌面 | `RunPane` 只持有本次 Run 的 View、图像和局部显示状态，提供 Pause／Resume／Step／Stop。关闭 Pane 不停止运行；整窗口关闭按文档策略停止运行。Run 没有作者历史。 |

一次只接纳一个活动 Run。Play 前验证空闲、固定 delta 和至少两个 Process CPU worker；不以暂停释放 worker 配额。主路径没有新全局 Manager、Context、Adapter、EventBus 或事务框架。

`SceneEditor.cpp` 从基线 2440 行变为 2153 行。结构编辑已提取，保存和模型放置等既有职责没有为了行数继续拆分。

## 运行、失败与关闭协议

1. Main 捕获固定作者内容及 StateId，并锁定已准备好的必要渲染资源。
2. 既有 Process sender 执行 codec 工作；Main 异步完成后端 Scene／Feature 绑定。
3. worker 获得 `SceneRenderInput`，在自己的最终存储中建立 Registry／Simulation／Scene／stages。
4. 每次只执行一个固定 delta。稳定点的必要发布遇到背压后等待容量；只重试发布，不重复执行该模拟步，也不继续无限推进后续步。
5. Main 消费有实际 packet budget；Renderer 使用最后采用的状态。Stop 同时唤醒控制等待和发布容量等待。
6. worker 结束并销毁 Scene；Run View 等待 CPU 引用及实际 GPU 完成；有序 Program 附件确认前缀退役；最后关闭后端 Scene、解除资源保留并释放活动名额。

Pause 有独立的请求中事实，worker 到达稳定点后才报告 PAUSED。Step 只接纳一个待执行／执行中的步；重复 Step 返回 BUSY。再次启动发行新的 RunId，旧 RunId 不能停止新 Run。

准备失败仍由准备 owner 持有资源；文档准备完成、尚未交给 Editor 时取消，也必须继续推进关闭后才返回终态。关闭未完成不会丢弃 owner。

正常业务失败在准备边界返回。安装支持条件由组件能力说明，不能从“可移动”推导无业务失败；标准容器的分配本身不被本轮禁止，也没有新 OOM 转换或故障分配框架。

结构提交不等于自动原子事务。它先完成可拒绝准备，并在提交期间封住作者公开业务读写入口；正式业务通知在历史提交之后发布。没有声称任意 EnTT 回调、闭包列表或 EcsCommandBuffer 自带全局事务保证。

## 已发布、已采用和 GPU 完成

这三个事实没有合并：

- `published`：进入 Scene→Main ring。
- `forwarded`：Main Program client 接纳；不是后端采用或 GPU 完成。
- 真实后端采用：测试等待有序 Program 前缀，然后读取实际 Mesh／Light control stats。
- GPU 完成：使用现有 Renderer 完成水位以及 ViewImage 的 `GPU_COMPLETE` 证据；也不等价于物理屏幕呈现。

当前统计中的 `pending`／`high_water` 只描述容量为 1 的 ring 队列，不包含 consumer 当前持有的包或 Program client/server 队列。Main 持有的 `forward_pending` 被纳入实际关闭判定；不能用 ring 计数为零替代全部排空。

## 修前与修后

审阅方提供的是源码风险，本报告中的运行结果由本轮实际产生。

旧候选 R01—R03 实际通过：同源 Mesh／Light 独立删除、幂等更新和淡出后重新关联不能被描述成旧版已复现缺陷。

R05 的完整代次源键场景则实际发现旧版问题：原场景一个点光源；对同一索引的不同代次源键执行创建、旧源删除及新源幂等更新后，旧版出现 **4 个点光源，预期 2 个**。新候选得到 **2 个**，清理后回到原计数。
旧实现将外来源键作为 EnTT 实体 hint；已有索引与请求代次不能同时满足时，实际创建的实体身份不再是请求键，后续查找失去该关联。新 owner map 保存完整外部键。

`R05-before-observed-mismatch.log` 保留首次准确计数及断言失败；`R05-before-exact-negative.log` 的专门驱动检查这个具体不一致后正常关闭。后者的 `PASS case` 表示负例驱动完成，不表示旧产品通过 R05。不是把任意崩溃、超时或工具错误当成复现。

## 逻辑验收映射

逐项记录见审阅包的 `acceptance-results.csv`；下表说明实际覆盖边界。

| 条目 | 实际证据与范围 |
| --- | --- |
| R01—R03 | 真实 Render handlers/control stats；同源 Mesh／Light 删除、重复 upsert、旧实例淡出与新关联并存及回收。 |
| R04—R05 | 作者与 Run 的不同后端 Scene：作者删除后 Mesh=2，Run 保持 Mesh=3；完整源键代次、旧删除和旧句柄拒绝。 |
| R06—R07 | 独立 Vulkan owner 测试覆盖 Mesh／Light 无源键分配和退出、shutdown 后关联与存活计数。该测试有真实 GPU 分配但没有绘制提交；实际 View 退役由桌面 Run 测试另证。 |
| C01—C06 | 真实生成组件、坏的后续值、A/B 双向引用、已有／空／未解析引用、嵌套容器、真实删除及八次 Undo/Redo 代次恢复。失败保留作者内容、选择和历史，同一失败解除后可重试。 |
| C07—C08 | 丢弃值和 stopped TaskScope 准备均精确析构一次，并在值之后释放 provider lease；独立生成 Domain DLL、GUI DLL 和安装消费者实际编译运行。 |
| T01—T07 | Main-only 负例仍准确拒绝；实际 worker 独占 Registry／stages；受控 CPU 工作期间 Main 更新相机且 GPU 推进；零／一／正常预算、真实背压、Stop 唤醒及最终 Program 排空。没有将受控工作称作真实 SLAM 负载或宣称 TSAN 覆盖。 |
| D01—D03 | 非空真实 Scene、GPU 完成的运行预览、Pause、单次固定 Step、重复 Step BUSY、Resume 不累计暂停时间。 |
| D04—D05 | 有限但越出 render page 范围的作者值在 Run 首次稳定发布处准确失败：`run.stable`、`SceneExecutionFailure::SYSTEM_FAILURE`、system=2；作者内容／历史不变，pins=0，修正作者后新 Run 成功。作者删除／Undo 不改变冻结 Run 的 Mesh 和资源保留。 |
| D06—D08 | RunPane 实际销毁后任务继续；活动 Run 随正常窗口退出收尾；Stop 后新身份；准备期间 Stop；已有 Main binding／worker 状态／资源保留的首次发布失败收尾。不是全部驱动故障／设备丢失路径的穷举。 |

新增逻辑条目与 CTest 注册数不相加。真实物理按键／鼠标手势未在本轮冒充自动接口测试。

## 工程资格

- 独立 clean clone：`E:/lux-ed-d2/d3q/s`；RelWithDebInfo、EDITOR、`BUILD_TESTING=ON`，UI／publication diagnostics 均 OFF。
- 完整构建 `--target all -j 4 -- -k 0`：1014 个步骤通过。第二次 `ninja: no work to do.`。
- 安装到新 SDK，再复制到新位置 `d3q/relocated`；消费者通过该安装前缀构建运行。
- **37 项安装 CTest**、**9 个独立组件消费者**、**1 个直接 owner 测试程序**分别通过，不合并统计单位。
- Core／Material／Flow 业务消费者无 UI／window DLL 闭包；Scene 保留规范允许的 editor_rendering 依赖，不声称纯 CPU SDK 闭包。
- 生成器准确拒绝缺失反射组件，并逐字节保留原输出；新 schema value callback 和 typed Inspector 实际跨 DLL 使用。
- 四条 D3 桌面／GPU 路径记录 `validation_errors=0`，不是仅凭 exit=0 判断正常关闭。

完整生产构建最初固定在 ecaf2082，测试扩展后依次切换干净 tracked 候选。最终 28523b4e 相对 364466d9 仅修改 owner 测试的 LightHandle 判空；生产及安装消费者均再次 no-op，owner 单独重建运行。没有隐藏源码补丁。

早期测试驱动错误（取图时点、测试状态编号、编译器选择、LightHandle 判空）和失败日志保留原身份，不记作产品负例。构建失败后没有执行旧 EXE 作为新证据。

## 有限成本

五组独立进程配对使用旧 SDK 88bbaab0 与本候选，固定四对象、同一层级／组件输入和结构操作。每组均完成 76 次历史 revision 变化，保留量同为 457 字节；无 warmup。

| 主动结构操作耗时（微秒） | 样本 1 | 样本 2 | 样本 3 | 样本 4 | 样本 5 | 中位数 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 旧候选 | 736.4 | 741.9 | 795.1 | 750.6 | 746.5 | 746.5 |
| 新候选 | 528.8 | 568.6 | 569.2 | 606.0 | 546.9 | 568.6 |

该小场景的主动操作中位数降低约 23.8%；不能外推成大场景、复杂容器或整体 Editor 的固定收益。Render 代次负例不符合相同业务完成量，未拿失败的旧执行时间作加速比较。

另有五次进程测量 256／4096 个连续及高位稀疏源键，各执行 100 轮查找、校验独立 checksum、创建和退役；数据在 `cost-owner.csv`。逻辑键／值 payload 分别为 8192／131072 字节，**不是包括 hash 节点、bucket、allocator 和对齐的完整内存占用**。

Run 五次样本分别完成 20／22／22／21／27 个首轮模拟步，Main 帧数为 421／72／77／75／84；作者场景静止，所以每次仅一次 full update，正常 Run 未触发发布背压。另一个真实 producer/Main 联测每次形成两次背压，published=forwarded=2，ring 高水位 1，Stop 后排空；主动 CPU 工作、等待和 Stop/close 时间分别记录。
这些 Run 完成量不同，不做进程总时间加速或 FPS 比较。Stop/close 包含 View 和运输收尾，不是纯唤醒延迟。

未实施分配拦截、组件深层动态字节全量计量或逐操作 p95；未删除引用安全扫描、GPU 保护来换速度。ObjectEdit 暂存计费明确包含各容器／条目和内联值，有溢出检查；不覆盖 provider 内部全部动态容量，因此不宣称严格总内存上界。

## 保留范围与交审材料

D2-C 的 C19 双实际 Pane／排队通知完整组合、C24 物理持续拖动取消及文本 Ctrl+Z，以及旧桌面 native-close 发送者来源问题，继续保持原报告状态。历史材料仍绑定 88bbaab0，不用 D3 成功替代。旧报告中的“不实现 Run”是当时范围记录，本轮用户明确授权了上述有限 Run。

不包含插值、外推、自动重同步、动态渲染资源流送、新时基、MCP、Launcher、无窗口产品、真实机器人／SLAM 集成。未新增这些能力的完成承诺。

审阅包包含候选完整 Git 源码、相对基线 diff、Q01—Q14 与 R/C/T/D 映射、修前／修后原始日志、全部成本样本、SDK／DLL／测试 EXE 身份以及复用后的验证脚本。
`source-identities.json` 分开记录 Git blob OID、Git blob SHA256 与 Windows checkout SHA256；`artifact-identities.json` 记录实际产物，不用换行差异伪造源码变更。

后续仅等待独立审阅，不自动合并 main、推送或开展下一轮产品扩展。
