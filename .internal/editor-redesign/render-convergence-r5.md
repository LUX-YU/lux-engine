# Architecture convergence R5 implementation

Parent: `5f29c622f3b02efbe790a7db3341cc1f1e53a9fe` (R4).

ShaderArtifact and PassProgram move to native/vulkan/Program.hpp, namespace lux::render::vulkan. All actual
consumers migrate together; Feature's installed header and probe are removed, with no compatibility alias.
The replacement public header is compiled independently, including a Server probe without Vulkan SDK includes.
FeatureDefinition remains neutral. No Shader parser, author graph or global Program registry is added.

Native compilation retains the exact G4 execution order. Within one graph the fixed RGBA8 raster recipe and
push-color layout are common; complete stage words plus ABI identify each Pipeline. One vector owns unique
Pipelines, recipes borrow handles. No hash-only match or duplicate physical Pipeline destruction is possible.
Binding decisions are computed once in execution order. Every command buffer begins with its necessary bind;
Begin/EndRendering and transfer commands do not invalidate the graphics pipeline binding. No dynamic conditions
or secondary command buffers are accepted by this subset. Future lowering must explicitly account for those.

Scope merging requires consecutive graphics recipes, identical full-range color attachment and LOAD on the next
recipe. Native support already excludes descriptors, depth, sampling, feedback and conditional execution. Under
these restrictions color attachment accesses remain ordered within dynamic rendering. Only the replaced internal
attachment barrier is removed; previous frame reuse, transfer and host visibility dependencies remain. CLEAR,
other attachment and intervening transfer preserve separate scopes. Conditional Native work remains Unsupported.
No Pass reordering, dependency recomputation, generic optimizer or extra per-frame lookup is introduced.

Legacy reference audit: RenderGraphCompiler.cpp computePipelineBindingPlan (1698 onward) and
RenderPassPlanner.cpp (topological grouping, non-graphics boundary around 320 onward). The cold linear scan and
scope boundary principles are retained. Old render-pass/subpass objects, full Kernel registry and blanket binding
reset at scope boundaries are not copied into the dynamic-rendering subset. Descriptor/Indirect plans require
real consumers and remain NOT_IMPLEMENTED; draw count is unchanged.

Actual GPU instrumentation (same production source, compile-time existing Native seam) counts calls AFTER each
vkCmdBindPipeline/BeginRendering/EndRendering/PipelineBarrier2/Draw executes. Production expands to direct calls.

| Case | Same/different Shader | Pipeline creates / binds | Begin / End | Barriers | Draws |
| --- | --- | --- | --- | --- | --- |
| Adjacent same attachment LOAD | same | 1 / 1 | 1 / 1 | 3 | 2 |
| Adjacent same attachment LOAD | different | 2 / 2 | 1 / 1 | 3 | 2 |
| Second CLEAR | same / different | 1 / 1 or 2 / 2 | 2 / 2 | 4 | 2 |
| Different attachment | same / different | 1 / 1 or 2 / 2 | 2 / 2 | 4 | 2 |
| Intervening transfer read | same / different | 1 / 1 or 2 / 2 | 2 / 2 | 5 | 2 |

Each row executes on GPU and verifies center and clear-area pixels. Different Shader swizzles red to blue;
LOAD retains the original background while CLEAR uses the second background. All Pipeline owners return to zero
per case. Conditional candidate rejection also rolls back its already-created Pipeline. Swizzle source and
SPIR-V hashes are stored in its generated header; regenerate with test/shaders/generate.py --swizzle (glslc and
spirv-val, Vulkan1.3). Raw counts: build/release/render/native/vulkan/test/native-binding-plan.csv.

Implementation verification: Release/ASan all -j 4 -- -k 0, full CTest -j 1 -R "^(render|platform)\." 61/61 each.
Final comment/header-generator normalization is followed by rebuild/no-work and binding-plan rerun. No Vulkan,
SyncValidation or ASan errors. Existing pixel, failure, retirement, scoped lifetime and async Runtime tests remain.
New checker preserves every file outside the exact whitelist, Legacy719, historical checkers/reports, IPC and
user modifications. Three SDK prefixes back up the removed Program.hpp before installing the committed new path.

R5 gates pass; final independent Debug/Release/ASan matrix belongs to R6. IPC historical PARTIAL unchanged;
full product closure separately BLOCKED. Descriptor/Indirect/Compute/RT NOT_IMPLEMENTED, GPU timestamps
NOT_MEASURED, Linux/Android/TSan NOT_RUN. No G10-G13 work.
