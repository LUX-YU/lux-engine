# NA1 第二轮：等待与结果交接的实际结果

实现与本轮联合验证完成；采用状态仍为 `NOT_APPROVED_PENDING_REVIEW`。不合并 V4/main，不宣称性能等价。

## 身份与修改

- 工作起点 `7d5cd844`；本轮 A 为 `21d106014e05cf0a7746d45fd05eac8ae01575f2`，原 C/C-common/C-flow 镜像。
- 最终生产/独立 clean clone 资格 B 为 `7f20191692d3f5e1037a14273a61fc99eef95714`，新 D/D-common/D-flow 镜像。
- `479d8db1` 实施六项交接优化，`c650e3e2` 修正编译限定，`7f201916` 补齐默认复制的对象尺寸证明。后续文档提交不是新资格源码。
- lux-cxx `3100f54d`、toolset `99c3d048` 与实际安装身份保持；只用 Lua55、原 patch/INC/16 MiB、Native ABI6、RelWithDebInfo。
- 复用原 Toolchain/Developer 两个构建槽，使用 next-tools/next-sdk 独立安装前缀。原 V4、A1、H1、main 未修改。

[六项具体修改及合同](NEXT-CHANGES.zh-CN.md)逐项区分冷期证明、动态输入与权限、借用和回收。
本轮没有修改 Lua 跨语言桥本身：压缩的是 Event 复制与完成、来源读取、记录构造、结果交接和已排空完成窗口。
固定依赖的容器检查仍保留，不通过删除逻辑容量、write pin、队列背压或预算取得收益。

## 当前生产对照的九腿三对

每腿三对 AB/BA/AB，共 54 个最终有效进程；原实例/预热/计时/seed/worker/affinity16/预算/资产保持。
每行业务计数与独立实例结果由原 oracle 比对；正常 GC。表中两侧为每实际操作成本中位数，变化列为配对比值中位数，不能把两列中位数直接相除代替配对。

| 场景 | A | B | 配对中位变化 | 三对变化 | 单位 |
|---|---:|---:|---:|---|---|
| P1 | 0.415418 | 0.389295 | -6.46% | -6.46% / -6.55% / -6.29% | μs/完整调用或任务 |
| P2 | 0.567993 | 0.575200 | +1.95% | -1.57% / +5.19% / +1.95% | μs/完整调用或任务 |
| P3 | 1.367165 | 1.332333 | -2.37% | -2.37% / -3.76% / -1.29% | μs/完整调用或任务 |
| P4 | 9.567158 | 8.858102 | -7.41% | -6.47% / -7.41% / -7.49% | μs/完整调用或任务 |
| P5 | 1.458816 | 1.378351 | -5.23% | -3.71% / -7.11% / -5.23% | μs/完整调用或任务 |
| S1 | 0.120082 | 0.122485 | +2.02% | +1.16% / +2.44% / +2.02% | μs/完整调用或任务 |
| S2 | 2.249821 | 2.205528 | -1.97% | -1.32% / -2.97% / -1.97% | ms/原帧 |
| S3 | 0.268002 | 0.246402 | -8.06% | -6.28% / -8.06% / -8.88% | μs/完整调用或任务 |
| P6 | 2.262544 | 2.220388 | -1.74% | -0.68% / -2.37% / -1.74% | ms/原帧 |

P1 单 Event；P2 NextStep；P3 三等待；P4 单任务 32 次 Event；P5 ValuePose；S1 Lua scalar；S2 C++ Sequence；S3 FlowForge Event；P6 混合 Physics。
全部总时间、prepare、每批 p50/p95/p99、实际工作、错误和 backlog 见 [机器可读结果](next-result.json)及原始归档。
P1–P5 每进程错误/backlog/lease 为零；其他腿保留原驱动的错误观测范围。逐批错误、单独恢复延迟、正式计时分配未采集，保留 null。

没有剔除最终慢组，没有将旧候选四次已完成计时混入本表。旧候选还有一次中止进程，记录完整保留。
本轮未重跑 V4：不能把不同时段的历史百分比连乘来证明最终候选胜过 V4。

**成本不是全面收口。** NextStep 配对中位 +1.95%，三对为 -1.57%/+5.19%/+1.95%；两侧操作成本中位 567.99→575.20 ns。
Lua scalar 三对均慢，中位 +2.02%，两侧 120.08→122.49 ns，绝对差约 2.40 ns/调用。
这两腿没有进一步分项采样或因果消融，不能认定只是噪声、必要安全成本，或用 Event 收益抵消。本轮按有界范围保留为明确的新增残余，交独立审阅决定是否接受。

## VTune 与机器码

P4 一对独立 software Hotspots，ITT 限定完整任务波 ROI；每侧 640,000 个任务、20,480,000 次等待/恢复。准备、预热和关闭观察不计入该 ROI；正式计时另跑。

| 指标 | A | B |
|---|---:|---:|
| ROI wall 秒 | 5.920893 | 5.449164 |
| VTune CPU 秒 | 5.742730 | 5.293618 |

下表是各目标最大的一条调用路径的包含 CPU 秒，父子节点不能相加，也不能把 resumeOne 的整棵子树称作恢复管理成本。

| 目标 | A 包含 CPU 秒 | B 包含 CPU 秒 |
|---|---:|---:|
| ScriptExecution::resumeOne | 4.484021 | 4.422691 |
| ScriptExecution::waitEvent | 1.347797 | 1.297760 |
| ScriptInstances::eventSource | 0.331969 | 0.225997 |
| ScriptExecution::admitAwaitable | 0.398197 | 0.359187 |
| ScriptEventWaits::registerWait | 0.363454 | 0.410981 |
| ScriptExecution::takeAwaitable | 0.317432 | 0.604312 |
| ScriptExecution::completeClaimedEventWaiter | 1.015254 | 0.643631 |
| copyPayload | 0.101205 | 0.015136 |
| ScriptExecution::drainExternalCompletions | 0.046336 | 未单独采样 |
| lua_pcallk | 0.807310 | 0.672306 |

默认 int payload 的优化产物及私有内核反汇编保存在 next-machine-code/next-analysis。
实际 copyPayload<1>：TypeId 立即数 `0xA2BCFC75D3D77326`；保留本次输入五类检查；有效路径为一次 4 B load/store，没有 FNV 循环、call 或间接跳转。调用这个 projection 本身的分派仍存在。
finishPreparedEvent 的可见外部 relocation 只剩 ResumeRing::push，没有通用布局验证和二次来源释放。
反向结果也保留：registerWait 包含时间 0.363→0.411 s，takeAwaitable 0.317→0.604 s。它们包含容器/释放等子节点，不能把源码少一个临时对象直接称作该子树更快。
优化产物里的 takeAwaitable 仍有一次 ScriptOwnedResumeValue move 构造；optional 的原位构造仍含既有值销毁分支。没有把这些残余谎称为零。
这些证据证明具体代码变化；没有逐项因果消融，不能把全部耗时变化分摊给六项改动，也不能用软件采样宣称 cache miss 或分支预测根因。

## 布局、资源与保留限制

MSVC 实际布局：EventSourceAccess 48→16 B，包在 expected 中为 64→32 B；AwaitableRecord 192 B、EventWaiterRecord 88 B、ContinuationRecord 72 B、AwaitableOutcome 104 B 均不变。
Execution 472 B、Ingress 48 B、ResumeBatch 24 B 不变。五个最终 coroutine frame 仍为 224/288/416/224/304 B，512 B 上限及 N×R backing 未缩减。
P4/10,000 实例 prepared 快照：两侧 Lua live 11,138,443 B、active pages 21,102,592 B、pinned slack 445,056 B；awaitable backing 2,334,160 B、native frame backing 5,760,000 B 均相同。
两侧 heap_alloc/free 均为 328/5；这些是本次资源快照中的 Lua allocator 下层计数，不代表全进程分配。全进程 heap busy 100,501,480→100,503,576 B，不能宣称内存减少。
完整布局、最终 frame 立即数、1,000/10,000 实例的 prepare/wait/warm/end/closed 资源快照见 JSON。
EventSource 借用及 Outcome 临时交接的变化不等于整体 VM 内存减少。Awaitable/EventWaiter 逻辑与物理配额、固定 frame pool 上限保持。
资源诊断与正式计时分开；VM/root/本地下层 heap 统计按实际字段记录，不把未采集写零，也不把 heap 调用称 OS syscall。
仍有身份关系、容器 find/erase、来源 unlink、结果保护/搬运、Lua pcall/registry/table 等成本。本轮未宣称它们全部必要或全部可移除。

## 正确性、安装与失败记录

- 同一 clean tracked commit 的 Toolchain 110/110、Developer 127/127 CTest；两个 all 构建及第二轮无工作。
- 固定且 hash 匹配的 Lua55 VM 合同 4/4；未恢复 Lua54/JIT，未测试 Android。
- 16 个安装消费者重新生成/编译/执行；原 13 类值转换增量、5 项组合增量及 3 条迁址链通过。迁址期间原受控 source/build/SDK 不可用，结束后全部恢复。
- 新负例涵盖 default/custom 的实际结果、坏输入不调用 callback、错误对象尺寸 guard，以及空/未发布/背压/后 frontier 完成窗口。原 lifecycle、pin、Timer、Event、预算、合法重入及安装纵向断言保持。
- 首次构建的 namespace 错误、Windows PowerShell stderr 包装失败、补尺寸证明前的验证/计时都保留并单列；不标作最终候选回归或最终有效组。
- 只有上述机器和配置获得本轮证据；历史成本、未解释份额及未验证环境继续保留。

[原始 ZIP](evidence/next-hotpath.zip) · [逐文件 SHA、源码与产物身份](evidence/next-hotpath.json)。
停止等待独立审阅；不发布 tag，不自动扩展下一项目。

[固定提交原始证据下载](https://github.com/LUX-YU/lux-engine/raw/e23d623684a3f8cf9aca174fcb30a8b6b0020013/.internal/script-native-lua-na1/evidence/next-hotpath.zip) · [远端 741 项回取核验](evidence/next-remote-readback.json)。
