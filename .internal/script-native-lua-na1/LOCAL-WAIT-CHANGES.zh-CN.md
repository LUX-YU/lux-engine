# 本地等待与同步步骤：实施记录

状态：实施中。下列合同是本轮输入，尚未执行的检查不代表通过。

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
