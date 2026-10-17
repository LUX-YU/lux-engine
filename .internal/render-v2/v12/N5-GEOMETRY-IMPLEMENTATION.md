# N5.5 Vertex / Index / Indirect — implementation checkpoint

Parent `84f7ef5ea5fb90dd589f9dcfc5ec771415d3728c`. N5 remains IN_PROGRESS; no independent geometry PASS yet.
Graph Draw.hpp provides concrete value contracts for Vertex layout, input resource binding, direct vertices,
indexed and indexed-indirect. Existing PassProgram consumes them; no new owner/identity/registry/queue.
CompiledGraph's existing pipeline identity now combines complete Shader and canonicalized GraphicsState;
resource names, offsets and draw counts are excluded from Pipeline identity. Compiled recipes hold only
resolved resource indices, native input facts and draw arguments. Runtime record binds prepared Buffers and
executes the corresponding Vk command. Publication continues to update the existing resource binding/LastUse.

SPIRV-Cross checks exact vertex scalar/vector formats and cross-stage varyings. Device Vertex limits and
actual format support, source roles/ranges/alignment, direct draw/index ranges and indirect record ranges
are checked cold. Indexed values/indirect record contents are trusted GPU or validated asset data; no CPU
readback is inserted to inspect generated commands. Current Device does not enable multiDrawIndirect:
count>1 explicitly returns Unsupported. One indirect command can draw multiple instances; no claim of
full GPU Culling. Depth is not yet exposed/supported by this checkpoint.

Real GPU Compute produces three Vertex vec4s, reordered UINT32 indices and a Vk-compatible indirect record.
Direct, indexed and indexed-indirect each render RGBA64 through actual vertex input and Vertex→Fragment
varying. Five executions per variant retain first-party new=0. Nine missing identity/format/offset/stride/
index count/alignment/direct bounds/indirect range/duplicate attribute negatives reject. Existing Graph,
Native compiler, BindingPlan, Storage binding, parameters, lglsl and Compute assertions retained.

Windows Release all -j4 -- -k0 and second no-work; targeted15/15,6.47s. GPU Validation/SyncValidation0 errors,
actual EOS duplicate warning retained. Initial Compile.cpp if-initializer variable collision corrected;
failed full-build diagnostics preserved. No tests ran against that failed build. New Draw.hpp independent
C++20/no-RTTI probe and relocated Graph consumer passed. Full matrix and independent N5 V still required.
Raw evidence E:/RenderV2Rebuild/V12/evidence/n55-geometry; generated source/compiler/SPIR-V hashes recorded.
Legacy719,user modifications/history protected. Performance PARTIAL, product historical BLOCKED unchanged.
Next: Depth/attachment formats, then N5 combined qualification before N6 production storages and3D sample.
