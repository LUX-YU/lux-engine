# S6 safety / TaskGraph / HookChannel joint closure qualification — 2026-09-05

## Revision and status

- Starting main: `8354ae10e5d247cbc69746ae1f79c97ddfdd5ab9`.
- Final implementation/test snapshot: `5f03e9b156421583ae81857025ec6156ad0e0f05`.
- Independent clean clone: `E:/SyncForder/CodeRepos/lux-engine-hook-qualified`.
- Implementation branch/worktree: `codex/s6-hook-channel-closure`, `lux-engine-hook-closure`.
- Status: **IMPLEMENTATION COMPLETE / QUALIFICATION CANDIDATE PASSED / AWAITING INDEPENDENT REVIEW**.
- No push, R1, new scheduler/manager/backend or normative framework-freeze declaration is included.

The main worktree's four existing user edits were not qualification inputs. The implementation and clean clone
contain only tracked source. Historical evidence is retained; later review notes supersede its freeze authority and
identify the unproven Lua 5.4 GC attribution without rewriting historical measurements.

## G0 safety closure

Lua closure provenance is checked before local-slot indexing or typed dispatch. Closures retain a full-userdata
layout token plus backend identity and local ordinal; reachable old closures retain the token. A foreign artifact
with the same machine signature cannot redirect a call to another provider. Same-prototype instances use their own
prepared binding. The negative was reproduced before the fix; afterwards provider and Event-wait registration
counts remain zero on mismatch. Retained old closures and prototype recreation are covered.

Nested same-backend execution is explicitly tested for inner success, error and coroutine yield. The outer Lua
entry invokes its own Ability again after the inner return, proving execution-context restoration. The suspended
inner coroutine subsequently resumes with a fresh ScriptStepContext and owned packet. Both JIT-on and interpreter
tests pass. No global/thread-local current Script or saved cross-suspension ScriptStepContext was added.

LuaJIT's Windows error unwinding required a backend-local hidden Lua wrapper: typed C++ thunks return status/value,
then Lua raises or yields after the C++ noexcept frame has returned. This preserves the canonical user surface and
existing continuation runtime; it is not a second awaitable or scheduler.

External values are validated against runtime size, owned layout, identity/alignment and 32-byte transport limits
before provider admission. Void, 32-byte success, 33/64-byte rejection, over-alignment and long canonical names are
covered. Rejected external cases start zero providers and retain zero Awaitables/continuations. A safe owner-local
64-byte Event copy remains supported. Eager, FAILED, FULL retry, cancellation/reuse, stop and late results retain
their existing semantics.

## Compiled execution and ownership

The existing L0 TaskGraph/TaskExecutor is unchanged. Simulation cold preparation collects named tasks/Hooks and
separate construction/execution dependencies, validates references/cycles/ambiguity, then submits one graph.
Every authoritative task—including provider-state-only work—takes the Simulation-local READ fence; Script-capable
Hooks take WRITE plus caller affinity. Business order is explicit, not a side effect of resource registration.

Production Hook dispatch requires a private graph-issued HookInvocation. Script channel delivery is separately
authorized only in the compiled delivery phase, rejects early/manual consumption and cannot reenter itself.
Connected Script endpoints without runtime callbacks fail execution before clock advance or any task/user code,
including connections made after an initially empty graph was sealed.

Real tests use 0/1/2/4 workers, reverse registration, an independently scheduled provider-state task, dedicated
caller ownership and two simultaneously executing Simulations. Multi-region tests exercise:

```text
parallel producer work → caller Hook/commit → worker continuation
→ caller stable Hook/commit → final derived propagation → Scene publication
```

HookChannel storage is Simulation-owned. Typed factories create bounded lanes; prepared ports activate only in
their declared logical producer stages. Lanes merge by stable System/stage identity, then record order. Native
consumers borrow sealed spans without flattening. Non-scalar values require an explicit noexcept ownership copy.
No per-record mutex/atomic or transport subscriber store exists. ScriptSystem still owns exactly one endpoint
connection, EventBuckets, generational one-shot waiters, Awaitables, continuation lists and ResumeRing.

Overflow fails the step, skips dependent work and allows cleanup/reset. Owner re-production is explicit, scoped to
the declared Hook, and goes into a separate bounded next generation. The next-generation/cutoff and multi-consumer
storage lifetime are tested. Mounted instance/waiter population never changes graph shape.

Simulation wire is v8. Script schema is 12 / LXSA wire 10; Native ABI remains v5. Event delivery Hook identity,
normalized producer/execution relationships and re-production policy enter exact compatibility checking. Current
System registration, artifact and endpoint must match before user code. The old asset-local comparison helper was
replaced by a shared stateless Simulation contract comparison; there is no compatibility shim.

## Commands, lifecycle and resume

Prepared command producers are point-owned and assigned deterministic storage order. EcsCommandBuffer snapshots
its producer records into a second preallocated bank before applying Registry operations. Observer-generated
commands enter a subsequent batch; they cannot self-feed the current commit. DeferredEntity generations distinguish
batches. The final scripted commit's follow-ups are not flushed behind final derived propagation; they await the
next step. Non-scripted Simulation retains its ordinary final commit.

Retirement first stops admission and invalidates the instance generation. Current native invocation stacks exit
before physical continuation/object destruction. Cleanup follows only the incarnation's owned waiter/Awaitable/
continuation lists, then EndPlay and destroy. Failure cleanup is retirement-only, not new BeginPlay admission.
Entity component availability during EndPlay remains the existing limited contract; EnTT destruction order is not
changed. Foreign component/Registry apply failure does not roll back an already-applied prefix.

Each actual step has one stable-resume Hook. Other Hooks may mark READY but never resume or refill budget. External
ingress captures a frontier and bounded admission budget; a publication hole ends the drain without waiting or
overtaking. The existing ring definition was moved unchanged into a non-installed private header so a deterministic
test can hold a reserved cell, publish a later cell, prove nonblocking front(), FULL retry, fixed frontier, stale
generation and close behavior. No production test callback or alternate ring was introduced.

Scene ScriptRuntimeSystem owns ScriptSystem and Timer integration, binds neutral callbacks while paused, and
disconnects before teardown. The old Scene gameplay-pump task and mutable ScriptSystem accessor are removed.
Live stats are owner-only; an existing LatestSpscExchange publishes copies for one observation consumer.

## Clean qualification matrix

All rows below use the same final snapshot above, MSVC 19.44.35228.0, Ninja and `all -j 4 -k 0`.

| Configuration | Result | second build |
|---|---:|---|
| DEVELOPER / RelWithDebInfo / LuaJIT | 200/200 full CTest | no work |
| TOOLCHAIN / RelWithDebInfo / VM-independent | 176/176 full CTest | no work |
| DEVELOPER / Physics2D OFF / LuaJIT | 197/197 full CTest | no work |
| DEVELOPER / Debug / LuaJIT focused | 21/21 + frontier 1/1 | no work |
| Release source-composed installed closures | 5/5 consumers | no work |

Roots are `build/RelWithDebInfo/hook-q-{developer,toolchain,physics-off}` and `build/Debug/hook-q-developer`.
Reproduction helper: `cmake/RunScriptHookClosureQualification.ps1`; each root retains commands, logs and
`qualification.json` with the exact SHA. Release consumers link the qualified non-Debug RelWithDebInfo SDK;
this is not a claim of a separate full Release Engine CTest matrix.

PLAYER, EDITOR and Lua 5.4 configure/build/test/sampling are **NOT RUN** by explicit user scope. Android was not
configured/built. LuaJIT interpreter-only is included in the focused/full suites, including provenance, coroutine
and four-worker Scene execution. C++, Native/FlowForge, Physics2D and real Process Timer remain covered.

100× repeat-until-fail:

- RelWithDebInfo: 14 selected core/provenance/continuation/Event/lifecycle/multi-worker/Scene tests, all 100 repeats.
- Toolchain: FlowForge artifact, AOT runtime and Physics FlowForge integration, all 100 repeats (3 tests).
- Debug: 8 selected owner/frontier/Event/lifecycle/Scene tests, all 100 repeats.
- Logs: the corresponding `stress-100.log` files; reproduction `.internal/Run-HookClosureStress.ps1`.

## Installed, source and dependency closure

Fresh prefix: `E:/SyncForder/CodeRepos/install/RelWithDebInfo/hook-q-luajit` (Toolchain + Developer SDK closure).
Ten relocated installed-only consumers fresh-configure/build/run/no-op: Physics2D, Event runtime, FlowForge
compiler, Lua packager/runtime, Scene runtime, Ability codegen, Hook binding, C++ coroutine, static Ability and IPO.
Five also run in Release: C++ coroutine, Lua runtime, Event runtime, Hook binding and IPO. Their roots are
`build/RelWithDebInfo/hook-q-consumers-final` and `build/Release/hook-q-consumers-final`.

The C++ consumer uses real Simulation → Physics → HookChannel → Script → Event.await → NextStep → Entity reuse →
teardown, with four workers and no manual gameplay dispatch/resume. External Inventory metadata produces canonical
Event schema, packages one Lua source, publishes a real installed System provider, and runs lifecycle/eager Ability/
Event suspension through the same graph. Optimized consumer assertions remain active; a stale expected value
(4 after setting the provider to 6) was exposed and corrected rather than silently skipped by NDEBUG.

ValidateTrackedSnapshot, ValidateSourceArchitecture, ValidateSourceStyle and ValidateInstalledArchitecture pass.
Targeted guards keep EventPoint transport retired, HookChannel free of subscriber/mutex/atomic transport, Scene free
of a second gameplay pump and L0 TaskGraph free of Simulation/Script ontology. Physics OFF compile commands contain
no Box2D/Physics2D input; Physics public links expose Eigen, not Box2D. Runtime Script/Native DLL dependency
inventories contain no LLVM, MLIR or FlowForge compiler. The Lua runtime alone contains its selected lua51 dependency.

Modules header synchronization to Debug, RelWithDebInfo and Android include prefixes is byte-identical:

| Header | SHA-256 |
|---|---|
| ScriptEvent.hpp | DDBBB6FA5D54BC07C6EF3D12311CF90DAF4E4B9BB325E77BCD08D43164E03448 |
| description/Script.hpp | 5BFE24D95AD3F02CC8D12533AADEB2879637D59DEC0F9236AFA4EA6824CC813F |

## Performance and limitations

See the separate `l5-v3-script-s6-hook-channel-performance-2026-09-05.md` and Hook migration inventory. B0/B1/B2
paired measurements, FlowForge/C++/Physics, worker comparison, allocator diagnostics and VTune are recorded without
an absolute performance PASS threshold. Safety has a measured cost; the evidence does not claim “no regression.”

No automatic channel resizing, Lua coroutine pooling, live owner migration, universal Event/variant/collection
framework, second scheduler or new backend is provided. Physics collision/trigger fixtures remain explicitly
synthetic where no production event protocol exists. AssetLoad residency handles, Navigation, asynchronous
BeginPlay/EndPlay, temporary activation policy and R1 are not implemented by this wave.

Final disposition: **qualification candidate passed; independent review required**.
