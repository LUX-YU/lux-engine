# Render V2 v11 S1/S2 — 最终实施说明

施工起点：98798da75a201933e6950ef5631444fc199e783a。隔离分支 codex/render-v11-s1s2；目标推送分支 codex/render-v2-rebuild。
用户原工作树未施工。输入包和 G0 保护快照见 RENDER_V11_S1S2_FREEZE.md。
这是 Implementation 说明；最终独立 V 另提交，仅以精确最终 I 的 clean clone 实测判定。

## 门禁与唯一权威

| 门禁 | 提交 | 实际结果 |
|---|---|---|
| G0 | 35069de3991732c13d69d76a2ad3c40f9345a0b7 | 基线、21项包清单、719 blobs、消费者和白名单冻结 |
| G1 | 360c895236350fd5ba65c7b57106d168449c9a16 | 真实 C++ IPC 对象、静态 TypeOps、跨模块代码保活 |
| G2 | 1302d56aa8483a61537d5f8001e4624a543e7387 | 原目录原地迁为 Command，类型化绑定、请求和完成 |
| G3 | 222a90730dbb077372f0f19a10283a20076c4a7a | S1旧接口/Command Wire删除，87/87×3 |
| G4 | 82ec60ff572b0608a720dde18ce6c5e20db833ee | CPU完成事件、现有宿主Scheduler、94/94×3 |
| G5 | 6847e6693d1ceca8a61cbc53ff54f05d3f13d7f3 | 物理Queue回调、统一upload、LastUse、96/96×3 |
| G6 | 本 Implementation | 真实Hash碰撞、CPU成本、安装/布局/时间线及独立验收准备 |

IpcMessage唯一拥有实际构造的对象、opaque元数据及代码Pin；IPC物理SPSC/Channel/Link算法不变。
Client原SlotMap唯一分配RequestId，BudgetLease记入动态capacity和Asset完整backing，返还原请求可回收vector容量。
Server::Impl原SlotMap是唯一Command目录；Native通过项目内借用接口完成专业实现，Native不反向include RenderServer。
同一Impl的固定StableSlotMap唯一拥有原ReceivedRequest、有限Sender operation和GPU/Readback完成状态。
CPU Receiver只发布该槽的Session/代际身份，宿主CpuScheduler从既有ExecutionRuntime注入；Render不创建线程池。
DeviceSession继续唯一拥有设备、资源Registry、物理Queue、VMA及Staging账本；Effect保有唯一CompiledGraph。

Command声明采用稳定名称和Request/Response，bind只在完整名称、类型、ABI、尺寸、对齐验证成功后形成不可修改资格。
热态直接CommandId索引；无第二个目录或每帧Schema/Feature名称解析。外部Buffer和Texture都通过相同注册接缝实现。
Reply/原请求/单独取出的Readback Payload可晚于Client、Server、实现DLL，最后代码Pin在实际删除器返回后释放。
跨DLL外置对象由canonical TypeOps在正确模块分配和销毁；嵌套自定义资源仍要求兼容allocator ABI或自己的deleter+Pin。

## S2机制与实际删除

CPU事件队列使用lux-cxx有界BlockingRingQueue，短临界区仅属于有限CPU通知。每活槽预留一个终态落点，
通知发布后不再访问可能已经被消费的槽。最后发布尾栈计数覆盖queue通知返回；停服等待所有尾栈结束。
已重现并局部修复PortSender同步完成后start访问已销毁operation的UAF；原始ASan失败证据保留。

每个物理SubmissionQueue有固定容量、有序侵入回调；提交前保留额度，真实Submit接受后挂接Ticket。
Queue poll只推进完成前沿，显式安全点先摘节点再调用，不在callback返回后读取节点。
Transfer仍是唯一分块、CPU Staging、Buffer/Image Copy、QFOT状态机；专业Texture候选只处理格式/Region/发布/回滚。
唯一render::upload(storage,data)由StorageTraits进入真实Texture和外部TestBuffer实现。

已实际删除：runtime Operations.hpp/.cpp、Wire.hpp/.cpp及Runtime专业Texture实现；
旧Runtime Vulkan GpuCompletion头；Native TextureUpload.hpp/.cpp、private UploadState.hpp。
保留的同名TextureUpload.cpp是历史行为测试；Session私有UploadState定义移入Transfer.cpp，原Staging/预算/QueryPool能力保留。
剩余DataSchema/Wire用于Feature/Storage配置和Binding，不是Command编解码。没有兼容转发头或假Target。

LastUse是最多三个物理Queue的固定值；同Queue serial取max，不同Queue分别保留，Copy/Draw在Submit接受时合并。
完成旧T1不会覆写新T2。Prepared Binding只借稳定地址，Retirement使用同一条件。
正常tick不扫描全部Deferred/Transfer等待项。已有Context帧槽复用Queue.poll、资源退休收集、冷态拓扑取消遍历保留，
不宣称整个渲染热帧只有一次poll，也不取消必要的GPU错误检测。

## 必要补充与失败记录

最终复核发现G3明确记录的CommandName真实碰撞尚未构造，G6补入可复算的FNV1a64向量：
gMPflVXtwGDXbIhP73TX 与 LtHf1prlU1bCeYZEdqWf 同为0x8fcf3be2de898214。
未修改Hash或伪造ID，真实Server注册/Hello/Bind/双Client调用保持不同CommandId和1005/2005响应；完整名称不混同。
公开primary源码固定commit与独立复算结果保留在证据中。

G5新DLL Readback测试最初错误要求UNORM精确128，实际前后均127且全buffer完全相同。改为原有±1像素oracle+
析构前后逐byte相同，未修改生产颜色。G6首次编译暴露测试namespace别名detail歧义，局部更名修复。
各次失败原始日志留存，不删除旧测试或放松生命周期保证。

## 测量边界

最终Layout由实际MSVC TU测量，无生产friend/accessor：CommandName24/align8；CommandId、RequestId8/align4；
CommandHandler32；CommandEntry136；Client Pending48；CommandContext8；BoundCommand<CreateView>72；
IpcTypeOps88；IpcMessage144/align16（object80）；IpcReply304/align16；RenderRequest256；RenderCompletion384；
CompletionEvent24；DeferredCompletion736/align16；SubmissionCallback和SubmissionRequirements各48。
Deferred固定容量为配置的completion+compile（最多136），不能用无界vector换零分配。
Binding32、PreparedBinding64、RetirementEntry72，10k记录1,343,640B/61冷态分配保持。

96/144/208B真实SBO候选、32/64/128B与双核原始样本见G3；96不能容纳真实Batch+BudgetLease，选144。
历史21.6922ms离群和IPC R2 PARTIAL不改写。小型Command编码已由真实对象构造替换，完整请求测量仍包含提交、
Handler和类型化消费；固定sleep_for(1ms)、等待和调度单列。外置128B对象有实际分配，不宣称全路径全对象零分配。

CPU成本新测试复用一个Server/Device/实际ExecutionRuntime，sync/normal/burst × N0/1/16/128，每组5预热+100批。
记录真实登记、入队、取出、采用、Owner cycles和GetThreadTimes；精确受控burst证明queue peak=N、K=N、drop=0。
仪器本身有开销；GetThreadTimes短区间量化为0不表示CPU不工作。正常路径peak是采样值，不伪装连续最大值。

LastUse CSV记录真实Submit CPU begin/end、终态观察、发布、Work reset后Staging租约归还、源最后析构、T2收集销毁。
Region在三配置中仍使用Graphics；多chunk取消场景实际仅接受首chunk并证明无T3，完整多chunk由原上传测试覆盖。
实际VMA范围/Descriptor/Backing销毁被核验，此路径没有独立Arena。GPU timestamp未测的时间线明确NOT_MEASURED；
原Transfer查询支持的GPU时间另表，绝不将CPU wall当GPU时长。

旧新交替样本、最终矩阵和保护结果在运行完毕后追加。没有等价工作量证据不宣称性能更快。
Root TOOLCHAIN仍因旧入口render/client、feature_client.cmake、standard_content缺失而BLOCKED；
不以模块bootstrap成功替代完整产品资格。Linux/Android构建与TSan NOT_RUN；Android仅同步安装头。
S3–S6及G11B–G13未施工。最终独立V通过并正常FF推送后停止。

### G6 追加校正

CPU成本夹具最初错误地用逻辑Pending上限136作为物理SlotKey数组下标上限。lux-cxx StableSlotMap实际以256槽页
预留，free-head从255开始；观察表改为覆盖这一真实页，逻辑准入仍128+compile，K/drop/峰值要求未变。
首个实际Key与原失败均归档。该修正不改变生产SlotMap或队列容量。

Batch现同一个真实Server、两个Scene各两View、四OffscreenTarget、一个共享Graph持续运行，
LOCAL/RANDOM各160样本保存逐样本提交/tick前后值，并核验每组真实GPU推进、四View末态像素和编译/Feature调用次数不变。
这仅是并行渲染负载：CPU测试values未冒称已经接入GPU transforms。原全部预算、容量重用和revision/epoch断言保留。

### G6 实施环境最终结果（独立 V 尚未执行）

- Release 98/98（99.54s）、Debug 98/98（103.03s）、MSVC ASan 98/98（133.93s）。
- 三配置全量 all -j 4 -- -k 0 和第二轮 no-work 通过；79 个原名称全部保留，Client compile commands 不含 Native/Vulkan。
- 最后只调整 Batch 对照的测量位置：原始 samples 保留全部分配，原 Server stop/join/reset 后再离线测同次数 move，避免减掉并发 RenderThread 分配。该改动后重新执行三配置 all、no-work、typed_batch，均通过。
- Native/SyncValidation Error=0，ASan Error=0；真实 warning 计数和未使用 CMake 变量警告保留在原日志。
- CPU 受控 burst N=128 的 100 批共 12800 个通知均到达、消费与采用，peak=128，drop=0。
- G5 审查记录的实际 Hash 碰撞及四 View 同场 Batch 两个缺口已由最终新增测试关闭；原审查文档不改写。
- Legacy719、用户两处修改、依赖版本、IPC物理算法与历史证据保护检查通过。

同机旧/新各3轮、每轮小Command1000样本，旧源精确G11A-R I=5cb564d6245c8a3c009ecc528fde6866643343ac，
它与本轮起点之间仅两份报告；DLL与EXE SHA、每轮起止时间及所有样本归档。

| CPU度量 | 旧 p50/p95/p99/max | 新 p50/p95/p99/max | 解释 |
|---|---|---|---|
| 完整小Command往返 ms | 1.9947/2.0647/2.1676/2.9914 | 1.9916/2.0518/2.1841/2.5785 | 包含固定宿主等待与调度，不是IPC本体耗时 |
| Native录制 execute us | 104.9/166.4/194.3/1385.1 | 104.4/174.5/205.4/1520.1 | p95约+4.9%、p99约+5.7%，不宣称更快；全部离群保留 |
| Dedicated上传并行四View间隔 ms | 2.0041/2.2468/2.9722/4.1473 | 2.0097/2.7342/3.4963/4.3388 | 上传吞吐工作量不同，p95约+21.7%，只报告观测 |
| Alias上传并行四View间隔 ms | 2.0071/2.3158/3.1144/7.0725 | 2.0347/3.7528/3.9645/4.6857 | 新实际上传频率不同，p95约+62%，不据此推断纯机制退化 |

小Command的EXE及Client DLL各3000样本 new=0；四View稳定帧首方new=0；R5相同/不同Shader的
Pipeline/Bind/Scope/Draw仍为1/1/1/2及2/2/1/2。旧Texture总时长与新专业Storage事务的计时边界不等价，
不能由新总时长更短宣传GPU更快。最终V会再次采样并记录它的独立数字。

施工证据根：E:/RenderV2Rebuild/V11/evidence。完整命令与脚本在同级runners；最终按精确I归档至
E:/RenderV2Rebuild/V11/archive/<I>/。最终V提交记录不可变INDEX SHA256，不使用本说明替代验收。
