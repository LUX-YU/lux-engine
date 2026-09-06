# S6 v3 implementation checkpoint — BLOCKED, not a qualified candidate

Date: 2026-09-06. This records the current partial implementation and actual verification.
It does not replace historical baseline numbers or declare a framework freeze.

## Identity and scope

- Entry Engine: `27e9a9f943118c772e0c9b5503f4459ab75d4fff`.
- Entry lux-cxx: `e2355ca4ce4e862f4e108f74422a30fce8afb736`.
- Bcompare runtime: `44b11a60f6bae71f83db257a5f4bb16d267a718c`.
- Bcompare supplementary benchmark driver: `c2594b21b46abe74d6a86f59d9280404a21e384e` on
  `codex/s6-v3-reference-driver`. Only benchmark source differs from the Bcompare runtime.
  This driver has not yet been rebuilt or formally paired; do not identify it as measured Bcompare.
- Runtime/benchmark qualification snapshot: `ab0eb127493799ab914f3976ad3273dfe9246660`.
- Subsequent production fix: `babbd2d220aba2d957c5628781db6157258d5f8e` (remove absent Lua packager
  header directory from installation; no C++ runtime modification).
- Checkpoint measurement/consumer tooling: `d1337df715402a502d2ba195d196e5b2a9e45f29`.
- lux-cxx: `af25d29dbd54b6e140ca24814508bbde91fac13a`.
- Additional dependency inspected, NOT changed: lux-cmake-toolset
  `961c63eda82448b8108219461ba624ff016b2297`.
- Main remains `a577c49409e2519029693fbe780fe8ce3ab2dc1e`; its five user changes remain outside this work.
- No merge, push, R1, GUI, new runtime/scheduler, or freeze declaration.

## Actual implementation status

| Item | Implemented | Remaining qualification or limitation |
|---|---|---|
| I01 | Exact Native state `(size, alignment)` recipe association; executable frame envelope association; same-layout shared quota; synchronous zero-frame configuration | Formal B07 three-process capacity/time records not collected |
| I02 | Explicit generated `script_attach=true`; typed host attach and `requires_host`; static/const/overloaded export coverage; lifecycle cleanup | Final isolated installed consumer rerun outstanding |
| I03 | Runtime-only Artifact content ScopeId; bounded active validation association; sorted generated export search; generated Ability ordinal resolver | Exhaustive source/include/template/validation/ledger/schema/macro/target-option invalidation matrix outstanding |
| I04 | Three-language source hints; explicit binding priority; stable authored Lua AssetId input; real generated/AOT/packaged reimport consumer | Development installed consumer passed, final relocated SDK closure outstanding; GUI intentionally absent |
| I05 | Instance/layout/runtime-scoped Event handles; prepare-only semantic resolution; one route lookup per occurrence; stable Awaitable slots; final result owner; write-pin/deferred reclaim; no Event external completion capability | Final 100x selected stress and Lua54 closure outstanding |
| I06 | Per-VM safe wrapper factories; table sizing hints; narrow typed conversion access; per-Hook reachability cache; endpoint lifecycle authority removed | Existing Lua prototype/function cache lives until backend teardown; no new explicit prototype unload/hot-reload API; final machine-code and retained-memory report outstanding |
| I07 | Independent clean clones, Developer full test/build/install and Toolchain full build/test/no-op run | Relocation blocked; remaining configurations stopped/not started; final stress/install/dependency closure incomplete |
| I08 | Added generated C++ Update, real multi-Region numeric graph, cold graph groups; Event requirement/targeted benchmark inputs; paired-run and summarization scripts | Formal paired measurements NOT RUN; no performance acceptance classification is justified |

I01/I02 real negative reproductions, development test results and intermediate failures are retained in
`l5-v3-script-s6-codegen-closure-worklog.md`. Those older results are not the final qualification matrix.

## Execution/data ownership after I05

```
Artifact semantic Event requirement
  -> mount exact validation
  -> per-incarnation PreparedScriptEventAdmission / opaque handle
  -> Lua local slot | Native import ordinal | generated C++ token mapping
  -> Event waiter -> one Awaitable result allocation
  -> occurrence route lookup -> claim eligible prefix -> callbacks
  -> generation revalidation -> pinned typed copy -> READY
  -> existing ResumeRing -> unique stable-point budgeted resume
```

Handles carry runtime identity, instance generation, layout identity and local ordinal. Runtime buckets and
pool slots are not serialized. ENTITY_TARGETED remains exact-self-only. Native ABI remains v5.
Cancellation invalidates ownership immediately; a copy pin prevents memory reuse until copy returns.
Claim scratch also participates in admission backpressure when nested dispatch cancels/re-registers waiters.

Only external Ability completion opens the external ticket/lease path. Event result storage no longer has
a waiter-owned intermediate payload. External 32-byte admission, MPSC/FULL/eager/late/close rules remain.

StableSlotMap reserves pages. Its physical slot capacity can exceed logical admission capacity; external ticket
index storage covers those physical indexes. Statistics expose that padding. It is not counted as live usage.
No idle waiter scan or global-population per-instance retirement loop was introduced.

## Fixed facts / remaining cold work

| Fact | Owner / fixed when | Invalidation / remaining work |
|---|---|---|
| symbol, signature, attach, lifecycle | C++ source template and canonical semantic declarations | generation/target compilation; decoded Artifact exact validation remains cold |
| coroutine argument ownership layout | generated typed operations / target compiler | no runtime reflection plan; bounded argument + actual compiler frame allocations remain separate |
| local Ability/Event order | generated contract or artifact-local prototype | new content/layout; provider receivers are still prepared per instance |
| full Artifact/executable association | bounded CppStatic backend admission cache | new runtime content identity or executable association; active records cannot be evicted |
| Native state/frame class | population layout recipe + module executable envelope | new prepared layout; no hot class fallback or heap fallback |
| Event result layout/endpoint | mount preparation | runtime/incarnation/layout generation; hot wait validates handle provenance and capacity |
| marshal/copy | canonical typed owner or generated projector | unsupported layout fails preparation/admission |
| wrapper shape | once per VM per used fixed shape | prototype/token closures are distinct; cache does not capture provider authority |

Removed paths include the CppStatic callable hash duplicate, per-instance repeated full contract comparison,
runtime Event identity scan, waiter intermediate result storage, redundant Script endpoint seal/reset/discard
functions, and repeated Lua wrapper source compilation. The normal Lua bridge still crosses explicit DLL/VM
boundaries; no unsupported inlining/speed claim is made.

## Fresh matrix actually run

All rows here are RelWithDebInfo, MSVC 19.44.35228.0, Ninja, `all -j 4 -k 0`.
Source clones: `lux-engine-v3q`, `lux-cxx-v3q`; build roots `build/RelWithDebInfo/q/*`.

| Snapshot / configuration | Build | CTest | Second build | Install |
|---|---|---|---|---|
| lux-cxx af25d29, q/c | PASS | 50/50, 14.01 s | not yet final no-op qualification | PASS, but non-relocatable exports |
| Engine ab0eb127, Developer LuaJIT Physics ON, q/d | PASS | 237/237, 98.59 s | no work | PASS, but non-relocatable exports |
| Engine ab0eb127, Toolchain, q/t | PASS | 215/215, 98.37 s | no work | FAILED: absent packager include directory |
| Engine babbd2d2, Toolchain install-fix follow-up | PASS | 215/215, 72.24 s | no work | PASS, but non-relocatable exports |
| Physics OFF, q/p | INTERRUPTED | NOT RUN | NOT RUN | NOT RUN |
| Lua disabled | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Lua54 | NOT RUN | NOT RUN | NOT RUN | NOT RUN |

The in-flight Physics OFF process tree was explicitly stopped after discovering the installation scope blocker;
no binary from its incomplete build was executed. Remaining automatic matrix entries were not launched.
Debug, extra Release, PLAYER, EDITOR, Android and Linux are NOT_REQUESTED_THIS_ROUND.

The clean lux-cxx first configure registers 50 tests. Its existing CMake condition registers `generator_test`
and `generator_validation_failure` only when the generator executable already exists at configure time.
A subsequent configure/build and those two tests are still required; old development 52/52 results are not
silently substituted for the clean clone's 50/50.

The Toolchain installation failure is fixed in babbd2d2. Its fresh-snapshot follow-up results are recorded
separately in the qualification manifest; the failed ab0eb127 install remains a failed historical observation.

Static gates on clean babbd2d2: `ValidateTrackedSnapshot`, `ValidateSourceArchitecture`, and
`ValidateSourceStyle` passed. `ValidateInstalledArchitecture` passed for fresh Developer and fixed Toolchain
prefixes. These architecture checks do not test relocation; their PASS does not override the failing relocation
probe. No build or validation process remains running at this checkpoint.

## Confirmed installation blocker outside the approved two-repository changes

`lux-cmake-toolset/component/install_components.cmake` uses the configure-time absolute installation prefix
for component headers, config destinations and generated import metadata. Its `component_dep.cmake.in`
also embeds that prefix. Both freshly installed Engine and lux-cxx packages contain these paths.

Minimal reproduction (no C++ compilation/runtime execution and no edits to installed package files):

1. Copy fresh `q/c/share/lux-cxx` and `q/c/include` into `D:/LuxV3RelocationProbe/c`.
2. Configure `cmake/installed-consumers/sdk-relocation` with that copy as `CMAKE_PREFIX_PATH`,
   `EXPECTED_PREFIX=D:/LuxV3RelocationProbe/c`, original `q/c` in `CMAKE_IGNORE_PREFIX_PATH`, and both package
   registries disabled.
3. Configure exits **1**. The imported include and assets properties still resolve to the original prefix:

```
expected_prefix=D:/LuxV3RelocationProbe/c
include=E:/SyncForder/CodeRepos/install/q/c/include
assets=E:/SyncForder/CodeRepos/install/q/c/share/lux-cxx/container
```

Thus a consumer that "passes" while the original installation remains available is not proof of relocation.
The same defect is visible in Engine `script_core` imports. `CMAKE_IGNORE_PREFIX_PATH` cannot rewrite explicit
paths embedded in imported targets. Manually rewriting copied package files is prohibited and was not done.

Smallest next scope decision: authorize a separate lux-cmake-toolset branch to generate prefix-relative install
and import metadata, qualify that generic fix, then regenerate both SDKs. Do not add an Engine-only shim or
patch installed files. This is an installation dependency fix, not a change to Script/Simulation semantics.

## Performance and pending requirements

No five-process Bcompare/Bfinal series has been collected. Therefore B01–B10 currently have no accepted
IMPROVED / NO_MEASURABLE_GAIN / REGRESSION_EXPLAINED classification. Lack of measurement is not "no gain".

Diagnostic multi-Region runs showed actual worker overlap and equal 0/4-worker business checksums; a development
1024-node/8-Hook graph recorded 16 reachability walks. These were development probes, not independent paired
timing or clean final qualification. They do not establish speedup or hardware cache/branch explanations.

Additional mandatory gaps to finish after the installation scope is resolved:

- B05's exact combined NextStep -> Event -> Simulation Delay sequence and full per-language completed-work audit;
- B02 complete tiny-boundary/IPO comparisons and B06 complete backend population/retirement records;
- final Lua54 measurements, selected 100x stress, full invalidation matrix and final installed three-language reimport;
- Bcompare driver rebuild with declared dependency versions; binary/DLL/PDB manifests and the five balanced pairs;
- final memory/operation CSVs, valid/invalid run audit, final nonempty entry disassembly and representative VTune;
- all four final validators and dependency isolation audit on the eventual accepted source snapshot.

The current checkpoint's static validator results above are retained. They must be repeated on the eventual
accepted snapshot after remaining implementation/dependency work; this is not a claim that they have never run.

FlowForge liveness, args/frame merge, provider-bound plan cache, PGO, unsafe Lua entry/allocator/pooling and GUI
remain explicitly deferred. They have not been evaluated or passed in this checkpoint.

## Verdict

**BLOCKED. Implementation has advanced, but the required joint qualification/performance candidate is not complete.**
The external installation scope issue needs a decision before widening repository changes. Incomplete matrix,
stress, generation and performance work remains explicit; it is not hidden as future work or a performance PASS.
