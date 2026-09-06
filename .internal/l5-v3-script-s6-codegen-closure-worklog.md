# S6 build-time codegen correction work log

## Status and identities

- IN PROGRESS, NOT QUALIFIED. No framework freeze, main merge or push is authorized.
- Bwork: `05cb8fde4e1e39bee03595df2559cb60c2b7e2f3`.
- Bsafe: `b03cd8c043773565a79a4de6a91d0a6b3fbe6fa7`.
- Engine branch: `codex/s6-deep-optimization`; main's five edits remain excluded and untouched.
- Dependency baseline: lux-cxx `7716c087e384d1bedc8a4a17a2d6ecb01ed30344`.
- Dependency worktree: `lux-cxx-script-codegen`, branch `codex/script-codegen-closure`.
- New order: P0 baseline, P1 actual generated C++ vertical, P2 migration/authoring/storage,
  P3 prepared Event/result owner, P4 finite Lua/graph closure and final qualification.

## Disposition

- KEEP: W0 ownership/pending/producer/command safety fixes; bounded direct class allocation;
  one TaskGraph, execution owner, HookChannel, stable resume, continuation and ingress contracts.
- REPLACE: CppStatic reflection projection, reflected/typed matcher, arity fallback and prepare-time
  fixed ownership layout calculations with Engine-authored templates and actual typed operations.
- DELETE: unintegrated SuspensionFrameLiveness.hpp. Existing AOT lowering is unchanged.
- NARROW: actual-class metadata and semantic population/budget inputs; no arbitrary page reclassification.
- REQUIRED: generated-source consumers, incremental dependency proof, prepared Event to final owner,
  Lua safe factory reuse, machine-code and memory accounting, final clean qualification.
- DEFERRED BY AUTHORIZATION: FlowForge liveness, arguments/frame merging, provider-bound plan caches,
  public batch APIs, new result compression, command trackers, PGO, VM native entry/allocator/pooling.
- GUI, R1 and new runtime managers are not in scope.

## P0 baseline

- 2026-09-05: Bwork all-j4-k0 build succeeds using MSVC Developer environment.
- Focused CTest: 26/26 PASS, including C++ prepared/coroutine, LuaJIT on/off, continuation,
  Event wait, lifecycle, Hook/Channel, Scene and benchmark smoke.
- First test invocation lacked the MSVC environment: 24/26; two negative compile tests failed
  because the standard `chrono` header was unavailable. That run is not a semantic failure or PASS.
  The complete 26-test selection was rerun in the proper environment and passed.
- Local logs: `build/RelWithDebInfo/deep-developer/p0-codegen-baseline-all.log` and
  `p0-codegen-baseline-tests-msvc.log`. These are development evidence, not final clean qualification.

## Remaining gates

P1-P4, full profile/install/relocation, codegen target/incremental proof and final paired performance
are NOT DONE. Historical checkpoints must not be represented as final qualification.

## P1 development checkpoint

- lux-cxx dependency implementation: `99a5d7e65b25aca1423a98f3d3c8552ca39f6724` (separate branch, not pushed).
  All build and 52/52 dependency CTest; multiline JSON and validation-first rendering are covered.
- Engine-authored `.template` files and `lux_cpp_static_scripts()` now generate constant contract/entry tables
  from real headers plus the existing symbol ledger. One header/target/environment shares one parse job.
- CppStatic no longer uses runtime Ref* projection, typed/reflected matching, reflection invoke or arity tables.
  Native language layout and const-reference ownership are constant-evaluated; objects use typed construction.
- Main C++ tests and benchmarks use generated entries. Generated tables carry no provider or endpoint address.
- A real installed consumer produces a ScriptArtifact, uses the Simulation graph's caller Hook, resumes
  an owned const-reference coroutine through NextStep, retires it and runs EndPlay. Exit 0; second build no-op.
  SDK: `install/codegen-p1-developer`; dependency SDK: `install/script-codegen-cxx`.
  Consumer source: `cmake/installed-consumers/cpp-generated-script`. No Engine source include directory supplied.
- Developer all build + full 209/209 CTest before adding the 13 generation-negative cases.
  The added 13/13 negative cases pass: nontrivial borrowed argument, pointer/mutable/rvalue references,
  non-noexcept method, async lifecycle, missing/duplicate symbol, private method, reason identity,
  borrowed return, missing coroutine context and variadic method.
- Resumable-only prepared entries are valid; lifecycle admission explicitly requires sync-only entries.
  This removes the old need for a dummy synchronous callable on a true coroutine export.
- Isolated dependency includes exposed old-install masking: three codecs used retired ByteIO headers;
  Native fixtures omitted the semantic target; UI test omitted generated headers. Separate commit d5038180
  migrates to current codec error propagation and explicit dependencies. Metadata asset regression passes.
- Installation must use a configured CMAKE_INSTALL_PREFIX: toolset component config destinations are absolute;
  `cmake --install --prefix` alone did not relocate all generated package files. The successful SDK uses
  the configured fresh prefix, not that mixed preliminary install.
- Local logs are `p1-*` in `build/RelWithDebInfo/deep-developer` and `script-codegen-cxx`.
  These are development gates, NOT final clean Bfinal qualification or a performance claim.
- P2 still must migrate remaining installed consumers, close authoring/type-schema inputs and simplify quotas.
  Full artifact/executable compatibility checks still run during instance creation; redundant cold checks and
  fixed local import mapping remain in the P2 information-accounting work. P3/P4 are not implemented.

## P2 partial checkpoint (2026-09-06)

- Engine code checkpoint: `eca61106e2acb228ed65408f54535d7c503ea71e`.
- Dependency checkpoint: lux-cxx `e2355ca4ce4e862f4e108f74422a30fce8afb736`.
- Native storage populations consume real executable state/frame layouts; public internal page plans are removed.
  Sync modules allocate no continuation frame arena. C++ frame request limits and byte budgets remain explicit;
  private frame-header overhead and generated owned-reference storage are provisioned by the backend.
- Class metadata now accounts only for slots in each page's actual class. Arbitrary empty-page reclassification
  and makeUniformStorageClass (which silently narrowed the promised size) are removed.
- Lua population helpers derive actual entry counts from canonical requirements/contributions. Logical entry
  quotas and byte-budget ceilings remain distinct; tested budgets no longer guess private entry sizeof values.
- Binding-candidate enumeration and explicit/default selection use the existing Script description owner and
  share signature rules with runtime preparation. Basic selection, mismatch, rename-by-symbol and codec tests pass.
  The complete C++/Lua/FlowForge common authoring workflow and source-authored default hints are still incomplete.
- Lua packaging consumes target-generated semantic schema input. The old handwritten scalar size table and
  value/record layout command-line inputs are removed. Generic Event schema output belongs to Script core.
- An attempted new engine/toolchain/script root was rejected by the unchanged architecture guard and removed;
  no replacement aggregate toolchain root or new runtime owner was created.
- P3 handle editing was started prematurely during development regression. It affected the last four negative
  compile tests. The partial change was completely reverted, and that run is excluded from evidence.
  ScriptEventWaitFactory still consumes ScriptEventWaitRequest; P3 is NOT implemented.

### Verified development results

- Fixed P2 snapshot: TOOLCHAIN all + full 199/199 CTest (`p2-stable-tests.log`).
- DEVELOPER all + full 223/223 CTest (`p2-stable-tests.log`).
- Class/storage/native/Lua focused selection: 24/24; later C++/Native/Lua budget regression selection: 20/20.
- Installed consumers: system-hook-script-binding, cpp-coroutine-script, lua-script-packager configure/build/run
  successfully. C++ Event requirement is generated from canonical schema produced by the same composition
  description used at runtime. The first and third second builds report no work; the second needs a final no-op record.
- Current parser requires explicit unexpected<EClassStorageError> in the public class-storage header. That final
  spelling-only correction was validated by the installed coroutine consumer; full profile tests above precede it.
- TrackedSnapshot at eca61106, SourceArchitecture, SourceStyle and InstalledArchitecture for
  install/codegen-p1-developer and install/codegen-toolchain pass.
- Physics's installed schema now participates in all, including TOOLCHAIN with Lua runtime disabled.
- These are incremental development builds, not the requested final independent-clean-clone qualification.

### Overall result: NOT COMPLETE / qualification BLOCKED

The block here is missing implementation/evidence, not a claimed new architecture impossibility:

1. P3 prepared Event handle, one-route claim, owner-only admission, sole payload owner and reentrant copy pin.
2. P4 VM wrapper factory reuse, private typed-helper visibility and Hook reachability caching.
3. Remaining P2 authoring/default/import information accounting and cross-language workflow proof.
4. Clean Debug/Release/Physics-OFF/Lua-disabled/Lua54 qualification, complete relocated consumers, 100x stress.
5. Bsafe/Bwork/Bfinal paired performance, actual assembly/LTO proof, memory ledger and final docs-branch update.

No final production/qualification candidate, optimization speedup, framework freeze, main merge or push is claimed.

## V3 continuation (2026-09-06)

- Authorized scope is I01-I08, continuing Engine `27e9a9f943118c772e0c9b5503f4459ab75d4fff`
  with lux-cxx `e2355ca4ce4e862f4e108f74422a30fce8afb736`.
- The previous unfinished Debug/Release matrix is superseded for this wave: both are
  `NOT_REQUESTED_THIS_ROUND`. All required builds use RelWithDebInfo, including affected Lua54 coverage.
- Main remains at a577c494 with its five user changes; neither main nor those changes are implementation inputs.
- Bentry: clean tracked check succeeded, Developer all-j4-k0 succeeded, full CTest 223/223 succeeded
  (58.53 seconds). This rerun includes the last checkpoint's header correction.
- The exact bin directory (executables/DLLs/PDBs), CMakeCache, compile_commands, build/test logs and SHA256
  manifest are preserved at `build/RelWithDebInfo/s6-v3-evidence/Bentry`. This is a baseline artifact,
  not final clean-clone qualification. Public review evidence will include the manifest and raw measurements.
- I01 begins with real native DLL population regressions for same-stride alignment, a mixed-method frame
  envelope and synchronous-only zero coroutine resources. Bcompare is not fixed yet.
- No I03-I06 optimization or P3 interface migration is admitted until I01/I02 pass and Bcompare is saved.
- I01 real-DLL before tests: state failed at B instance 2, frame failed at A invocation 2, zero-resource sync
  backend was rejected. First frame-fixture run lacked its explicit 16-byte alignment; that run is excluded
  from defect evidence. Corrected before run is `i01-before-corrected-tests.log` (all three expected failures).
- I01 fix preserves exact state layouts and each executable's prepared frame envelope. It removes both
  runtime smallest-fit selections; populations with equal full layouts share slots. Sync-only zero coroutine
  resources are accepted. Allocation-failure catching now remains outside the recipe append lambda.
- Initial after gate: Developer all + 11/11 focused tests pass, including failure injection and benchmark smoke.
  Added telemetry includes reserved slots, live/requested versus occupied frame bytes and complete layout metadata.
- Completed I01 development gate: all + 12/12 focused tests pass, including reversed recipes, shared exact
  layouts, unknown layouts/envelopes, real exhaustion/recycling and observable generated frame destruction.
- I02 uses only `LUX_METHOD(script_attach=true)` for a public mutable non-static
  `void(ScriptBehavior&) noexcept` member. Generated object operations now distinguish host-required objects;
  missing/unattached context fails after construction and is destroyed once, before any lifecycle call.
- The generated installed consumer now covers two independent Entity objects, pending coroutine retirement,
  rematerialization with a new Entity generation, attach/BeginPlay/EndPlay/destructor counts, constructor failure
  and preparation rollback. Static, const and overloaded source methods are generated and invoked as well.
- Eight attach generation negatives supplement the existing 13 negatives. First negative runs lacked an
  explicit ScriptBackend include under parser mode; those results are not semantic proof. Corrected all +
  33/33 focused tests passed (`i02-checked-tests.log`). Final expanded checks still precede Bcompare capture.
- lux-cxx `bbe43fa` adds generic `concat_arrays` to compose existing static/non-static declaration lists;
  its full all + 52/52 tests and install pass. No Script semantic was added to lux-cxx.
- Bcompare harness preparation adds actual 1/4/16/64 declared Event requirements and repeated measured
  registration/delivery/resume/cancellation samples. Requirements are projected from canonical Simulation
  metadata instead of handwritten payload hashes. The awaited source sorts last in the old scan.
- Targeted fan-out uses one Entity/mount and K normal Event callback invocations, then one targeted
  occurrence completes K waits. The first attempted fixture used multiple mounts on one Entity and was
  correctly rejected with SCOPE_MISMATCH; production ownership was not changed to accommodate the benchmark.
- Event smoke coverage now includes idle/fan-out/sparse groups and all four targeted requirement counts.
  Checked 64 targeted waiters: dispatch resumes=0, subsequent stable point resumes=64, repeat checksum=37568.
- CSV schema 6 records Event requirement count/route and allocation-accounting policy. Performance mode
  disables allocation counting; diagnostic mode remains a separate counting pass. Baseline snapshots and
  eventual timing manifest must use the configured compiled SHA, not merely the newest worktree HEAD.
- Bcompare runtime checkpoint is Engine `44b11a60f6bae71f83db257a5f4bb16d267a718c` with lux-cxx `bbe43fa`.
  Developer rebuilt at that compiled SHA: 238/238 and second build no-op. Frozen source worktree:
  `lux-engine-s6-v3-bcompare`; saved bin/hash/cache/log bundle: `build/RelWithDebInfo/s6-v3-evidence/Bcompare`.
  The original deep-developer build is now reference-only (some fixture paths are embedded in executables).
- Independent Bcompare Toolchain: long root `s6-v3-bcompare-toolchain` exceeded MSVC object-file path limits;
  that failed build was not run. Short root `v3bt` rebuilt all and passed 214/214 (109.45 seconds).
- I03 development: new `v3d` Developer all passes; 31/31 focused tests pass (43.16 seconds).
  Artifact content identity is runtime-only, moves with content, and is absent from LXSA. CppStatic now uses
  sorted generated exports, a bounded backend-local validation association with an O(1) inactive eviction list,
  and a generated Ability ordinal resolver. Provider bindings remain instance-specific and are checked cold.
  Tests prove one full validation for 10k instances, identity changes on reload, stable active associations under
  eviction pressure, mismatch rejection and no changed wire bytes. No speedup is claimed before paired runs.
- Artifact public header was synchronized and byte-checked in Debug/RWD/Android include prefixes only;
  this did not configure/build Debug or Android. Container is an explicit public header dependency.
- I04 source suggestions are produced by the C++ template, Lua packager, and FlowForge export metadata.
  Existing headless selection resolves them only against the supplied composition; explicit choices win.
  No suggestion enters LXSA, no runtime rebind loop or new authoring root was added.
- Current Developer all + 6 focused tests pass; Toolchain all + 7 FlowForge/packager/authoring tests pass.
  FlowForge source entry rename is compiled/AOT-loaded and executes using the codec-reloaded old binding.
- New installed-only `script-authoring` consumer creates three real assets (build-time FlowForge compiler,
  Lua source packager, generated C++); the runtime executable does not link the FlowForge compiler.
  Fresh copied consumer ran three processes: original, source-renamed, then defaults removed. Each observed
  cpp=30, provider=60, calls=6. Incompatible C++ signature is rejected before user execution.
  Saved bindings remain SHA256 `07769007b77559681fbdb96dffc4216b7878248cb953362383ec03522afef082`.
  Consumer second build is no-op. Development closure logs: `build/RelWithDebInfo/cc`.
- That test first exposed Lua packager's implicit content-derived AssetId changing on source edits.
  Added explicit existing AssetId input (`ASSET_ID` / `--asset-id`) for authored reimport, without changing
  asset wire or inventing a Script residency handle. Without this input, one-off content-addressed packaging
  remains supported. Stable/non-nil UUID validation and retained identity across changed contents are tested.
- Full isolated final install/relocation and paired performance remain pending. The development consumer
  used current v3d + codegen-toolchain SDKs, not a final qualification prefix.
- I05 development replaces raw runtime Event requests with instance/layout/runtime-scoped prepared handles.
  Native imports, Lua local slots and C++ descriptor-bound typed source tokens resolve only during preparation.
  Normal Event admission no longer searches semantic requirements or endpoint identities.
- Awaitable is the only Event result owner. Stable-slot backing plus a short write pin keeps inline/spilled bytes
  valid across copy callbacks that cancel an instance or erase a different record; deferred releases finish on unpin.
  Event-only admission constructs no external completion capability/lease. Claim performs one route lookup per
  occurrence and preserves callback/cutoff ordering. Outstanding claim scratch also participates in admission
  backpressure so reentrant cancellation/re-registration cannot overflow it.
- First shared regression exposed physical stable-slot indexes exceeding the external ticket array's old logical
  count. Ticket storage now covers the actual page-rounded slot capacity; logical concurrent admission is unchanged.
  Reserved slots, backing bytes and ticket bytes are explicit metrics, including small-population page padding.
- Developer all + 46/46 focused tests pass (`v3d/i05-complete-tests.log`, 40.91 seconds), including LuaJIT on/off,
  Scene 0/2/4 workers, continuation, C++ and Event pin/cancellation/provenance tests. A separate race-only Event
  test skips the 10k/50k/100k one-time structure cases for later 100x stress. Toolchain closure is still running.
- I05 Toolchain all + 34/34 focused C++/continuation/Event/FlowForge tests pass (41.52 seconds).
  The one remaining direct-backend negative fixture was migrated to prepared Event entries; no runtime fallback remains.
  lux-cxx dependency is now `af25d29` (prepared stable insertion and exact backing-capacity accounting).
- I06 development: per-VM four-shape safe wrapper factory cache, known host-table capacity hints, and
  VM-neutral typed numeric range/conversion definitions visible at composition callsites. Provenance and
  VM C API access remain behind the existing narrow DLL boundary; no entire State/VM headers were exposed.
  Argument count is returned by the existing invocation access check instead of another DLL call.
  Existing prototype-cache lifetime policy is unchanged; factory closures capture no prototype/provider authority.
- Cold graph construction binary-searches the sorted execution points and lazily caches two reachability
  walks per needed Hook. Region tests assert four walks for two Hooks. Statistics cover traversal and backing
  memory; this is not a claim that all graph-building subalgorithms are linear.
- Removed redundant seal/reset/discard functions from the Script endpoint adapter; Simulation-owned Channel
  lifecycle remains intact. Standalone tests borrow their concrete typed Channel via the existing owner association.
  Removed retired reflection-era CppStatic runtime error values.
- I06 Developer all + 21/21 focused Hook/Lua/Event/Scene tests pass (`v3d/i06-tests.log`, 2.75 seconds).
  No final correctness/performance qualification or freeze claim is made yet.
