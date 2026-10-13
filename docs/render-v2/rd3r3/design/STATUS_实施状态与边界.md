# Status — 已实施 vs 候选目标（2026-10-13）

## 已验证的 Git 历史入口

- Repo: `https://github.com/LUX-YU/lux-engine`, branch `codex/render-v2`。
- 实际上次查询 HEAD: **`960e755395be2c9f78a86339a062c507826c4eb4`**，P2-A Verification V（独立报告 `docs/render-v2/rd3r2/P2A_VERIFICATION.md`）；Implementation I **`483d20ed900b8c226a740704f3c888498b1dae11`**，源代码 `runtime/DeviceSession.*`, `ResourceDomain.hpp`, `Backend.cpp` 等修改。
- P1 Implementation I `db9cec6d1268a4b2847c1723a702b5bad8505ccf`、V `98002bb03bb8ae0c6ed156fcd7e847082b6a7148`，实机 two Scene/four Views/one shared native Graph、FIF 2/3，提交报告为有限 PASS。
- P2-A report: Windows x64 43/43 scoped tests、Native SyncValidation 0 errors、P2A preserve Legacy 719/719。报告可由仓库复核；**这不是本包再次实测的成绩**。
- 历史 RD2 `F4/F5/F6.b` 的性能 PARTIAL/未验证项目原样保留；原字节历史报告不得删除或改判。

## 当前真实 P2-A 代码仍存在的旧形态

- `DeviceSession` 直接拥有 `InstanceContext`（最新 R3-D13 要改为 Native bootstrap owning）；内部 `DeviceContext/ResourceContext` 已存在。
- DeviceSession 仍有 `ResourceDomainId` 与 `SlotMap<unique_ptr<ResourceDomain>>` 树；`Scene/View` 保存 DomainId；还没有编译 ResourcePlan + attach 使用权 RAII 的完整新模型。
- Backend 唯一维护 EffectId→Binding triples 这条关系，但派生 `ExecutionJob` 仍在 `runtime/Objects.hpp`；必须审查并收敛，不能额外再造一个管理层。
- Effect 仍由固定 Vulkan `EffectSource` 构图，并非完整 Device-neutral Feature/Graph Author；Native 共享 CompiledGraph 实测，完整用户 Edit/Atomic Publish 还未实现。
- Engine ECS/plugin neutral DataProjection、mixed Effect final Target/WSI/history、Legacy 特化 GPU Storage 和 UE H1–H6 均未完成。

## 本压缩包范围

当前仅创建设计/CSV/工作单/一致性工具，不会修改 GitHub 生产源码，不会让 R3-A 自动获授权。建议下一张工作单 **`work_orders/01_R3A_AttachmentRAII_Scope_Context.md`**；正式开工前用户批准，实际提交 SHA 可能已推进需要重查。

## 可核查链接

- P2-A I: https://github.com/LUX-YU/lux-engine/commit/483d20ed900b8c226a740704f3c888498b1dae11
- P2-A V: https://github.com/LUX-YU/lux-engine/commit/960e755395be2c9f78a86339a062c507826c4eb4
- P1 V: https://github.com/LUX-YU/lux-engine/commit/98002bb03bb8ae0c6ed156fcd7e847082b6a7148
- Legacy freeze: https://github.com/LUX-YU/lux-engine/commit/a669409a289a6fa4092f21176397795b1cdb7f3e


## 本次源码专项发现：仍未落地的能力

- 当前已有 R2 `UPLOAD` Lane + byte-budget，但 P2-A Backend 的 `pollUpload` 尚未 active 处理 Mesh/Texture；Legacy `GpuTransferPipeline` Dedicated/Record-only/typed task 路线不能当当前实现。
- 当前存在独立 MaterialGraph / ShaderIR / Toolchain/Cooker，而新的 RenderEffect/ResourcePlan/RenderRuntime 尚未贯通它与 Global MaterialStorage/TextureStorage；当前 Shader backend 主要 Vulkan SPIR-V 且有显式 unsupported pass。不能把现有模块的存在当成产品支持。
- 当前 R4 Graph 有真实 Native 编译/执行基础，但第 29 章 Legacy 特化的 Compiler 优化和 Tile/Resize/Transient pool 不可一概标为移植完成；逐项在新清单注明 `PARTLY_R4_NATIVE` 或 `NOT_QUALIFIED_IN_P2A`（并不等同于源码缺失），以实际新提交 I/V 验证为准。
