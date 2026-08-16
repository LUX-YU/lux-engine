# EVSM 阴影现状说明（为何默认使用 PCF）

> 日期：2026-06-05
> 适用版本：`render-refactor` 分支
> 关联(原 `.internal` 文档已不在此树内,链接失效,仅存档名备查)：`shadow-lighting-audit-2026-06-05.md`（完整审计 + RenderDoc 数据）、`evsm-shadow-implementation-guide.md`（实现计划）

## TL;DR

引擎现阶段**默认使用 PCF** 阴影。EVSM 已实现并可用（`--evsm` 开关 / `default_technique = EShadowTechnique::EVSM`），但因**性能不达标**暂不作为默认。本文说明 EVSM 当前的问题与后续改进方向。

| 维度 | PCF（默认） | EVSM（可选） |
|------|-------------|--------------|
| 帧率（4070 Ti，400 cube + 6 点光 + 1 方向光） | **~1400 FPS**（RelWithDebInfo） | **~45–50 FPS** |
| 显存（阴影图集） | 0.8 GB（D32 单图） | 3.2 GB（RGBA16F × 2 图） |
| 自阴影 bias | 需调参（已修，见下） | bias-free（算法优势） |
| 软阴影质量 | 硬件 PCF，较硬 | 预滤波，较软、更平滑 |

## 1. 核心问题：blur 是带宽瓶颈，占 86% 帧时间

EVSM 的"bias-free"来自**预滤波**——每帧必须对整张阴影图集做分离高斯模糊。RenderDoc 抓帧（4070 Ti，RelWithDebInfo，26.48 ms/帧 ≈ 37.7 FPS）：

| Pass | 耗时 | 占比 |
|------|------|------|
| GBuffer（400 cube） | 0.17 ms | 0.6% |
| EVSM 矩 caster（真正"画阴影"） | 1.70 ms | 6.5% |
| **EVSM blur（H+V，逐 slice）** | **22.81 ms** | **86%** |
| 延迟光照 | 1.22 ms | 4.6% |
| skybox / grid | 0.35 ms | 1.3% |

**关键不对称**：把阴影几何光栅化进图集只要 1.7 ms，而对每张 tile 的每个纹素做模糊要 22.8 ms（**13×**）。开销不在"画阴影"，而在"模糊每一个纹素"。

**为什么这么贵**：blur 每帧触碰约 2.18 亿纹素（4 cascade × 4096² + 36 cube face × 2048²），×2 方向 ×(5-tap 读 + 写) ≈ **每帧 ~21 GB 的 RGBA16F 流量**。4070 Ti 显存带宽 ~504 GB/s，这是**显存带宽瓶颈**，不是算力瓶颈，也不是 Debug/validation 的问题（Release + 关 validation 实测仍 < 50 FPS）。

对比 PCF：**完全没有 blur pass**（采样时用硬件比较滤波 `sampler2DArrayShadow`），这就是 PCF ~1400 FPS vs EVSM ~45 FPS 的全部来源。

## 2. 这是 EVSM 的固有特性吗？

**部分是固有的**：预滤波 blur 是 EVSM 的"入场费",无法删除——删了就不再是 EVSM。

**部分是当前实现放大的（约 4×）**：
1. **全分辨率模糊**：在 4096²（方向光）/ 2048²（点光）上做模糊。但 EVSM 的指数 warp + bilinear **天生容忍低分辨率 blurred 图**——这正是它的设计初衷，当前实现没利用。
2. **36 个 cube face 主导**：6 点光 × 6 面，每面都要 caster + 双向 blur，占 blur 时间的 ~60%。
3. **每帧全量重做**：即使灯/相机没动，也重新 blur 所有 tile。

## 3. 已知可改进方向（按性价比）

详见审计文档 §8 的完整取舍表。摘要：

| 方案 | 预估 blur 收益 | 取舍 | 状态 |
|------|----------------|------|------|
| **A. 降分辨率模糊**（矩全分辨率渲染 → 下采样到更小图集再 blur + 采样） | 4096→1024 ≈ **16× 削减**，blur 22.8→~3 ms ⇒ **~140 FPS** | 软阴影近无损（EVSM 专为此设计） | **推荐，未实现** |
| B. 点光 6 面 → 2 面（dual-paraboloid） | 点光 blur 14→~5 ms | 抛物面投影畸变，需足够细分 | 未实现 |
| C. 静态光缓存 blurred tile | 取决于动态光比例 | 真实场景收益大，纯动态场景收益小 | 未实现 |
| D. single-ESM（RGBA16F → RG16F） | 带宽减半，blur ~12 ms | **牺牲 dual 负矩的抗漏光** | 不建议 |

**结论**：让 EVSM 真正可用的根治办法是**方案 A（降分辨率模糊）**,单独就能把帧时间从 26 ms 砍到 ~7 ms（~140 FPS），且 EVSM 视觉几乎无损。这是后续 EVSM 优化的首选项。

## 4. 已修复的相关问题（历史）

EVSM 上线过程中修复的一系列渲染错误（详见审计文档）：
1. **同步竞争**：`DeferredLighting` 未声明对 `EVSMBlurV` 的图依赖 → 读写竞争导致闪烁/碎裂阴影。已加 `.read()` + `.after()`。
2. **调试硬编码残留**：`shadow_evsm.glsl` 里 `bleed_reduction` 被硬编码为 0.9（10× 放大器）→ 亮面噪点。已改回读 config UBO。
3. **透视光源深度精度**：点光/聚光用双曲 NDC 深度直接 warp → fp16 精度坍缩 → 远处噪点闪烁。已改为**线性深度** warp。
4. **显存优化**：EVSM 图集从 3 张减到 2 张（分离模糊 ping-pong 回 moment 图）→ 省 1.6 GB。

PCF 侧也在本轮修复了点光源 acne（重新引入 normal-offset bias）与 cube-face 接缝白点，现已可作为高质量默认。

## 5. 如何切换 / 测试 EVSM

- **引擎默认**：PCF。`ShadowMapCommConfig::default_technique` 与 `DeferredLightingCommConfig::technique` 默认即 `EShadowTechnique::PCF`。
- **压力测试**：`deferred_stress_test.exe`（默认 PCF）；加 `--evsm` 切到 EVSM 做回归对比。
- **运行期切换**：`ShadowMapFeature::setActiveTechnique(EShadowTechnique)` 已暴露（需同时切 lighting feature 的 `technique` 以绑定匹配的 SPIR-V 变体）。

## 6. 架构备注（后续清理）

`IShadowTechnique` 的 `recordShadowPasses / recordFrameSetup` 虚函数 hook **目前未被调用**(`recordPostFrame` 已经在用——`MeshShadowFeature::addPasses` 里 `tech->recordPostFrame(fctx)`,`EVSMShadowTechnique::recordPostFrame` 已实装)。真实的 caster + blur pass 调度仍是 `MeshShadowFeature::addPasses` 里散落的 `if (use_evsm)` 分支。这两个 hook 是**有意保留**的未来脚手架:实现方案 A 时把 EVSM 的 caster + blur 调度真正搬进 `EVSMShadowTechnique`,让"新增阴影技术零修改既有代码"落地——**不是**可删死代码。
