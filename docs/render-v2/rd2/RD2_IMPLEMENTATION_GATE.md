# RD2-D0-C1 阶段准入与未完成事项

## 当前状态

RD2 是 Candidate；本轮授权仅 doc-only D0。Implementation 的设计审阅不是独立资格，最终结论由 RD2_D0_VERIFICATION 给出。D0 文档 PASS 后仍需用户批准架构，不自动 supersede FINAL。原 FINAL、历史报告、四 CSV、719 Legacy 均只读。

| 阶段 | 本轮允许 | 进入条件与责任 |
|---|---|---|
| D0 | 新增 rd2 文档、修订候选、I/clean V/推送/STOP | 仅文档资格；任何未解决的设计矛盾标 PARTIAL |
| F4-C | 不允许 | 用户审查 RD2 并另行授权；证实/证伪三项 native 风险，关性能门禁，明确原 F4 缺失能力责任 |
| F5 | 不允许 | F4 PASS + 用户单独授权；唯一 Runtime、4 View 共用计划、bounded invocation slots、独立进度；仅 DataSet Scope 接缝 |
| F6 | 不允许 | F5 PASS + 用户授权；CPU/Wire/GPU 同源可选合同、多 structural owner、最小真实固定容量 DataSet backing、installed SDK |
| F7 | 不允许 | F6 PASS + 用户授权；扩展同一 backing 基底，typed CPU storage/dirty/residency/Projection，不造第二 Store |
| F8–F11 | 不允许 | 原业务功能与产品职责保留；F11 关闭 F01–F36/I01–I44 |
| H1–H6 | 不允许 | F11 后逐项授权和算法资格；不以存储机制或 F11 代替 H01–H06 |

## F4-C 未关闭项

| 项目 | 当前事实 | 需要的证据 |
|---|---|---|
| C1 Compute→Host | 静态审查风险，未声称实机已触发 | 最小真实 compute SSBO write→host readback、sync validation/可见性 |
| C2 imported alias | 不同 native handle 可能实际重叠；未完成针对性反例 | 真实 memory/offset/size 重叠拒绝、合法独立 allocation |
| C3 import replacement | A→B 时旧 ImageView/backing 寿命需验证 | A completion 后替换 B、销毁 A、旧 plan 保留的 GPU/fault 路径 |
| Binding 性能 | p50 +5.0420168067%，CI [+3.3058%, +7.6271%]；p95 +4.7619%，门禁未解除 | 锁定输入/二进制的 A/A、B/B、成对 A/B 与归因，不能抽幸运样本 |
| Native 覆盖 | 3D/cube/relative extent/vertex-stream NOT_IMPLEMENTED | 对照原 F4 合同确定缺口，不能改称硬件 fallback 或擅自推至 F8 |

## 设计批准以后仍需冻结的实现细节

现有 parser 可接受的 DataSchema grammar；可选 GPU representation 的准确 public shape；CPU storage 对非平凡 T 的具体策略；RecordHandle owner 编码/generation wrap；容量、dirty 合并/Scatter 阈值；完整 multi-owner LayoutPlan 下层修改与性能影响。均在真实消费者阶段提交最小实现与测试，不在 D0 增加 Type Registry、Clock、Manager 或 native owner。

F5 使用现有可表达输入验证计划共享；结构性绑定合同不允许实例 ID，但 F6 的完整 DataSchema/multi-owner 实现不提前塞入 F5。F6 最小 DataSet 需真实 GPU consumer，F7 扩展该 owner 而非临时兼容存储。F8 才以真实 Skinning 补齐 F5 scope 共享业务证据。

## 资格与停止

I 只含 rd2 新文档。独立 no-hardlinks clean clone 检出 I，机械与语义审查分开记录；发现缺陷先出新 I，再验收。V 只新增 RD2_D0_VERIFICATION.md。无生产变化，不跑 build/CTest/ASan/GPU/SDK/性能、不同步安装头；它们为 NOT_RUN。

远端或用户保护基线改变、需要改旧 FINAL/生产代码、或者存在未决架构冲突时停止相关推进，不 reset/stash/强推。正常推送后 STOP；F4=PARTIAL，F4-C/F5=NOT_AUTHORIZED，V2_PRODUCT=EXPECTED_UNAVAILABLE。
