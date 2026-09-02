# L5 v3 Runtime Scripting S2 Time-Awaitable Qualification Evidence

Date: 2026-09-02

Status: **PASS — S2.0 through S2.3 are qualified; S2.4 AssetLoad is stopped on the missing Asset handle contract**

Qualified source revision: `0cbd4f265bc1e9fa2e36e9d581937e4315fef7cf`

## S2.0 production stable point and clock

- `Simulation` owns a monotonic nanosecond clock. Every valid execution advances `step_index` once before task
  dispatch and records the supplied effective delta; zero delta advances the step but not elapsed simulation time.
- Negative delta, elapsed-time overflow and step-index overflow fail before task execution.
- Simulation Systems explicitly publish their existing Hook/Event endpoints during composition. Publications borrow
  System-owned endpoints and are cleared before System destruction.
- The L3 `scene_script_runtime` integration leaf owns the production `ScriptSystem`, decodes the existing Script data
  from `SimulationDescription`, and installs its stable-point callback through `SceneBuilder::addStablePointTask()`.
- Production order is Simulation task graph -> caller-thread ECS command flush -> dependency-ordered Scene stable
  hooks. A test dependency places Script resume before a later stable consumer and proves the consumer observes the
  resumed state.
- Scene teardown clears hooks before destroying Scene Systems; `ScriptRuntimeSystem` shuts down ScriptSystem before
  its borrowed Simulation endpoints/providers disappear.

## S2.1 NextStep

- The generated `lux.simulation.delay` Ability exposes `nextStep()` as `ASYNC_OPERATION`.
- ScriptSystem owns a reserved-capacity binary heap keyed by target step and stable insertion sequence.
- Registration targets `current step + 1`; promotion happens before resume drain, so a continuation that requests
  NextStep while stable point N is draining cannot run again until N+1.
- Stale/unmounted completions are discarded, queue exhaustion fails closed, completion backpressure retains the
  earliest waiter, and existing global/per-instance continuation and resume budgets remain authoritative.

## S2.2 Simulation delay

- `seconds()` and `simulationSeconds()` are synonymous simulation-time async methods on the same Ability.
- Durations are validated as finite/non-negative and rounded upward to nanoseconds. Deadline and sequence overflow
  fail closed. Zero duration is constrained to the next eligible step.
- A separate reserved-capacity min-heap uses `(deadline, insertion sequence)` ordering and never scans all active
  delays per frame. Equal deadlines are deterministic.
- Positive delay does not progress under zero effective delta. Exact deadline, overshoot, pause, zero-delay,
  same-deadline budget and cancellation/shutdown paths are exercised by the production Scene test.

## S2.3 real delay

- `realSeconds()` bridges through a Process-agnostic L1 `ScriptRealDelayEndpoint`; `simulation_script` does not link
  Process execution.
- The L3 real-delay provider reuses `TimerClient` and owns a concrete `TaskScope` plus bounded request records.
  Timer terminal callbacks only atomically mark records. The next Scene stable point adopts completion into the
  existing AwaitableIngress/resume queue and performs script execution.
- Timer errors remain typed Delay operation failures. TaskScope consumes all Sender terminal channels before owning
  lifetime.
- Shutdown first invalidates Script instances/awaitables and clears capability use-sites, then stops/closes timer
  tasks. Requests and completions own neither ScriptSystem nor the provider.
- Real time advances while simulation time is paused; the test observes no resume before Scene stable point and safe
  cancellation of long pending timers during Scene destruction.

## Clean tracked snapshot

An independent detached worktree at the qualified revision reported empty porcelain status, passed
`git diff --exit-code`, and passed:

```text
cmake -DLUX_SOURCE_DIR=<clean-worktree> -P cmake/ValidateTrackedSnapshot.cmake
-- Tracked snapshot is clean: 0cbd4f265bc1e9fa2e36e9d581937e4315fef7cf
```

The primary worktree's existing `.gitignore` and `WorldPartition.hpp` changes were excluded. Bellman experiment
processes and files were not touched.

## Windows RelWithDebInfo matrix

Every profile used its own clean build tree, MSVC RelWithDebInfo, `all -j 4 -k 0`, CTest inside `vcvars64`, and a
second complete build:

```text
Default Developer: 175/175 CTest passed
PLAYER:            175/175 CTest passed
TOOLCHAIN:         162/162 CTest passed
EDITOR:            188/188 CTest passed
Second build:      ninja: no work to do (all four profiles)
```

From the clean Developer tree:

```text
scene_script_runtime_test:          100 consecutive repeat-until-fail passes
simulation_script_continuation_test: 100 consecutive repeat-until-fail passes
```

The first installed-consumer attempt exposed an incorrect transitive package name. It was fixed in the qualified
revision and the full exact-revision build/test/install matrix was rerun; no earlier revision is claimed as PASS.

## Installed and external closure

- A fresh combined Developer/Toolchain/Editor prefix passed `ValidateInstalledArchitecture.cmake`.
- `ValidateSourceArchitecture.cmake` passed and enforces that only the L3 Scene integration links Process execution;
  L1 Simulation Script remains Process-free.
- Fresh relocated consumers configured, built, ran successfully and received no-work second builds:
  - `scene-script-runtime`
  - `simulation-composition`
  - `system-hook-script-binding`
  - `script-ability-codegen`
  - `process-execution`
- Installed codegen and package imports use installed paths and do not refer to the repository source tree.
- Android configure/build/CTest was intentionally not run; this checkpoint qualifies Windows RelWithDebInfo only.

## S2.4 AssetLoad STOP

Current `loadAsset<T>()` returns `std::shared_ptr<const ConcreteAsset>`. Storage providers explicitly do not own
residency/reference counting. The repository has `AssetId`, but no approved generational/residency-backed Asset
handle, lease-safe runtime token, typed handle resolution contract or owner that can keep a successfully decoded
asset resident after the load Sender completes.

Returning the input `AssetId` after dropping the decoded `shared_ptr` would falsely claim a loaded asset. Raw pointers,
pointer-shaped integers and `shared_ptr` are forbidden in the language-neutral Script ABI. Therefore no AssetLoading
Ability or compatibility seam was added.

The smallest missing architecture decision must define the stable script-visible Asset handle identity, type,
residency owner, generation/invalidation, resolution/lease semantics and shutdown behavior.

## Explicitly not done

- S2.4 AssetLoad is **not implemented**.
- S3 FlowForge coroutine lowering is **not implemented**.
- S4 Lua coroutine/yield-resume is **not implemented**.
- S5 Event.await and production Physics/Navigation Abilities are **not implemented**.
- S6 C++ coroutine ergonomics/static specialization is **not implemented**.
- Python, provider hot swap and multi-provider routing are **not implemented**.

No Time/Async/Coroutine manager, SceneServices, ServiceRegistry, ScriptSystem-owned ExecutionRuntime, direct timer
resume, unbounded delay container or provider shared ownership was introduced.

S2.0-S2.3 are qualified for later S3 time-awaitable work. AssetLoad remains blocked until the Asset handle contract is
approved.
