# N5.6 color-only execution — internal implementation

Parent: N5 cold verification `6f2a776e3f0d11d85ca8b1ca95a86be65c23c6a6`.

DeviceSession::prepare no longer requires a target-sized readback Buffer. An explicit exported Buffer
still uses the existing Graph Copy, host barrier and GPU Ticket path. A graph containing only a color
output allocates its image and records its declared Clear/Draw; there is no inferred copy or host buffer.
ExecutionContext::readbackBytes returns zero when that output is absent. Native ReadbackOperation and
the Runtime readback admission reject it before creating a completion reservation or submitting work.
The synchronous readback boundary similarly rejects an absent/invalid output before polling completion.
There is no new owner, graph, queue, hidden temporary copy or readback manager.

New real GPU test `render.native.color_only` covers clear and graphics, repeated execution, compatible
resize, zero Buffer creation and one declared color barrier per frame. It checks synchronous error
completion without a GPU submit, and in-flight detach retaining the image until real Ticket completion.
Four image allocations are destroyed exactly once. Existing explicit readback/pixel and R5 plan tests
are unchanged. Targeted Release tests: execution, binding_plan, color_only, runtime.programs and
runtime.gpu_ready all pass (5/5,10.16s), Validation/SyncValidation errors zero. Full all build and second
no-work pass. The first build failed on an unqualified Native error constant in Runtime; fixed in the
implementation tree, original failure log retained.

Evidence directory: `E:/RenderV2Rebuild/V12/evidence/n56-color-only`.
Independent ASan for this exact checkpoint is still required. This is only N5.6's removal of mandatory
readback, not full N5.6: complete descriptor/barrier/parameter rebinding remains. The unique LayoutPlan,
expanded shader ABI and draw/compute primitives remain pending. No performance qualification changes.
