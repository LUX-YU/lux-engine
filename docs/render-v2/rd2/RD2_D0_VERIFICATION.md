# RD2-D0 独立文档资格验收

日期：2026-10-11。结论：**RD2-D0_DOCUMENT_QUALIFICATION = PASS**，仅限候选设计与文档完整性。RD2 尚待用户批准，不取代已批准 FINAL；F4 仍为 PARTIAL。

## 身份与实施范围

| 项目 | 精确身份 |
|---|---|
| 批准基线 | `b9f9a909bc34ac87f00c594996201a20f0af00c8` |
| 首次 Implementation | `264e285d6948fadf24feee56efc828eaa8bbb5e6` |
| 最终 Implementation I | `f5cd177dc3a3c254befd590e26301fc0b2630332` |
| Frozen Legacy | `a669409a289a6fa4092f21176397795b1cdb7f3e` |
| 候选版本 | RD2-D0-C2 |
| Verification V | 承载本报告的单独 Git 提交；SHA 以提交结果和交付收据为准，不自引用 |

I 相对基线恰好新增 44 个文件，全部在 `docs/render-v2/rd2/**`。最终 I 包含一次独立审查后的文档修订，故基线至最终交付为两个 Implementation 提交加一个 Verification 提交。V 只能新增本报告；候选 MANIFEST 不回写。

## 独立验收过程与证据

独立检出：`D:/LuxQualification/render-v2-rd2-d0-f5cd177dc3a3/source`。使用 `git clone --no-hardlinks --no-checkout`，设置 clone 的 `core.autocrlf=false` 后 detached checkout 精确 I；确认无 alternates、无共享硬链接、工作树干净。施工检出和独立 clone 的 `ValidateTrackedSnapshot.cmake` 均 EXIT 0。

本报告由同一实施助手在独立 clean clone 上另行执行机械检查和语义复核形成；不宣称另一位人员或独立代理复现。全部原始命令、退出码、检查脚本、逐项结果、输入原件、保护快照和哈希保存在：

`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-D0/f5cd177dc3a3c254befd590e26301fc0b2630332/`

- 外部 `EVIDENCE.sha256` 覆盖 60 个证据文件，SHA-256：`7a627d9cbf677b4b7dfa781bec79ab7121dae72ac51308a5df8f1e4c0ae4b4d2`。
- 候选 `MANIFEST.sha256` SHA-256：`6626a2070e6c7db409257673742361189d6968cb0df30adace19676458aed98d`，覆盖 43 个 I 文档载荷，不含自身及本 V 报告。
- 机械原始结果：`mechanical/qualification-results.json`、`qualification-checks.json`、`qualification-commands.json`。
- 语义原始结果：`semantic-review.json`；逐章哈希、结论和所有权/缓存/生命周期反例分别列明。
- 原包、裁决和 CSV 身份见 [来源清单](RD2_PROVENANCE.md)；原 ZIP 未覆盖、未重打包。

首轮 I 的 4,492 项机械检查通过，但语义复核发现 §4.3 流程图顺序以及 §7.1、§8.2 两处旧术语仍有冲突，登记 **PARTIAL**。已回到施工检出修正文档，生成新 I 后完整重验；未在验证 clone 修补正文。首轮证据及其 manifest 保存在 `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/RD2-D0/264e285d6948fadf24feee56efc828eaa8bbb5e6/`，引用也包含于最终证据。

## 机械资格

| 检查 | 结果 |
|---|---|
| 总机械断言 | **4,493/4,493 PASS**；断言数不等于运行测试数 |
| 来源与覆盖 | 原 RD2 包 34 文件、33 manifest 条目匹配；单独 D0/F4-C/F5 与包内原件一致 |
| 文档完整性 | 00–20 共 21 章、五份工作单、用户裁决、合订版、围栏、本地链接及锚点通过 |
| 合订版 | 与修订后 00–20 的机械串接逐字一致 |
| 原始矩阵 | 4 CSV 与 FINAL、RD2 原件字节一致；86 能力、25 工作负载、51 要求、131 类型，主键/引用完整 |
| 对账材料 | 162 行要求映射、131 行类型映射，原行哈希一致；无新功能 PASS |
| 决策及阶段 | D01–D32、N01–N17、C0–C13、DF01–DF15 完整 |
| 历史来源 | 102 处源码/操作引用对冻结 tree；其中 3 处活动 toolchain 引用按历史 SourceSHA 核对，不把后续已演进代码冒充旧字节 |
| Legacy | **719/719** blob、mode、size 与冻结提交一致 |
| 只读保护 | 基线全部 **3080** 个 tracked entry 的 mode/blob/size 不变，涵盖 Core、Transport、Graph、Vulkan、CMake、FINAL 和全部历史报告 |
| 用户保护 | 两个用户检出的 HEAD、分支、status、staged/unstaged diff、未跟踪清单及 **7/7** 修改文件字节哈希不变 |
| 隐藏输入 | 无隐藏源码补丁；独立 clone 干净；既有 tracked snapshot 规则通过 |

## 语义复核

完整复读候选 00–20、五工作单及审查材料；对照原 FINAL、用户裁决和基线源码锚点。关键词检查仅用于辅助，不替代下述设计推演。

1. 稳定 `RenderDataTypeId` 保持既有 canonical-name identity；版本和布局另验。DataSet 为 Scope 实例身份，RecordHandle 受其 owner 约束。
2. CPU Schema、CPU 生命周期、Wire 编码和可选 GPU 表示分开。非平凡 CPU-only 类型合法；原 `RenderDataDescriptor` 不重新解释为 GPU fingerprint。
3. 冷态 Composition 先解析需求/权限/Scope，再冻结 StorageInterfaceShape 和结构槽，再贡献 Graph。原 C0–C13 保持原职责；C10 不得晚改 Shader ABI。
4. DataSet 逻辑 authority、Backend RAII backing、Feature borrow 和 CPU asset ledger 不重复拥有。Dirty revision 不证明 Ready/Completion；erase/resize/detach/卸载不能提前释放在途资源。
5. BindingSlotKey 与 DataSetId 分开；两 Scene 相容结构可共享计划，main/preview 必须明确实例绑定。真实 Graph use/version 决定 GPU hazards，需求声明不凭空产生 Barrier。
6. D16 为 REPLACE，D23 保留 CLARIFY 分类并明确 View 有效期/Backend native ownership；D10、D14、D15、D26 的继承扩展逐项列明。
7. 三类数据和四个作者场景具有正负约束及文件/类型/重复声明统计模型；均为目标设计，不是编译结果、已交付 SDK 或性能测量。
8. F6 多 owner 验证归 F6；F5 接缝不提前实现完整存储。F11 关闭 F/I，H01–H06 留各 H 阶段。唯一执行域、独立渲染进度、Device Lost 终态保持。

精确条款、反例与阶段责任见 [决策迁移](RD2_DECISION_MIGRATION.md)、[一致性审阅](RD2_CONSISTENCY_AUDIT.md)、[逐项追踪](RD2_TRACEABILITY.md)。候选正文自身已统一，无须靠勘误页覆盖相互冲突的正文。发现的首轮冲突已经关闭；**本次未留下阻断 D0 的设计矛盾**。

## 未运行与未关闭义务

生产 build、CTest、ASan、GPU、性能、SDK/install、Android/Linux、示例编译：**NOT_RUN — 文档阶段无生产变更**。未修改公共头，不执行安装头同步；历史资格仅引用，未冒充本轮重跑。

RD2 权威仍需用户裁决。DataSchema 具体 grammar、GPU representation ABI、非平凡 storage 策略、handle/generation 编码、容量及 dirty 阈值留真实实现阶段验证，不是本轮已冻结的生产接口。

F4 的 compute→host、imported memory overlap、import A→B 生命周期风险尚待复现；原 binding 性能门禁和 3D/cube/relative extent/vertex-stream 缺口仍未关闭。D0 不证实或证伪这些 native 风险，不重写历史成绩。详见 [阶段准入](RD2_IMPLEMENTATION_GATE.md)。

## 完整 Implementation 文件清单

以下路径相对 `docs/render-v2/rd2/`，全部为基线后新增；没有生产或历史文件变更。

- `00_README_权威总纲.md`
- `01_设计哲学与成熟引擎对照.md`
- `02_模块分层与所有权.md`
- `03_数据描述与类型身份.md`
- `04_类型化存储与GPU布局.md`
- `05_通信与发布事务.md`
- `06_Feature组合与数据依赖.md`
- `07_RenderGraph推导与编译.md`
- `08_Shader与布局自动生成.md`
- `09_VulkanNative与执行.md`
- `10_Runtime多Scene多ViewTarget.md`
- `11_插件SDK与扩展边界.md`
- `12_Legacy功能完整迁移.md`
- `13_先进渲染兼容性.md`
- `14_Cpp20类型和编码纪律.md`
- `15_端到端API示例.md`
- `16_性能模型与可比实验.md`
- `17_完整验收与正确性矩阵.md`
- `18_阶段计划与交付门禁.md`
- `19_冻结设计决策RD2.md`
- `20_原设计迁移对照.md`
- `AUDIT_来源与一致性.md`
- `FINAL_RD2_全章合订版.md`
- `MANIFEST.sha256`
- `RD2_CONSISTENCY_AUDIT.md`
- `RD2_D0_ERRATA.md`
- `RD2_DECISION_MIGRATION.md`
- `RD2_IMPLEMENTATION_GATE.md`
- `RD2_PROVENANCE.md`
- `RD2_REQUIREMENT_MAP.csv`
- `RD2_SOURCE_ANCHORS.json`
- `RD2_TRACEABILITY.md`
- `RD2_TYPE_RESPONSIBILITY_MAP.csv`
- `README.md`
- `appendix/GoldenWorkloads.csv`
- `appendix/LegacyCapabilityMatrix.csv`
- `appendix/RequirementsTrace.csv`
- `appendix/TypeOwnership.csv`
- `work_orders/00_D0_用户歧义裁决_只读.md`
- `work_orders/01_D0_文档架构冻结工作单.md`
- `work_orders/02_F4_CLOSEOUT_实施工作单_待授权.md`
- `work_orders/03_F5_RUNTIME_入场工作单_未授权.md`
- `work_orders/04_F6_DataSchema_FeatureSDK_预备范围.md`
- `work_orders/05_F7_TypedStorage_GPUResident_预备范围.md`

V 另新增本 `RD2_D0_VERIFICATION.md`，不属于 I 冻结载荷。推送前再次核对远端基线，只允许普通 fast-forward；提交/远端 SHA 与最后用户保护检查保存于外部交付收据。

```text
RD2-D0_DOCUMENT_QUALIFICATION = PASS
RD2_AUTHORITY = CANDIDATE_PENDING_USER_APPROVAL
F4 = PARTIAL
F4-C = NOT_AUTHORIZED
F5 = NOT_AUTHORIZED
V2_PRODUCT = EXPECTED_UNAVAILABLE
STOP
```

下一阶段须先由用户批准 RD2 并单独授权 F4-C；F4 PASS 后，F5 仍需另行放行。
