# L5 v3 Script S3-H Suspension/Lifetime Hardening Qualification Evidence — 2026-09-03

## Qualified revision

- Production implementation and regression tests: `97e68dc9c3f222c2f09be3ebefe513b697fd128d`.
- Qualification used a detached worktree at that exact revision.
- `ValidateTrackedSnapshot.cmake`, `git status --short` and `git diff --exit-code` all confirmed a clean tracked
  snapshot. The developer checkout's pre-existing `.gitignore` and `WorldPartition.hpp` changes were not included.

## Suspension analysis closure

`flowforge_compiler` now builds one private source-level `SuspensionAnalysis` before validation and AOT generation.
It records directly reachable async Ability calls and graph-function call edges, then reaches a deterministic
fixed point for transitive `may_suspend` summaries. Recursive call-graph components terminate without recursive
analysis; an async witness propagates through the complete component.

Lifecycle and borrowed-value validation consume the same summary:

- BeginPlay/EndPlay exports which call an async graph function fail with
  `ASYNC_LIFECYCLE_NOT_SUPPORTED` before IR/AOT generation.
- `BORROWED_STEP` values used after an async graph-function call fail with
  `BORROWED_VALUE_CROSSES_SUSPENSION`.
- Path validation keeps separate pre/post-suspension visitation state. Reaching a consumer on a safe fan-in leg no
  longer hides another suspension-bearing leg, and the result is independent of link insertion order.
- AOT still performs mechanical whole-module inlining and marker lowering, but verifies that the generated step
  entry presence agrees with the shared source analysis. It does not maintain a second source call-graph policy.

The analysis is a private Toolchain implementation detail. No FlowForge runtime, scheduler, VM, public graph API or
Native ABI version was added or changed.

## Borrowed Ability import closure

Native ABI v3 already represents `LUX_SCRIPT_PASS_CONST_REF`. The loader now accepts it for synchronous QUERY
Ability import results while retaining these fail-closed boundaries:

- exported Script function returns remain VALUE-only;
- COMMAND Ability imports cannot describe results;
- ASYNC_OPERATION eventual results remain VALUE-only;
- `NativeScriptBackend` still matches the import type, pass and layout exactly against canonical Ability metadata.

The positive test executes:

```text
BORROWED_STEP QUERY
    -> immediate COMMAND consumer
    -> value discarded
    -> async wait
```

through FlowGraph compile, NativeModule load, prepared provider invocation and step suspend/resume. The import retains
`CONST_REF`, while the generated erased call copies the current referenced value into its same-step result slot; no
borrowed value is persisted in the continuation frame. A separate malformed native fixture proves that a CONST_REF
exported function return remains `INVALID_MODULE`.

## Correctness qualification

Windows 11 Pro 10.0.26200, MSVC 19.44.35228, x64 RelWithDebInfo, `-j 4 -k 0`:

| Profile | CTest | Final build |
|---|---:|---|
| DEVELOPER | 178/178 | `ninja: no work to do` |
| PLAYER | 178/178 | `ninja: no work to do` |
| EDITOR | 191/191 | `ninja: no work to do` |
| TOOLCHAIN | 167/167 | `ninja: no work to do` |

`ValidateSourceArchitecture.cmake` passed from the clean source tree. One hundred repeat-until-fail runs passed for:

```text
flowforge_script_artifact_test
flowforge_script_runtime_integration_test
script_native_runtime_contract_test
simulation_script_native_test
```

The focused regression matrix covers indirect async lifecycle calls, indirect borrowed crossing, fan-in in both link
orders, legal same-step borrowed consumption, Native loader return-shape isolation and existing end-to-end FlowForge
continuation execution.

## Installed closure

A fresh combined prefix at `E:/SyncForder/CodeRepos/install/RelWithDebInfo-s3h` passed
`ValidateInstalledArchitecture.cmake`. Fresh relocated consumers configured, built, ran and produced no-work second
builds:

- `script-ability-codegen`;
- `flowforge-model`;
- `flowforge-compiler` with an explicit `LUX_FLOWFORGE_LINKER`;
- `scene-script-runtime`.

The changed public ABI header was synchronized byte-identically to the Debug, RelWithDebInfo and Android install
include prefixes. Android was not configured or built.

## Focused PB1 regression

The runtime/AOT hot path was not changed, so the historical PB1 baseline remains the canonical performance record.
Focused measurements used the same seed `1592598566` on an Intel Core i7-13700KF (16 physical / 24 logical cores):

| Scenario | Parameters | S3-H result | Historical PB1 observation |
|---|---:|---:|---:|
| Ability QUERY | 10k calls | p50 0.0659 ms | p50 0.0651–0.0688 ms |
| Suspend | 10k objects | 2.8796 ms | 2.8063–3.0884 ms |
| Steady resume | 2k/frame after a 50k burst | p50 0.3740 ms | typical 0.27–0.42 ms |
| Update-heavy | 20k objects, 500 frames | p50 0.2064 ms | p50 0.2413 ms |

Benchmark smoke also passed. The focused data shows no material regression and does not replace or rewrite PB1.
Raw CSV remains a build artifact under `lux-engine-s3h-toolchain/pb1-focused`.

## Explicitly not done

- S2.4 AssetLoad remains blocked by the script-visible residency-backed Asset handle contract.
- S4 Lua coroutine/yield-resume is not done.
- Lua production lifecycle authoring closure is not done.
- S5 Event.await and Physics/Navigation production Abilities are not done.
- S6 C++ coroutine ergonomics/static specialization is not done.
- Python runtime is not done.
- Async BeginPlay/EndPlay and temporary Script activation/deactivation are not done.

S3-H is qualified. The compile-time suspension/lifetime closure is sufficient to proceed to S4 without reopening the
S3 runtime architecture.
