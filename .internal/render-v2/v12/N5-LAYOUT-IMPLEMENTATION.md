# N5 Native layout and complete resource rebinding — internal checkpoint

Parent `ec2c34fb197d0c023acb1012efb0628dabc46e94` (independent stage/entry V).
Scope remains N5 IN_PROGRESS, not a completed N5/N6 gate. No push.

StorageDescriptor no longer owns Shader set/binding or descriptor shape. Its typed factory/config/argument
identity and capabilities remain. PassProgram maps actual cooked variable names to G4 resource keys.
The existing Effect-owned CompiledGraph owns one private LayoutPlan, whose program entries determine
Shader relocation, complete shared/private Vk layouts and Context descriptor writes. No second Graph,
Registry, execution queue or lifetime system is added. Public ProgramResourceBinding is a cold name pair;
DescriptorUse/ProgramLayout/LayoutPlan are private compiled value records with distinct layout responsibilities.

The existing device sampled-image table has one complete frozen Native shape and a single weak lookup.
TextureStorage contributes actual view/sampler values through its existing slots; no per-Scene descriptor
megatable is created. Private Buffer fields are packed into one set instead of assigning a set per resource.
Controlled relocation gathers original decoration positions before any write, rejects unsupported/ambiguous
forms, validates the original SPIR-V, and reconciles the final artifact with SPIRV-Cross at pipeline creation.
Source hashes are in N5-LAYOUT-sources.json plus the previously frozen algorithm sources; Legacy is not linked.
Existing lglsl Pass tooling integration remains for the next N5 step, not claimed by this checkpoint.

ExecutionContext preallocates two private descriptor banks before publication. execute permits at most one
uncompleted submission per Context, so Owner replacement can write the other bank. Repeated updates reuse
that safe bank. Context retirement was already reserved; no release-time allocation or extra retire map.
Prepared subscriptions notify only at Owner publication. Buffer handle, barrier handle, descriptor selection,
parameter borrow and LastUse update together; withdrawal disconnects while Storage lives. External Buffer
Storage no longer owns per-asset descriptor layouts/pools. Actual physical backing still follows real Tickets.

Tests preserve old valid behavior and explicitly migrate these superseded contracts:
- Storage-level physical layout mismatch checks moved to actual Native shader/Graph layout checks; invalid kind,
  layout size, name, stage, duplicate/mismatched position, count and missing resource remain rejected cold.
- Buffer Attach snapshot expectation is replaced by authorized Owner-point rebinding. All four existing Runtime
  Contexts converge to new pixels without reattach; old in-flight Readback is allowed only a complete old frame.
  This does not promise a fresh-frame transaction for Readback. Recreate/old generational identity, detach,
  exact late owner release and no hot allocation assertions remain.

Real GPU tests include provisional sets2/3/6 and bindings5/7/11, two packed fields, same and distinct Buffer
resources, reversed mapping order, exact RGBA bytes32/16, replacement pixels64->32, repeated updates with
completion observation held, descriptor-pool/set creation failure rollback, LastUse and pipeline/scope counts.
The old three-use demo cap is removed; four-use graphics now executes. Device limits remain checked cold.

Implementation Windows Release all -j4 -- -k0 and second no-work. Full107/107 in123.17s after fixing the old
snapshot tests; final distinct-resource/failure cases and cap removal subsequently rebuilt and targeted tested.
Earlier full run105/107 in123.68s retained as a failed attempt (same obsolete assertion in direct/installed
external Buffer consumers), not hidden or reclassified. Independent exact-I ASan is required next.
Validation/SyncValidation errors0 in implementation runs; actual EOS overlay duplicate warning retained.
Observed metadata: Binding32, Context296, PreparedBinding80, StorageUse104, RetirementEntry104 bytes;
10k host ledger+Binding cost1,343,640 bytes/61 allocations. These are metadata, not GPU memory savings.
First-party stable-frame new=0 and owner release new=0 retained in tested fixtures. Performance stays PARTIAL.

Protection719 Legacy blobs, three user files, dependencies and historical evidence pass. No IPC algorithm,
upper Engine/Editor implementation or historical report changes. N5 still needs general parameter/Shader ABI,
lglsl/Cook tooling, Vertex/Index/Depth/Indirect/Compute and final combined qualification. N6/N7 remain pending.
Raw implementation logs and failures: E:/RenderV2Rebuild/V12/evidence/n53-layout.
