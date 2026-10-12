# LuxEngine Render V2 — RD3 Effect / Graph / Domain Architecture

这是根据 `RenderV2_RD2_Architecture_and_Implementation(2).zip` **完全重写**的新设计包，而非 RD2 的增量补丁。日期：2026-10-12。**状态：Candidate / NOT_AUTHORIZED**，不是已经提交到 Git 的实现或验证。它保留所有 Legacy 与高级渲染功能需求，但不保留旧 Data-First/Scene-owned Graph 的过时类型/所有权模型。

## 建议阅读顺序

先 `00_架构总纲.md`，再看 `02_模块分层与所有权.md`、`06_RenderEffect与Feature组合.md`、`07_RenderGraph推导与编译.md`、`09_NativeCompiler与Executor.md`、`10_SceneViewTarget与ViewGroup.md`；Engine Client/ResourceDomain 看 04/05，代码实例看 15，阶段管理看 18/`work_orders/`，完整来源映射看 20 和 appendix。

## 核心类型

- `RenderRuntime` = Client；`RenderBackend` = neutral Server；`backend/vulkan/Backend` = native device owner。
- `Scene` 真正拥有 `View`；`View` 选择稳定 `RenderEffectId`；`Target` 独立归 Backend。
- `RenderEffect` 拥有 Feature 算法配置，可跨 Scene 和 View 共享/同时编辑；`RenderGraphId` 属 Backend 缓存中的不可变 native 编译产物。
- `RenderGraphBuilder` → neutral `RenderGraphIR` → backend `Graph` → `Executor` + per-execution `GraphExecutionContext`；一次 execute 完成当前 Target 所声明的最终画面。
- `ResourceDomain` + 唯一 `ResourceRegistry` 决定 GPU 资产逻辑归属和父链可见性；Engine ECS/资产系统仍是业务 CPU authority；GPU lifetime 按真实 Queue/WSI completion。

## 内容

- 21 章完整 Markdown；新型来源/一致性审计；一个新合订版；7 张**未授权**阶段工作单；4 份完全按 RD3 责任重绘的 trace CSV；一个 SHA256 Manifest。
- Requirement IDs、Golden IDs、Capability IDs 和旧 Frozen Legacy 证据全部保持可追踪。
- 任何“旧类名”只应出现在 `20_原设计迁移对照` 或 `appendix/CapabilityMatrix.csv` 的 Legacy 来源字段；新设计目录/所有权不以旧类命名为限。

## 阶段现状（2026-10-12 Git 快照）

R0–F6.b 已有不同范围的已验证机制。F6.b 总体仍 PARTIAL（Record/Submit p95 不确定 + 冷态 Layout 回退）；历史 F4/F5 性能债仍在；F6.c/F6.d/F7 以及业务/高级渲染尚未获得自动授权。用户审查设计并明确批准后，先执行 docs-only D0，再单阶段 I/独立 V/STOP。
