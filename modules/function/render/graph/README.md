# Logical RenderGraph — F2

C++20 `render_graph` is a static Core-dependent component. Its authoring frontend uses the existing generated PassSchema and a lightweight neutral Description header closure; the offline Shader toolchain is not a production dependency.

Use `RenderGraphBuilder` typed resources and graphics/compute/transfer/readback operations, then `finish() &&` and the free `compileLogicalGraph()`. The owning readonly `LogicalGraphPlan` contains versioned subresource hazards, deterministic schedule, culling, lifetimes, scope proof, imports/exports and JSON diagnostics. There is one compiler, with no Vulkan or Runtime execution.

[The F2 contracts and type inventory](../../../../docs/render-v2/F2_CONTRACTS.md) define exact producer selection, imported initialization, conditional alternatives, cache identity, synchronous FrameGraphBindings borrowing and the old test migration. The independently qualified result is recorded separately under `docs/render-v2/F2_*`.

Definition equality includes actual default values. Logical compatibility excludes scalar/sampler/clear/Camera/time/backing values because their current data comes from the owning per-invocation storage. Binding never compiles a graph and uses no first-party heap allocation in the tested warmed workload.

Logical intervals and Scene sharing eligibility are CPU proofs only, not GPU completion, alias safety or permission to share different View histories. F3-PRE-01 and F3-PRE-02 remain OPEN. Native RenderGraph is NOT_IMPLEMENTED; products remain EXPECTED_UNAVAILABLE until F11. F2 completion does not authorize F3.
