# S6-only test scope pruning — 2026-09-06

## Scope and source identity

The user explicitly requested deletion of tests unrelated to the current S6 work, not disabling them.
This supersedes the earlier read-only inventory's recommendation to keep unrelated regression sources.

- Before: `5ba2dd6f37e8087811ee8a9129baa13705659933`.
- Removal implementation: `9d40ec68489c17b8f4c91091974cb6b54263d0b1`.
- Branch: `codex/s6-deep-optimization`; no merge or push.
- 622 tracked test/fixture/consumer/obsolete benchmark files deleted.
- 102 CMake/validator files changed; implementation commit: 724 files, 9 insertions, 104,816 deletions.
- Production implementation sources and installed public headers were not edited.
- Removed test-only instrumentation definitions and test-specific fixture-presence guards alongside their targets.
  Production architecture/dependency checks remain active.
- No archive copy, disabled alternate suite, or compatibility switch was added.
  Deleted sources are recoverable from the parent commit in Git.

Main at `a577c49409e2519029693fbe780fe8ce3ab2dc1e` and its five user modifications remain untouched.
Existing measurement-driver edits and the untracked stress driver are not included in the removal commit.

## Retained coverage

Only the current Script/codegen/Event/Hook/Scene/Physics closure and its immediate safety dependencies remain.
This is intentionally reduced coverage, not a claim that every removed test had equivalent assertions elsewhere.

| Developer category | Registrations |
|---|---:|
| Script runtime / generated C++ / Lua / Native | 62 |
| Simulation / Hook / ECS / Transform | 20 |
| Architecture layer gates | 13 |
| Scene integration, including all failing Lua variants | 10 |
| Process execution / Timer | 6 |
| Physics2D | 3 |
| FlowForge source | 2 |
| Task graph / semantic metadata | 2 |
| Total | 118 |

The Toolchain profile additionally exercises FlowForge compilation/AOT and Lua packaging, without a Lua runtime.

Retained scenarios include all 21 generated-C++ negative compilation cases, coroutine ownership,
prepared storage, Event cutoff/copy reentry, continuation retirement, multi-worker Scene, schema,
generation, and lifecycle. Their current S6 relevance is why they were not removed after historical passes.

Removed coverage includes unrelated UI/Editor/Render/Material/resource-cooker tests, generic
Object/serialization/math suites, old World/Streaming/asset-loading tests, legacy test/benchmark trees,
obsolete architecture probes, and unrelated installed consumers.

Exact registration inventories:

- [Remaining 118 Developer registrations](evidence/s6-test-pruning/retained-developer.csv).
- [Removed 121 Developer registrations](evidence/s6-test-pruning/removed-developer.csv).

Additional deletions were disabled/unregistered tests and standalone consumers, so 622 files is not
the number of removed CTest registrations. No registration was added to Developer.

## Verification

Clean detached source: `E:/SyncForder/CodeRepos/lux-engine-v3r`, at the removal implementation commit.
Existing build roots were reconfigured against that tracked snapshot; this was not a new full S6 matrix
or a fresh-prefix relocation qualification.

MSVC 19.44.35228.0 (toolset directory 14.44.35207), CMake 4.1.2, Ninja, RelWithDebInfo;
build and execution were serial:

```text
cmake -DLUX_SOURCE_DIR=<clean-source> -P cmake/ValidateTrackedSnapshot.cmake
cmake -S <clean-source> -B <existing-profile-build>
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> -C RelWithDebInfo --output-on-failure
cmake --install <build>
cmake -DINSTALL_PREFIX=<profile-prefix> -P cmake/ValidateInstalledArchitecture.cmake
```

| Profile | Before registrations | After result | Before CTest wall | After CTest wall |
|---|---:|---|---:|---:|
| Developer, LuaJIT, Physics ON | 239: 233 pass / 6 fail | 118: 112 pass / 6 fail | 78.97 s | 52.58 s |
| Toolchain, Lua disabled, Physics ON | 216: 216 pass | 106: 106 pass | 75.03 s | 47.56 s |

Both `all` builds succeeded and both second builds printed `ninja: no work to do`.
Both refreshed install prefixes passed the unchanged installed-architecture validator.
Source architecture, source style, and clean tracked snapshot validation passed.

These are single CTest invocations, not paired engine-performance measurements. The Developer run is
26.39 seconds shorter with 121 fewer registrations; it is not evidence of faster gameplay execution.

Raw outputs are committed beside the inventories:
[Developer](evidence/s6-test-pruning/developer-ctest.txt),
[Toolchain](evidence/s6-test-pruning/toolchain-ctest.txt).
Build/configure/install logs remain under
`E:/SyncForder/CodeRepos/build/RelWithDebInfo/q2/{d,t}/prune-*.log`.

## Existing failure remains visible

All six existing Scene Lua failures remain registered and fail at the same assertion:

```text
engine/scene/integration/script/test/scene_script_lua_runtime_test.cpp:487
runtime->scriptSystem().activeContinuationCount() == 0U
```

Variants: default, workers 0/2/4, interpreter default, interpreter workers 4.
No assertion, expected result, fixture, runtime implementation, or failing test was changed to hide this.
This deletion task does not diagnose or fix the failure; S6 qualification remains blocked.

## Not run for this cleanup

- Additional Physics-OFF, Lua-disabled runtime, Lua54, Player, Editor, Debug, Release, Android or Linux builds.
- New relocation/installed-consumer builds, 100x stress, paired benchmarks, VTune.
- S6 implementation continuation, framework freeze, merge or push.

The retained tests and consumers are available for the next relevant S6 work.
