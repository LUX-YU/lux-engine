# N5 general parameters — implementation checkpoint

Parent `c37f70b690334a20626e9dd78fc79feb444ff6fc`. N5 IN_PROGRESS, no push.

ShaderInterface replaces push_bytes=16 with a full owned ShaderBlock plus actual stage visibility.
One existing LayoutPlan determines the device-limited shared pipeline push range, original fields and
source mappings. ProgramParameterBinding identifies Storage or Context source fields by full names only
at cold compilation/Attach. ParameterUse and the Context's ParameterCopy are compiled value records,
not registries or Owners. The actual Vk range can be empty; no implicit push block remains in the validator.

CompiledGraph owns immutable initial Pass bytes, field metadata and Context input ABI. ExecutionContext
owns its independent input and Pass byte blocks. PreparedStorageBinding borrows professional parameter
bytes/layout under its existing unique reference; Texture Use owns index/color-space/lod values. Native
prepare validates exact field shape, offsets/ranges, source presence and duplicate destinations once.
Storage publication updates the existing complete binding and copies the prepared fields. Owner input
updates touch prepared copies; execute directly pushes actual bytes without names/Schema/Feature dispatch.
No second Any/Graph/Storage/queue or permanent compatibility alias is introduced.

Native setColor/color_ is removed. Explicit setContextParameters and setParameters accept bounded Native
ABI bytes, not object-representation Wire. They fail before changing current data on bad ID/length. Storage
and Context-owned fields cannot be overwritten by Pass-local updates. Runtime's old View.color data remains
an explicitly unfinished N6 Camera migration: its existing consumers now supply exact bytes and preserve
resize/last-good. This checkpoint does not claim the actual Camera or 3D model completed.

Real GPU test uses64B across Vertex+Fragment: Texture control, external Buffer gain, Context tint, independent
Pass bias. Two Contexts produce16/32; per-Pass updates produce32 without changing the other Context. Buffer is
read by both stages; its Graph barrier stage mask derives from compiled visibility rather than Fragment only.
A separate zero-push graph actually renders64. Bad field/key/duplicate/stage/layout/length/type cases reject;
old output survives failures. Twenty update/record/submit iterations retain first-party new=0. Native Texture
sampling remains Fragment-only until a real broader consumer is implemented; Transfer algorithms unchanged.

Windows RelWithDebInfo all -j4 -- -k0, second no-work, full CTest -j1:110/110,121.39s.
All previous109 test names preserved. Validation/SyncValidation errors0, actual EOS duplicate warning retained.
Earlier targeted3/6 and full109/110 failures preserved: missing explicit parameter metadata/default input and
one renamed resource mapping in fixtures. Original pixel/Pipeline/Bind/Scope assertions remain; only implicit
red defaults became explicit caller values. Successful corrected targeted3/3 plus final full run retained.
Exact-I independent ASan is next; current Release is implementation evidence, not independent V.

Observed Context320B(previous296), PreparedBinding80, StorageUse104, Binding32, RetirementEntry104;
10k host Allocation/Binding ledger1,343,640B/61 allocations unchanged. Per-Pass parameter/copy vectors are
cold prepared mutable execution data, not a claimed memory/performance improvement. Historical performance
PARTIAL stays. Legacy719,3 user files/history/dependencies/whitelist protected. No IPC algorithm changes.
Generated source/compiler/SPIRV hashes and failures: E:/RenderV2Rebuild/V12/evidence/n54-parameters.

Remaining N5: existing lglsl asset tool integration, Vertex/Index/Depth/Indirect/Compute and format recipes,
final combined gates. N6/N7 and final product closure remain unqualified; upper Engine/Editor NOT_RUN.
