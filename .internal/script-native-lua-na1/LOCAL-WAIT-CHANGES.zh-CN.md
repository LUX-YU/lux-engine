# 本地等待与同步步骤：实施记录

状态：选定实现及验证完成，等待审阅。最终结果见 [交付报告](LOCAL-WAIT-RESULT.zh-CN.md)；以下按阶段保留原始记录，后文的“尚未”描述当时状态。

## 身份

- 工作树起点：`d63767c29315c2d78e5cfc904c0ce4c2a80875ba`。
- 生产及测量参照：`7f20191692d3f5e1037a14273a61fc99eef95714`，沿用匹配的 D/D-common/D-flow 镜像。
- 两者差异只有报告与原始证据。main 工作区的七项未知修改不动。
- 仅 Lua 5.5.1、现有 VM 补丁、INC 默认参数、16 MiB 完整空页预算、Native ABI 6、
  lux-cxx `3100f54d`、toolset `99c3d048`。仅 RelWithDebInfo。

## 本轮明确更新的顺序合同

事件 occurrence 的先后保持。同一个 occurrence 唤醒的不同任务不再承诺登记顺序，允许稠密删除。
不得以此重排不同 occurrence，也不得改变 claim → 普通 callback → 完成等待的边界。
一个 ScriptSystem 独占本地脚本状态；Scene 不增加脚本事件入口。并行生产者与脚本执行点之间
仍由现有 TaskGraph 同步。独立线程产生的外部完成继续使用原运输边界。

## 实施与检查

1. 合并 Event 等待关系与结果的物理存储，移除第二个 SlotMap 的创建/查找/删除；
   EventWaits 独占其关系字段的修改，Execution 独占结果和执行状态。
2. READY 状态以结果记录为权威；恢复顺序只保留必要通知。取消留下的过期通知仍消耗一次预算。
3. 检查连续等待的存储复用、按事件访问和实例关联；保留额外等待、未关联等待、外部等待、
   大结果、写入期间延迟释放的原容量语义，不能靠占住旧逻辑等待槽复用内存。
4. 将同步步骤及 Event 的不变关联放在绑定处，检查当前输入和可失效权限；
   用户代码后重验原调用，不能重新捕获 ACTIVE 把旧调用变合法。
5. 大 payload 共享只在真实来源和寿命合同允许时实验；32 位 payload 不强加共享计数。

现有取消/重入/pin/frontier/预算/生命周期回归保留；同事件兄弟任务的顺序断言可改为逐任务值与
次数，但不同事件的先后断言不得排序后比较。每一步登记实际代码、命令、源码身份和结果。
最终同量对照以本轮参照为 A，未测字段保留 null，不把旧性能债务归为新实现收益。

## 已执行

- `459a3671`：结果内嵌事件关系，删除 EventWaiter SlotMap；恢复通知由 24 B 改为 8 B。
  `local-wait-first-build/tests`：Developer all 成功、脚本/Lua 88/88。
- 后续固定结果存储替换 StableSlotMap：物理对象在准备时建立，归还逻辑槽不销毁物理对象；
  每次登记仍更新公开等待代次。来源关系和实例等待链直接使用稳定记录地址。
  接受/取消/正常 take/写 pin 最后释放均走同一结果存储，没有第二个结果池。
- `testClaimedCancellationAndSlotReuse`：两槽满载，在第一项 copy 内取消第二项并重用旧槽，
  旧 occurrence 快照被拒绝，替代等待只收到下一 occurrence（91、92），三个 frame 各销毁一次。
- `testRepeatedWaitStorageReuse`：等待/事件容量均为 1，连续 64 轮，实际值、恢复和销毁各 64，
  backing 不增长、每轮最终等待数为零。
- `local-wait-bank-regression-build/tests`：all 无工作检查成功，脚本/Lua 88/88。
  先前 `local-wait-bank-tests` 的筛选只命中两个测试，不能代替后续 88 项结果。

尚未测量本轮性能。全局事件任务数组、按事件批次压缩通知及大 payload 共享需要独立核对：
现有链已直接访问稳定结果，不再查第二个目录；不能仅为替换容器增加按 endpoint 成倍预留。

## 步骤与发布边界

- `ScriptCoroutineContext::bindStep<Signature>(ordinal)` 一次解析，绑定借用仅在所属 coroutine 生命期内有效；
  每次实际调用仍捕获原资格并在 Lua 返回后重验。动态 `callStep` 和绑定调用共用执行内核。
  32 次等待业务、真实 native-lua fixture 和安装消费者已迁移；参数与结果规则未改。
- CppStatic 的 Event import 改为稳定只读 view，删除每次等待的 backend instance slot 回查与 resolver 调用。
- Lua/core 资格上下文直接指向 Instances 独占的 InvocationState，生命周期标志、退休 epoch 与关闭权限
  也归到该状态，避免通过冷 Mount 再寻找热状态。初始 capture 后无用户代码区间不再立刻调用同一检查。
- 组合模块持有的步骤发布与其 native 子对象共同存活；此事实只由内部装配设置，公开 view 的复制不继承。
  外部可变 producer 保留 publication/身份检查，零 epoch 与空 owner 的合法 producer 仍支持。
- `local-bindings-tests` 保留错误地在构造期要求 ACTIVE publication 的失败；
  `local-bound-step-tests` 保留生成器 alias-template CTAD 和误拒绝零 epoch 的失败。
  修正未改变业务断言。`local-publication-boundary-build/tests`：all 成功，88/88。

## 按事件访问与复制

- 等待页每页四项，页内稠密，取消用末项补洞；各事件共享预留页池。预留 N 页保证 N 个等待
  即使全部属于不同路由也能接受，不能把常见 N/4 页使用量冒充最坏预留量。
- EventWaits 不再有实例目录或实例链。实例退休先沿 Execution 的等待链撤销 Event，再按旧顺序
  清理 Timer 和结果；仍只访问被退休实例，冷期多一次该实例等待链遍历，避免热期维护重复链。
- occurrence 的 claim 在 callback 前完整摘出当时的等待，因而删除无用途的逐等待 64 位登记序号。
  删除 `SEQUENCE_EXHAUSTED` 枚举项；其他错误的整数值保持（9 留空）。公开等待 token 仍为原 32 位代次。
- Event 的已验证 import、scope 关联由 Execution 借用。每次入口仍核对实际 handle 与当前权限，
  不再回到 Mount 找 scope、再回到 endpoint 判断路由。Entity 销毁通过已连接的 attachment 信号先撤权，
  不重复执行 Registry::valid；提交和实际初始化前的 Entity 检查保留。
- 内建 scalar copy 的不可重入性来自 typed factory 的真实 callback 身份。公开 callback 被替换即失效。
  分支每 occurrence 选择一次。成功路径不建立 pin/嵌套调用保护；copy 出错或队列满时重建原清理保护窗口。
  自定义 copy 路径和所有实际 payload/type/size 检查保留。
- `local-dense-events`、`local-event-ownership`、`local-event-imports`、`local-pure-copy`：
  每组 all 成功、脚本/Lua 88/88。新增 `testDensePageCancellation` 覆盖页中间删除、补洞、补满及最终释放。
- backing 新增单列页池、claimed ID 和恢复 ID 字节数；嵌入的事件关系已经算在结果存储内，不能重复相加。

尚未完成本轮最终安装/迁址/生成增量与同量成本。大 payload 共享不加入当前候选：主长任务 payload 为 4 B；
自定义逐等待 copy 可重入、有可观察副作用，不能用一次共享复制代替。已有 channel 双缓冲的寿命也不覆盖
跨 step 的 READY backlog。本轮不制造新大 payload 业务来宣称收益。

## 最终资格

固定 `841320a3`：Toolchain 110/110、Developer 127/127、16 consumers、13+5 增量、3 迁址、4 VM 合同通过。
完整成本、布局代价、scalar 残余及首轮无工作检查限制统一见 [最终报告](LOCAL-WAIT-RESULT.zh-CN.md)。
