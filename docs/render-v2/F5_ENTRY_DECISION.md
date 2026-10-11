# RD2-F5 入场裁决 — 2026-10-11

状态：用户正式授权 F5；不授权 F6/F7。本文记录新的入场决定，不修改历史资格结论。

## 锁定来源

- F4-C implementation：`5039fb42424dfda23bc7da518e7bbcf863f8c932`。
- F4-C-P implementation：`92b126f67873ccbb576d0ffc5f1330964d6f84f6`。
- F4-C-P verification / 批准基线：`719f3d06e077e639901ae598f230653c5312c905`。
- [F4-C 原报告](https://github.com/LUX-YU/lux-engine/blob/6342d56caf826504278399e2430a2a4499d9d3c2/docs/render-v2/F4C_VERIFICATION.md)。
- [F4-C-P 原报告](https://github.com/LUX-YU/lux-engine/blob/719f3d06e077e639901ae598f230653c5312c905/docs/render-v2/F4CP_VERIFICATION.md)。
- 新裁决来源：本轮用户《RD2-F5 — 正式实施授权》。它取代备用 F5 工作单中“必须先 F4 PASS”的入场前提；其余阶段责任继续适用。

## 有限延期，不能改写为通过

F4 继续为 PARTIAL。原不利样本、测量协议、报告和二进制保持原样。

| 冷态指标 | F4-C 原 p95 回退 | F4-C-P 配对 p95 | 原报告置信区间 | 本轮处置 |
|---|---:|---:|---:|---|
| C9_identity_and_candidate | +6.5574% | +3.4483% | [-1.4085%, +12.0690%] | 不确定性延期；不宣布满足 5% 门禁 |
| complete_native_candidate_including_cook_io | +5.9257% | +2.9952% | [+0.3965%, +6.8838%] | 不确定性延期；不删除 I/O 成本 |

原始证据：`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C-P/92b126f67873ccbb576d0ffc5f1330964d6f84f6/`。
其 evidence-manifest SHA-256：`737a6f3c9afe13f03d6bf9577914fe1d1e8b00a264cbaf631b09471b2d4f582f`。
完整分段、AA/BB、热态统计及其他未关闭置信区间以原报告和原始数据为准；本表不是重新运行结果。

F5 若修改实际冷编译输入/执行路径，必须在影响报告中列明，执行有界的相应性能评估。不得把这次延期解释为可任意改变冷态工作量、豁免新的回退或无限追加测量直至出现有利样本。后续阶段保留这两项债务及其来源，关闭须有新证据或明确用户裁决。

## F5 的独立门禁

真实单一 Runtime、四 View 共享 immutable Native Plan、独立有界 Invocation、共享 Scene 工作证明、两 Scene 共享 backing、真实 Target/Presentation/Resize、独立帧进度、Stop/Device Lost、GPU 数值或图像 oracle、热态零首方分配和生命周期门禁均未获豁免。

F5 尚未实施或通过。旧 156 项 Normal/Full ASan 和 GPU 资格只作为回归来源，不能替代本阶段验收。不得提前实现 DataSchema/TypedStorage、Feature SDK、Runtime 内自动设备恢复或第二执行框架。

## 工作区保护

开工远端重新核验为批准基线。隔离施工检出也位于该 SHA，初始干净。
用户原检出的六处修改与另一 V2 检出的 `Operation.hpp` 修改均与前次保护快照逐字一致。
本轮原始 HEAD/branch/status/diff/untracked/字节哈希和只读树快照保存于源码树外：
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-F5/preflight-719f3d06/`。

本阶段实现 I 与独立 clean-clone 验收 V 分离。V 只能新增验收报告。推送后 STOP，由用户决定是否进入 F6；F7 不随 F6 自动授权。
