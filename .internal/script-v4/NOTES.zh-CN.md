# Script v4 资源合同与资格范围

本轮从 `d45b7e9284b34686fac5425fed57a79d17ffb0d3` 开始。原五腿对照镜像的实际源码为
`e06208deb29b9b1de52fe49a129091f693bb7e08`。仅 Lua 5.5.1 + lux-lua55-v3-r2、Native ABI 6，
INC 上游参数和 16 MiB 完整空页预算。最终实测与状态以同目录 RESULT/result.json 为准。

## 资源所有权

- `LuaValue.cpp` 的 Operation 声明每个受保护 callback 所需的栈额度；trampoline 在自己的
  CallInfo 上执行 checkstack。失败返回 VM_FAILURE，外层 C++ frame 负责原有对象清理。
  caller 的额度不能代替 callback 额度。默认简单操作额度 8，深度上限仍 32，计划最大额度 136。
- `LuaPageAllocator` 独占页、Block owner header、partial/class-idle/global-idle 链和 direct 块。
  9 档为 32/64/128/256/384/512/736/1536/4096。partial 与 class-idle 共用 available 链接；
  全局淘汰链有独立链接。完整空页同时登记在后两条链，全部计入同一 16 MiB 预算。
  同档空页保留 freelist/stride/carved/header；跨档仅在 live=0 时失效这些信息。
  预算满时淘汰全局最旧空页；显式 clear 只释放空页。失败沿用一次 direct fallback，失败保留旧块。
- `BoundedClassStorage` 的 active/free/generation/size/capacity 是必需状态。步骤、峰值与 live/occupied
  累计是可选观察，默认关闭，Stats.observation_collected 明确区分未采集。
  C++ frame Header 从 storage pointer + Allocation 改为 24B Ticket：owner、64 位 generation、page/slot。
  释放检查 owner、活跃位、完整 generation；原 Allocation 入口仍检查 data 和 size。
- Native Instance 的 resetForReuse 先使 native_context 失效，再清除三种发布关系与 module/state 引用；
  abilities/local_abilities/events 的容量保留至 backend 销毁。reserve 在重新发布前完成，包含失败退出的
  retained_binding_bytes 按实际 vector capacity 计数，不在 stats 查询中扫描全部实例。
- `NativeFrameStorage` 是 Native 模块 pinclude 的私有存储，不进入 SDK。状态存储仍为 BoundedClassStorage。
  原相等 envelope(size,alignment) 共享 domain，不同 envelope 隔离。每域 N 个 R=alignUp(maxSize,A) 区域。
  方法 prepare 取得有引用计数的尺寸/对齐 class；最后一个 prepared 引用只能在其 frame 全部销毁后释放。
  非空 region 服务一个 class，使用 class 非满链；全空 region 回到本域空链。slot 空闲索引用 memcpy 写入
  已销毁 payload；过小 stride 保持一 region 一 frame，不越界写索引。
  完整代次和 owner 在不可复制 Lease 中，Lease 位于现有 NativeContinuation；Ticket 是只读值快照，
  不能代替 authoritative Lease。没有第二份 N 条 frame 身份目录。
  所有 payload、region/class/domain 元数据和嵌入 Lease 按原配置预算校验；不足真实拒绝，不提高预算。

## 容量与回收证明

若某域已有 k<N 个 frame，每个非空 region 至少含一个，故非空 region 不超过 k。
即使当前 class 无空位也必有空 region，能容纳任意不超过 R 的合法 frame。小 frame 不跨区域、不移动。
逻辑 continuation 先预留；物理获取失败归还预留。destroy 用户回调结束后才归还 frame 和逻辑槽位。
prepared layout 释放失败保留真实 prepared 记录。原 single-flight、结果/pin、取消、frontier、step 与预算不变。

## 观察与实验边界

诊断只在创建期启用，固定每 class 记录；Event 使用 16384 项固定 phase 缓冲，不从 VM allocator 分配。
现有 3000 周期四阶段记录为 12000 项，无截断。GC 内部 state/debt/cycle 未采集，明确为 null；
实际六项 GCPARAM 从 owner 侧诊断快照读取，不在 lua_Alloc 中调用 Lua API。
采样是一次完整进程的软件 Hotspots，含准备/warmup/检查/关闭，不冒充精确 ROI 或 PMU。

W3 固定 W2 产物比较 G0 INC 原值、G1 INC pause150、G2 GEN 原值，各 Event/原 record 一次，
计时和诊断分开。未见联合收益，保留 G0。另一次 32 MiB Event 是预算实验，未改变生产默认。
观察关闭的数值在最终报告为 null，不是零工作。

## 验证追踪

- W0：API-check 修前 `2de21a100` 深度16触发 CallInfo 栈断言；修后 `000005033` 深度1/16/32、
  内层增长 OOM、外层资源存活/析构及再次转换通过。当前 MSVC 深层 expected<T> 构造探测触发 C1054，
  最大深度 fixture 直接验证同一 typed read callback 与 consumePlain 构造；普通公开 Codec::read 既有用例保留。
- W2：page_allocator_test 的同档32次零块头重写、跨档、最旧驱逐、零/16MiB、Track两模式、
  单块钉页、resize 失败保持、真实 thread/GC/close；VM API-check 与正式 VM 身份分开。
- W4：class_storage_test owner/stale/代次耗尽两模式；cpp_static 原断言和既有512B容量；
  native_backend_test 32轮绑定缓冲复用、状态重新初始化及零活跃资源。
- W5：native_frame_storage_test 全大8、第9失败、4096次混合、旧票据、错owner、超对齐、tiny、
  domain隔离、32次layout复用；native_population 保留原共享/隔离，补实际DLL destroy重入和start失败清理。
  FlowForge integration 额外编译同一模块的16B与528B方法，使用真实生成 ABI/start/resume、typed provider，
  后端级 fixture 控制完成来源。8个小frame occupied128/active-region528，8个大frame occupied4224；
  两者 reserved4224 不变，metadata784。原 Simulation/ScriptSystem 集成断言继续独立运行。

Windows RelWithDebInfo 的最终资格、15 consumers、迁址纵向链、生成增量和有限成本统一执行一次。
不据此推导 Linux/Android/其他编译器、任意游戏工作负载或性能等价；不进入下一阶段。
