# RD2 原来源、机器校验与身份说明

候选修订 RD2-D0-C2。下列原包审计是来源声明，不是本轮独立 Verification；本轮裁决、来源与检查分别见 RD2_D0_ERRATA、RD2_PROVENANCE 和后续独立报告。

- 设计源文件：`LuxEngine_RenderV2_FINAL_Architecture_2026-10-10(4).zip`；SHA-256：`39be1dd08ca4e30964ef47fa0b274054bc426e79472a1d0e733deaa0fe83d551`。
- Git 审查基线：`b9f9a909bc34ac87f00c594996201a20f0af00c8`，审查时 F4 PARTIAL，F5 不授权；不是本包重新运行的验证。
- 新章节 00–20：**21 个**；合订版生成时合并所有章节；Work orders：5 张（第 1 张建议当前执行，其余未获授权）。
- 原矩阵：**86** capability = F01–F36（36）+ I01–I44（44）+ H01–H06（6）；**25** Golden W、**51** Requirements REQ；TypeOwnership 旧表完整复制。
- 原 ZIP 中旧 00–19、只读合订版与历史 R2 文档的原 SHA/内容**并未被新 00–20 覆盖**，用户原附件是可回溯证据。本新包按版本语义形成另一份候选完整设计。
- 本次在模型工作容器中做了：ZIP 提取/原附件存在验证、CSV 字节级核对、文档结构/代码围栏/编号校验和交叉身份审查；**没有**实际修改/构建 GitHub 代码、没有重新运行 MSVC/ASan/Vulkan、没有证明新 API 已可编译。正式 doc-only I/V 仍需实施方在 Git clean clone 执行。

## 冻结 CSV 原字节 SHA256

- `LegacyCapabilityMatrix.csv`: `ce3555c7eaf50a981a53ec22a9ae41f5bfbe35e626970a891623c8c4448451b6`
- `RequirementsTrace.csv`: `9f04107bcf5d06a77c42637614f8a315abf98c211aad101bca3340bdcbb5b9a0`
- `GoldenWorkloads.csv`: `bc9fd980b4d6455bc981a2f458ba53e427da1fceccb7e44b63ffc5a9c0f31b42`
- `TypeOwnership.csv`: `9a57c133d5bb7e1e4da6c57a34483be6364bfa6226195fcfe129892ed47952d4`

## 设计特别审计

1. `DataSchema`（语义类型及按用途可选的 Wire/GPU 表示）与 `PassSchema`（Pass 访问）不是重复的第二数据定义；共享现有 meta generator。
2. GPU 编译不存在先有 Native backing 才能建 Graph 的循环：冷态 Composition 先解析 DataSet/Feature 并冻结逻辑 StorageInterfaceShape，Feature 写 Graph → 原 C0–C8 → 原 C9–C13（C10 物理 backing；C11 native recipes；C12 queue/sync）。
3. `BindingSlotKey` 是冷态**结构性**身份，`DataSetId` 是运行时实例，不将不同 Scene/View 强制分成不同 PipelineLayout。
4. Data dependency / Graph resource hazard / Native readiness / last-use 是四种不同的关系与完成事实。
5. DataSet 由 Scope 拥有，Native backing 由 Backend 的 RAII owner 拥有；Feature 默认既不拥有共享 DataSet 也不拥有第二 VkAllocator.
6. Legacy 719/86/25/51、R0–R4/F0–F4 历史验证不修改；F4 PARTIAL / F5 NOT_AUTHORIZED。
7. GPU Native shader/layout/graph 允许扩展真正新 primitive，不允许数据类型本身迫使修改 Renderer 的业务类型 switch。

## 注意

此页是本机**静态包审计**，不是独立验证 V，不代表 RD2 在 GitHub 已批准。另请使用 `work_orders/01` 完成准确的 Git tree/文档来源/frozen legacy 核对；D0 不重新运行生产 CMake File API 或构建。
