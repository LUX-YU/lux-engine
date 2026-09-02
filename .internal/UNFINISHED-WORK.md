# Lux Engine Unfinished Work Index

This file is the single active index of Held work. It does not duplicate design details and grants no implementation
authority. Follow the linked ADR and satisfy its reopening evidence before creating production types.

| Work | Status | Decision source | Reopening evidence |
|---|---|---|---|
| CPU typed Asset lifecycle | Needs more evidence | `ADR-20260830_CPU_Typed_Asset_Lifecycle_Gate.md` | A real cross-Scene CPU Asset workload with measured ownership, cancellation, generation and memory behavior. |
| Asset residency / Product streaming | Needs more evidence | `ADR-20260830_Asset_Residency_Barrier_A_Review.md` | Real AssetId load, duplicate interest, failure/retry, GPU ready/release and generation replacement in two independent domains. |
| Spatial runtime index adoption / StreamingSystem | Needs more evidence | `ADR-20260830_Spatial_Runtime_Index_Adoption_Gate.md` | Cooked index wire, safe-point adoption and a real streaming workload. |
| Jolt far-origin dense broadphase | Held | `lux-engine-l0-l3-baseline-v3.1/ARCHITECTURE-GAPS.md` | Fixed-density broadphase sweep across increasing absolute coordinates and concrete private-region evidence. |
| Plugin hot reload | Held | `ADR-20260901_L3_Scene_System_Meta_Convergence.md` | A product requirement for mutation after startup; startup metadata remains immutable until then. |
| MaterialEditor durable persistence | Held | `lux-engine-architecture-implementation-docset-2026-09-02/07-implementation-roadmap-and-gates.md` | Approved MaterialGraph source codec and stable document identity. |
| FlowForgeEditor durable persistence / packaging | Held | `lux-engine-architecture-implementation-docset-2026-09-02/07-implementation-roadmap-and-gates.md` | Approved FlowGraph codec and stable ScriptSymbol source identity. |
| Project-specific product target generation | Held | `lux-engine-architecture-implementation-docset-2026-09-02/09-product-runtime-vfs-and-async-script.md` | Approved project manifest, target inputs, module selection and packaging specification. |
| GPU CI | Deferred | `l4-toolchain-convergence.md` | A release gate requiring repeatable physical-GPU qualification across maintained runners. |

Current L1-L3 and L4 closure work may update status and links here, but must not place implementation designs in this
index.
