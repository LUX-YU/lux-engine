# N5.4 explicit Shader stages and entries — internal checkpoint

Parent: color-only independent V `53e13fb16c51a789fcb6a8816795e0df06efc328`.

SpirvArtifact now owns a vector of actual SpirvStage values: the existing rdesc::EShaderType, an owned
entry name, and owned binary words. The old vertex/fragment fields are removed; every current producer
and negative fixture is migrated. No forwarding alias, parser, registry, native handle or code owner is
added. Complete artifact equality still compares all metadata and words, never a hash alone.

The current Vulkan graphics compiler requires exactly one Vertex and one Fragment by declared kind,
independent of list order. It validates the actual binary model and entry using the existing SPIRV-Tools
and SPIRV-Cross boundary, then supplies that entry to VkPipelineShaderStageCreateInfo. Empty, embedded-NUL,
wrong-name, duplicate/missing/unsupported stage inputs fail cold. The entry is no longer fixed to main.
Compute/Depth-only recipes are not implemented by this checkpoint; their stage metadata is not a PASS.

Data reuses the existing header-only shader_metadata build contract and installs its exact ShaderInfo
header plus generated visibility header. It does not link the resource/asset implementation, Engine
toolchain, Vulkan or stdexec. Fresh relocated Data and pure Client consumers pass. The existing Data
dependency assertion is extended only for this real build-interface header dependency.

`render.native.compiler` renames only the known checked-in fixture entry literals, declares draw/frag,
reverses stage order, creates the real pipeline, submits and checks RGBA red pixels. Six negative cases
preserve the original validation/pipeline tests. The production code contains no manual instruction walker.
All native/Runtime/extension fixtures retain their actual binary and behavioral checks.

Implementation Windows Release: all -j4 -- -k0, second no-work, targeted8/8 in10.01s, then full107/107
in122.09s. Final equivalent predicate grouping and active README correction were followed by all/no-work
and compiler1/1 in0.48s. Validation/SyncValidation errors zero. This checkpoint has no new independent
Debug/ASan claim; final N5/N7 qualification remains required. Evidence: `E:/RenderV2Rebuild/V12/evidence/n54-stages`.

N5.4 remains IN_PROGRESS: fixed push16/member/descriptor contracts, parameter placements, output formats,
Vertex/Index/Depth/Indirect/Compute are not certified. N5.3 unique layout and full resource rebinding also
remain. Performance is PARTIAL, product closure historical BLOCKED. No push or speedup claim.
