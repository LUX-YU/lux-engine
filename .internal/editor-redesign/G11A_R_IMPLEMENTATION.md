# G11A-R construction record

Baseline: `d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959`.
This is an internal construction record, not final I/V qualification.
R0 decisions and protection paths are in `G11A_R_ARCHITECTURE_FREEZE.md`.

## R1 wiring checkpoint

Added one neutral `render_resource` component. Existing Storage identity/ABI/Scope declarations moved
there; Feature, Native and Runtime consume that one definition. Descriptor validation compares complete
identity, policy and DataSchema. Native factory metadata carries the selected API, matching professional
Ops, configuration compatibility callback, actual creation callback and code pin. No physical instance is
created during registration.

Cooked Program moved to Feature and TransferOptions to Resource, without forwarding aliases. Graph Range
moved to a real PRIVATE include tree. Runtime public-header probes no longer add the native source include
directory. Public Runtime still has the old explicit native operation methods pending R3/R4 migration;
this checkpoint does not claim final public API convergence.

Texture upload/region/release now enter the original Operation SlotMap through `registerStorage<TextureStorage>()`.
Deleted the zero-pass Texture Feature and its registration header. `registerFeature<T>()` retains the same
existing dynamic registration implementation. A shared cold handler validator checks Schema/completion
contracts and duplicate names before either registration mutates the operation table.

Device capability negotiation is now driven by registered Storage required/optional capabilities, not Host
upload callback presence. Device selection records the optional capabilities actually supported by the
selected physical device; Manifest publication follows that negotiation. The old eager DeviceSession Texture
owner remains solely as the explicitly bounded R1→R2 wiring checkpoint allowed by the contract. Registry,
domain and lazy physical ownership are NOT_IMPLEMENTED in this checkpoint, and must replace that owner in R2.
Factory creation is the same fallible native Texture create function used by the current path; the isolated
second-API fixture only checks non-GPU factory/config/typed Ops semantics.

### Validation and actual failure retained

Commands, run in the isolated source using the external `E:/RenderV2Rebuild/G11AR/run.ps1`:

```text
run.ps1 -Action configure
run.ps1 -Action build             # all -j 4 -- -k 0
run.ps1 -Action build             # ninja: no work to do
run.ps1 -Action test -Filter .    # all 74/74, 83.55 seconds, Release
```

All original 72 names remain; new tests are `render.resource.contract` and
`render.runtime.storage_registration`. Existing installed/relocated consumers and independent public-header
compilation passed in that all build/test. This is implementation-environment Release evidence only;
Debug, ASan and final independent clone verification remain outstanding.

Initial failures were preserved, not reclassified:

* A generated header probe named `Storage.cpp` shadowed the new executable source in CMake. The probe now
  uses `headers/`, and the executable source is explicit. No test was disabled.
* The installed Feature dependency assertion still listed the old three components. It now explicitly
  includes the actual new `render_resource` dependency, retaining the unrelated-target rejection.
* The unchanged G10 lifecycle fixture intermittently awaited a submit which had already returned an
  explicit Native queue-capacity failure. Diagnostics reproduced `outcome=FAILED` and
  `error=7356206049137219893`, exactly `vulkan::kCapacity`, before any submit for sample 1021. The analogous
  stop sample 1022 used the same invalid assumption. Evidence: `r1-stop-reproduction.log` and
  `r1-stop-reproduction-2.log` in the external evidence directory. The final fixture consumes and records
  only explicit pre-submit capacity refusals, retries within the unchanged 15-second deadline, then performs
  all original abandon/stop/source-owner assertions after observing a real accepted Submit. No queue size,
  production GPU algorithm, observation hold, timeout or ownership assertion was weakened.
  Twelve consecutive same-queue GPU-ready runs then passed (83.97 seconds total), followed by full 74/74.

Protection checker passed: 719 frozen blobs, both actual user hashes/status/HEAD, dependency versions and
permitted change boundaries. Historical reports/checkers and IPC source remain unchanged.
Raw logs and snapshots are under `E:/RenderV2Rebuild/G11AR/evidence/`; final immutable archive/index is due
after final I. No final I/V, architecture PASS or push is claimed here. G11B–G13 remain NOT_STARTED.

## R2 checkpoint — actual unique instance ownership

DeviceSession now owns one ResourceRegistry. Its former `textures_`, `scoped_`, ScopedBuffer and
ScopeIdentity owners are removed. BufferInitializer and the FeatureRegistration buffer list are removed:
the former zero-buffer consumer installs a real Native factory through the same Storage registration.
Storage instance identity is Domain SlotKey + complete Type identity + semantic key. Domains use SlotMap
generation and a shared epoch token (not a second generation or a GPU owner); old/cross-session domains
are rejected before lookup. Root/Scene/View/Attachment parent chains are bounded to the four legal scopes.

Typed access returns the matching immutable Ops and instance borrow, with ancestor results read-only.
Config reuse is checked by the selected factory; new instances publish only after successful creation.
Domain close removes admission and generation first, while prepared Context/Upload references retain
instances until their actual GPU use finishes. Instance deleters run outside SlotMap mutation, allowing
professional indirect references to release another instance without re-entering container erase.
Factory code pins outlive their instance deleters. Scene and View store Domain values; the four-field
ExecutionGroup is unchanged. Context owns its Attachment domain and resolved bindings. Failed attach
rolls back only newly created instances and preserves earlier shared instances and last-good bindings.

Texture registration no longer creates GPU Storage in run(): first Root upload creates it lazily. Config
is supplied by `registerStorage<T>(T::Config)`, not Texture-specific Server capacity fields. The built-in
Native factory is selected at DeviceSession construction. Per-frame Context use caches its native binding;
Registry maintenance traverses prepared callbacks without Type/name lookup. Transient allocations remain
Context-owned and outside persistent Storage. Cold Texture helper APIs and the specialized upload work
remain explicit R3/R4 migration items, not claimed final APIs.

Verification in implementation Release: all build, second `ninja: no work to do`, full CTest **76/76**
(72.85 seconds). Log `evidence/test-RelWithDebInfo-20261015-162600-399.log`. Original 72 names remain.
New tests `render.resource.registry` and `render.native.resource_domains` prove scope/config/epoch
rejection, read-only inherited access, indirect-reference exact destruction, six actual Native allocation
failure points with empty rollback, last-good reuse, and domain closure while completion observation is
held after real GPU Submit. The existing texture-storage test additionally closes Root after accepted
upload work and verifies physical backing survives until completion and is destroyed on last work release.
The existing two-Scene/four-View, texture shader/color, queue, owner, installed and hot-allocation tests pass.
Public Registry/Native StorageBinding headers compile independently. GPU validation in affected tests is
zero error; Debug/ASan and independent final I/V are still due. This checkpoint is not final G11A-R PASS.

## R3 checkpoint — one Native transfer and Deferred work path

TextureUpload now owns only Texture metadata, reservation/publication and a Transfer candidate. Transfer
owns the bounded staging-page, host-prepare, chunk, queue submission, barrier/QFOT and accepted-ticket
state for both Buffer and Image. No TextureId appears in this common state machine. Native GpuWork
provides one erased advance/cancel/publication owner in the existing Deferred slot; Runtime has no
TextureUpload/texture_ready/commitUpload branch. The original RenderRequest remains the unique source
Owner. Single-ticket G10 work uses the same GpuWork completion branch. Missing or pre-existing tickets
cannot authorize a pending work's success. Buffer upload is a real new consumer, not a simulated copy.

Retirement entries now admit up to three physical-queue conditions and require every matching queue's
frontier; unrelated equal serial numbers do not prove completion. Session polls each physical queue once
before collecting retirement and professional backing records. Texture backing collection no longer
restarts a dense scan after each erase. Binding remains 32 bytes; RetirementEntry is now 72 bytes for
three queue conditions (an explicit bounded memory cost). 10,000 allocation/binding metadata records:
1,343,640 bytes and 61 new calls; ExecutionContext is 272 bytes at this checkpoint.

Full Release all build and second no-work succeeded; serial CTest **77/77**, 127.54 seconds, log
`evidence/test-RelWithDebInfo-20261015-165148-016.log`. The new `render.native.buffer_transfer` uploads and
reads back all 1 MiB bytes in 16 chunks on actual 0:0 alias, 0:2 same-family and 1:0 dedicated queues,
preserves the first ticket when the second submit fails, and rejects missing/old completion proofs.
A separate two-queue copy proves the same source stays alive until BOTH independent equal-valued ticket
serials are observed complete. Existing Texture 1/16/64 MiB, sampling/color, partial failure, shutdown,
GPU-ready, installed and IPC tests remain present and pass. Source protection: 719 blobs and both user
files unchanged. Debug/ASan, final external Storage/Graph consumer and independent V remain due.

Failures and measurement limits retained:
- An earlier build and test overlapped; that run is explicitly excluded from qualification. Subsequent
  build/test invocations were serial. All its original logs remain in the evidence directory.
- The first serial full run failed texture_progress in RetirementQueue destruction during injected
  Transfer DeviceLost. LLDB proved the destructor treated a different queue's poll error as failure of
  an otherwise empty/non-lost Graphics retirement queue. It now drains actual recorded queue uses and
  collects their individual completed/lost frontiers. The original loss/shutdown assertion is unchanged.
  Stack: `evidence/r3-texture-progress-lldb-stack.log`; corrected targeted and full runs both pass.
- GPU-ready wall times vary (about 6 seconds in one serial run, 21–31 seconds in another). This is NOT
  claimed as a performance improvement, nor attributed solely to the excluded overlap. Per-sample CSVs
  are retained in `evidence/r3-checkpoint`; the final equal-workload performance analysis is still due.
- The professional wrapper plus common Transfer introduces cold allocations. No end-to-end zero-
  allocation upload claim is made. The existing stable rendering allocation tests still pass.

This is an intermediate production checkpoint, not final G11A-R I/V or architectural PASS. R4 must still
remove Texture-specific compiler/binding/public API dependencies, followed by the R5 installed OCP test.

## R4 checkpoint — contract-driven compilation and prepared binding

ShaderArtifact now owns neutral ShaderInterface metadata instead of an ABI number. The existing
SPIRV-Tools/SPIRV-Cross path verifies set/binding/kind/array extent and the current explicit float4 block
layout. Equal-sized integer blocks are rejected. The supported StorageBuffer primitive is deliberately
one float4 at offset zero, not a claim of general Material/Shader authoring. CompiledGraph owns layouts,
Pipeline code and resource requirements, with no TextureId or asset value. Concrete field-encoded
StorageArguments live in Effect and enter Attach; generic PreparedStorageBinding is resolved once via
Factory ops. Executor uses direct prepared descriptors, parameter pointers and last-ticket pointers.
No Type/Name lookup, Feature call or per-frame Storage dispatch was introduced.

StorageRequirement can explicitly name its domain-local instance key independently of its graph port.
Texture's prepared future-use subscriptions belong to TextureStorage and refresh only on publication;
old physical backing remains protected by accepted tickets. The formerly public TextureVersion is a
private TextureStorage::Backing. DeviceSession no longer exposes textures/configureTextures/uploadTexture
or refreshTexture; the professional TextureUpload factory owns Texture validation and delegates transfers.
Ordinary OperationContext exposes the neutral ResourceRegistry; Native Device/queue/reservation entry
points reside in the explicit runtime/vulkan/GpuCompletion.hpp boundary. Device queue/family duplicates
were removed. Descriptor layout creation uses gapi::vk::DescriptorSetLayout::tryCreate, preserving VkResult
and fault hooks, with no second Native RAII owner. GAPI is linked privately and exported through actual SDK metadata.

The new external fixture under native/vulkan/test/extension uses only public APIs. A real Shader samples
Texture and reads the uploaded StorageBuffer together. Reusing the SAME CompiledGraph with old and new
prepared Buffer bindings produces exact 64/32 pixels; business removal prevents new binding while both
existing references remain valid. Old Buffer consumers intentionally keep their prior backing until detach.
32 stable iterations have Engine new=0. Descriptor kind/set/count/size and same-size wrong scalar layout
are negative tests. G4 gains only STORAGE_READ's real usage validation; its Analysis/hazard algorithm is unchanged.

Implementation Release: full CTest **78/78**, 152.13 seconds, log
`evidence/test-RelWithDebInfo-20261015-172643-841.log`; subsequent stricter same-size block reflection check:
all build, second no-work, affected compiler/installed/binding-plan/sampling/storage-bindings **5/5**,
`evidence/test-RelWithDebInfo-20261015-173045-887.log`. Final independent three-configuration V remains due.
Protection checker passed: 719 blobs, dependencies, original user hashes/status, 95 changed files versus base.

Retained failures: a fixture argument used `texture` instead of G5's canonical `shared:texture`; corrected
at the fixture source and negative Attach identity/LOD assertions preserved. Installed Texture consumer
also needed explicit Attach arguments. The new fixture initially destroyed Device before its retained
CompiledGraph; corrected fixture shutdown, Validation again zero errors. LLDB inadvertently generated an
empty CSV in the primary worktree because its command omitted an explicit cwd. Its timestamp/zero length
were verified and it was moved to external evidence (`r4-debug-empty-texture-sampling.csv`); no protected
file changed and full original status/hash checks passed afterwards. This procedural failure is not omitted.

R5 still must prove typed Server registration, CRUD/Readback Operations, installed external consumer and
final independent I/V. This checkpoint is not final G11A-R PASS.

## R5 final implementation — external extension and qualification boundary

Repository LUX-YU/lux-engine; delivery branch codex/render-v2-rebuild. Original and freshly fetched remote
HEAD remain d5b4ff0ad792dd8e2d9ada7c26bda3c86fc33959. Construction source is the isolated
E:/RenderV2Rebuild/G11AR/source worktree. Dependency and original dirty-tree hashes remain those in R0.
The final I is the commit containing this section; its exact SHA is recorded by the subsequent independent
V, avoiding a self-referential commit hash. R0–R4 checkpoint commits retain their intermediate limits.

The external extension is implemented under native/vulkan/test/extension and runtime/test/BufferExtension.*.
It uses real public SDK APIs, registerStorage<TestBufferStorage>(), registerFeature<TestBufferFeature>(),
the original Operation SlotMap, generational BufferId, original Deferred completion and common Native
Transfer. Create/update/remove/readback operate on real VkBuffers; Shader sampling multiplies Root Texture
by Buffer values through the normal Feature→Graph→Compiler→Attach→execute path. Expected pixels are 64,
32 and 128. Removal rejects old IDs while existing prepared consumers pin their old valid backing; reuse
changes generation. A returned CPU Readback owner survives Server/Client destruction. Two distinct Features
share the Root Texture across two Scenes/four Views. Stable rendering Engine new=0 is asserted.

The installed runtime consumer compiles this external extension against a fresh, relocated SDK. Its only
source-tree includes are TEST fixture sources, not private or public production include directories.
The independent neutral Buffer description header compiles without Vulkan. R5 changes only tests and
current construction evidence. check_render_g11ar.py --extension mechanically rejects any production
change after R4 (6ad87a920bc1194caca5d7626e6d59c3e5a36766), including new untracked files.

A material-like indirect consumer acquires an actual Texture backing plus Registry use, removes its
business ID and closes the Root domain, then performs real GPU readback. The instance survives exactly
until the indirect reference is released. This is a lifetime proof, not an implementation of MaterialStorage.

### Final responsibility / state accounting

* DeviceSession: one Device/VMA allocation ledger, physical queue owners, Retirement and ResourceRegistry.
  Deleted two specialized owners (textures_ and scoped_) and duplicate Graphics queue/family fields.
* Registry: domain generations/parent scopes and native instance ownership. One descriptor/factory record
  per type; one instance per domain/type/key/config. Factory/compatibility/name checks occur in cold calls.
* Context: Attachment domain and prepared StorageUse records retain actual physical consumers. Per-frame
  code directly reads prepared descriptors, parameters and last-ticket destinations. Graph transient
  allocations stay in Context; no second Registry/Graph/Storage owner.
* Transfer: one Buffer/Image staging/chunk/queue/ticket machine. TextureUpload is professional candidate
  publication around it. GpuWork erases this work into the existing Deferred slot; the original request
  is the sole retained input owner. No new worker, IPC queue, Operation table or pending-request collection.
* Effect: immutable code plus separate concrete StorageArguments. Shader identity/layout remains static,
  asset values do not multiply compiled graphs. TextureUse and external Buffer Use pin physical backing;
  only professional code decides whether future bindings migrate on update.

G11A_R_CHANGE_INVENTORY.json lists all 54 production/build files and per-file named class/struct definition
changes (33 additions, 10 removals; moves count on both sides; enums and forwards excluded). These are
definition locations, not 23 new runtime owners. Program/PassProgram/StorageRequirement/Range and
GpuCompletionReservation move; new descriptor/value records carry actual consumers. The substantial new
owners are the single Registry (replacing two specialized tables) and common Transfer (extracted from
TextureUpload). Every public contract has a compiled consumer. No production change is made in IPC,
Data, Protocol, Core, Legacy or historical reports/checkers.

Changes by purpose: Resource adds neutral description/factory/domain contracts; Feature owns neutral
Program and Texture binding data; Graph moves its private Range include and validates STORAGE_READ;
Native resolves generic bindings/transfer/retirement/reflection; Runtime installs typed registrations,
domains and generic Deferred work; gapi adds only recoverable descriptor-layout creation. No per-frame
Schema/type/name checks or fallible wrappers for ordinary computations were added.

### Measured costs and explicit limitations

Implementation Release measurements: Binding=32 bytes (unchanged); Context=232 (G11A 224); RetirementEntry=72
(G11A 32, now three independent physical-queue conditions); PreparedStorageBinding=64, StorageUse=80,
ResourceRegistry=240, ResourceDomain=24, StorageFactory=72. Existing 10k allocation/binding ledger test remains
1,343,640 bytes/61 new calls. 1000 Registry Scene domains + instances, one-byte dummy native owners included:
1111 cold new calls, 424,673 cumulative requested bytes. This is CPU metadata cost, not GPU residency or
peak live memory. The allocation fixture now additionally counts bytes; no end-to-end allocation-free upload
claim is made. Four-View steady rendering remains checked for zero Engine new; driver allocations are separate.

All original capacities/backing charges remain: no increase to Reply, Queue, staging or resident budgets to
obtain a PASS. Existing 1/16/64 MiB, Mip/BC/Region, source backing budget, partial-submit, DeviceLost injection,
two compile workers, Main 64 Reply/1.5s backpressure and client stop/abandon tests remain in the full suite.
Physical queue evidence on RTX 4070 Laptop: alias 0:0, same-family 0:2, dedicated transfer 1:0. Common Buffer
upload exercises all three; Texture exercises alias/dedicated and prior G10 covers same-family real submits.

Controlled external SleepProbe (same MSVC /O2, 1000 samples each, no timer-resolution changes) measured actual
sleep_for(100us): p50/p95/p99/max 15.5442/16.0940/16.2559/16.7001 ms; sleep_for(1ms):
15.5319/16.0970/16.2836/16.7268 ms. Slow GPU-ready observation/consumption segments have the same ~15ms scale.
This demonstrates a scheduling contribution, not proof that every tail has the same cause. No production
timing/queue optimization was made. Full per-sample raw CSV, GPU timestamp scope and equal-workload baseline
comparison are retained in the final V archive; no claim of speedup from code structure is made.

### Retained R5 failures and fixes

The first external ReadBuffer registration used RESULT_AVAILABLE with gpu_completion=true; registration
correctly rejected that inconsistent descriptor. The fixture uses RESOURCE_GPU_READY for its actual GPU copy.
No production registration guard was weakened.

An implementation all-test run passed 79/79 (96.07s). While recapturing a log overwritten by CTest --show-only,
gpu_ready's late-reply sample 1020 failed. Diagnostic reproduction on the third repeated run proved
outcome=FAILED, error=7356206049137219893 (vulkan::kCapacity), submit=0, fence=0. This is the same legitimate
pre-submit backpressure already handled by the 1000 measurement samples and 1021/1022 scenarios. The late
reply fixture now consumes ONLY this explicit refusal and retries within the unchanged 10-second admission
bound. Unknown errors still fail; all successful GPU byte/owner/late destruction assertions remain. No Native
capacity, GPU observation hold, production lifetime or timeout was relaxed. Original failures are retained
as r5-late-reply-failure.log, r5-late-reply-reproduction.log and r5-late-reply-diagnostic.log.

Root TOOLCHAIN was actually reconfigured in build/root-r5 and remains BLOCKED by missing render/client,
feature_client.cmake and render/standard_content, with their dependent export errors. No placeholders.
Existing warnings are preserved (including duplicate EOS Overlay layer warnings); Validation errors must
be zero. IPC R2 historical PARTIAL is unchanged. Linux/Android builds and TSan are NOT_RUN. General shader
block layouts, Material/Instance/Mesh products, G11B–G13 are OUT_OF_SCOPE/NOT_STARTED.

The corrected final implementation gate passed: full all -j4 -- -k0, second no-work, all 79 tests (97.88s)
and protection check. Full successful LastTest output and 23 raw CSVs are in evidence/r5-implementation;
the original 72 names are all present and the seven added names are explicitly listed in test-names.json.
Final three-configuration clean-clone V and immutable evidence archive are separate mandatory gates; this
implementation record alone does not declare final architectural PASS.
