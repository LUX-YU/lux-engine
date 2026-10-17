# N5 typed arguments and live lowering — implementation checkpoint

Parent: N4 independent V `752ad5dcf379d8165c72291222af23833e405195`.
This is a buildable N5.1/N5.2 internal checkpoint, not N5 completion or a performance qualification.

## Production changes

- ConfigValue keeps its existing immutable shared owner, retained-byte accounting and module pin. Its static
  metadata adds a cold semantic equality thunk only for a noexcept equality-comparable value. Full TypeToken,
  size and alignment must agree first. The same owner is equal without invoking code; distinct non-comparable
  values conservatively conflict. No padding comparison, serialization, new Any or TypeRegistry. Size remains48B.
- StorageRequirement carries the actual typed configuration into CompiledGraph and the actual StorageSet.
  PROVIDE/REQUIRE comparison includes that value; absent and supplied configurations differ.
- StorageArgument owns ConfigValue. StorageDescriptor names its argument C++ type instead of a DataSchema.
  Attach resolves argument keys once, rejects unknown keys/conflicting values, checks the full type before
  calling the professional thunk, and never encodes or decodes the C++ argument. The immutable Effect owns
  arguments; professional prepared bindings copy the required identity/values into their existing leases.
- Removed TextureBinding and external BufferBinding DataSchema/Codec specializations. Persistent Data codecs
  remain. Direct Buffer Readback now passes its existing deferred request to the real Native consumer.
- Native lowers only resources reached by the one G4 Analysis execution order/outputs. Dead History, absent
  factories and dead shader layouts cannot reject an otherwise valid live graph. Resource index positions
  remain stable; no second Graph or Analysis. Exporting the same resource makes its checks relevant again.
- Program ownership/duplicate validation remains cold. Missing Program callbacks only fail for live Passes.
  Zero-Pass contributors retain real declared Texture arguments. Unknown argument keys and conflicting
  values fail before publishing a candidate; prior Effects remain unchanged.

## Verification performed in implementation tree

Full `all -j4 -- -k0`, second no-work; 106/106 tests in122.43s before the final standalone Attach-key
negative cases. The final all/no-work plus five affected real-GPU tests pass after that addition.
Earlier targeted runs:9/9 Config/claim/binding/lifetime tests and4/4 compiler/Program/record-plan tests.
All test names and effective historical assertions remain. Programs' obsolete ignored `test.argument`
fixture is replaced by the actual Texture argument, with explicit unknown-key rejection and a dead invalid
shader case. Texture/Buffer sampled pixels64/32, retained backing and original routing/shutdown regressions pass.
R5 same Shader1Pipeline/1Bind/1Scope/2Draw and different Shader2Pipeline/2Bind/1Scope/2Draw remain.
No GPU Validation/SyncValidation errors; actual overlay warnings remain. Cross-DLL Config equality executes
while the foreign code pin is live, then destruction precedes module release. ConfigValue48B remains.

First build failures exposed old encoded-Binding tests and transitive Encoding includes; each is preserved
in external evidence. No historical report/checker was changed. Full final-I independent ASan still required.

## Remaining N5 scope

This checkpoint does NOT finish the unique LayoutPlan, existing lglsl/Pass contract integration, controlled
SPIR-V relocation, real member/stride/entry/stage ABI, multiple parameter placements, Vertex/Index/Depth/
Indirect/Compute, complete owner-safe-point rebinding or explicit optional Readback. Fixed ShaderBinding,
float4/push16 and mandatory readback still exist and are not qualified as the target architecture.
N5–N7 remain in progress/not started as applicable. Performance PARTIAL and product Root BLOCKED remain.

Evidence: `E:/RenderV2Rebuild/V12/evidence/n51-typed-bindings` and `n52-live-lowering` contain all build
attempts, commands, exits, samples and protection checks. The independent checkpoint report will index them.
