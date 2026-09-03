# L5 v3 Script PB0 Runtime Performance Baseline

Date: 2026-09-03

Status: **BASELINE RECORDED — this is measurement evidence, not an absolute performance gate**

Production and benchmark revision: `4a6f5072c1b44fbed110ce50d09bba9b8f0327d7`

## Environment

```text
OS:                 Microsoft Windows 11 Pro 10.0.26200 (build 26200)
CPU:                13th Gen Intel(R) Core(TM) i7-13700KF
Physical cores:     16
Logical processors: 24
Compiler:           MSVC 19.44.35228.0
Build:              RelWithDebInfo, Ninja
Seed:               1592598566
```

The benchmark CSV embeds the exact commit, build type, compiler, OS, logical CPU count, scenario parameters and
seed. Timing and allocation passes use the existing repository convention: `steady_clock` and a process-local global
allocation counter. PB0 does not pin CPU affinity, disable turbo/power management or install platform profiling code,
so sub-microsecond update-heavy measurements and maxima remain scheduler/frequency sensitive.

## Harness and scenarios

`script_runtime_benchmark` is an independent executable owned by `engine/domain/simulation/builtin/script`. It uses
production ScriptSystem, HookPoint, generated Ability binding, continuation/awaitable ingress, bounded resume queue,
NextStep heap, Simulation delay heap and Simulation-owned clock. It does not introduce a benchmark framework or a
benchmark-only production runtime.

Groups recorded twice from the same clean revision:

- `micro-sync`: direct C++ provider, generated Ability dynamic receiver/thunk, BoundScriptCall, HookPoint and Lua
  prepared call; 30 samples of 100,000 calls.
- `micro-async`: 10,000 suspensions, same-thread and cross-thread completion ingress, stable-point resume and eager
  completion/tail-queue behavior.
- `micro-lifecycle`: ScriptSystem admission/retirement plus real C++ Static, Lua and Native backend
  create/prepare/BeginPlay/steady/EndPlay/destroy phases at 2.5k, 5k, 10k and 20k objects.
- `scene-update-heavy` and `scene-gameplay-mixed`: 300 warmup + 5,000 measured frames at 2.5k, 5k, 10k and 20k
  long-lived objects.
- `scene-suspended-idle`: 300 warmup + 5,000 measured stable points with 20k and 50k suspended objects.
- `scene-resume-storm`: 50k suspended, 10k READY together, budget 2k/frame.
- `scene-object-churn`: 300 warmup + 1,000 measured frames, 100 retire/rematerialize operations per frame, at 2.5k,
  5k, 10k and 20k steady population.
- `scheduler-next-step`: 10k and 50k waits.
- `scheduler-simulation-delay`: 1k, 10k, 50k and 100k deterministic deadlines.
- `integration-real-delay`: 10k operations through a deterministic monotonic provider, cross-thread completion
  ingress and stable-point resume. The existing Process Timer integration remains covered by Scene runtime tests.

The checked-in CTest smoke runs every group with small parameters and validates lifecycle counts, checksums, queue
depth, resume budgets, idle suspension counts and capacity observations. Every measured workload produces an observed
checksum or exact count; setup and steady-state phases are separate.

Raw CSV was intentionally kept as a reproducible build artifact rather than committed in bulk:

```text
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-pb0-results-final2/run1
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-pb0-results-final2/run2
```

## Micro sync result

Nanoseconds per call; median/p95 across 30 samples:

| Path | Run 1 | Run 2 | p95 range | Timed allocations |
|---|---:|---:|---:|---:|
| direct C++ provider | 1.414 | 1.414 | 1.423–1.622 | 0 |
| generated Ability receiver + thunk | 1.400 | 1.431 | 1.470–1.759 | 0 |
| prepared BoundScriptCall | 0.742 | 0.742 | 0.838–0.878 | 0 |
| HookPoint, one handler | 0.933 | 0.937 | 1.025–1.107 | 0 |
| Lua prepared synchronous call | 22.167 | 23.247 | 24.824–24.977 | 0 |

The generated binding is statically known inside this benchmark and MSVC can optimize the constant dispatch. This
number proves that the prepared path adds no allocation or lookup; it is not claimed as a cross-DLL unpredictable
indirect-call worst case. The real C++ Static backend steady call below is about 4 ns/object and Lua about 22 ns,
roughly 5x apart in this deliberately tiny method.

## Async micro result

Nanoseconds per item:

| Phase | Run 1 | Run 2 | Timed allocations | Observation |
|---|---:|---:|---:|---|
| suspend + awaitable/continuation | 171.44 | 175.62 | 1/object | 10k suspended |
| same-thread completion ingress | 42.28 | 43.26 | 0 | 5k queued, no resume |
| cross-thread completion ingress | 138.00 | 129.38 | 2 total | 10k queued, no worker resume |
| stable-point resume/destroy | 125.45 | 145.19 | 0 | 10k resumed |
| eager complete + tail resume | 320.24 | 321.44 | 1/object | no recursive resume |

The two cross-thread allocations are `std::jthread` measurement overhead, not per completion. Synchronous
BoundScriptCall and QUERY/COMMAND Ability paths allocate no continuation.

The deterministic Real Delay integration recorded 233–247 ns/start, 88–91 ns/cross-thread completion and
126–142 ns/stable-point resume for 10k operations. Start consumed two allocations/operation in this dynamic path
(the generic completion adapter and synthetic continuation); completion and resume consumed no per-operation
allocation. Completion produced a 10k queue with zero gameplay resume until the explicit stable point. The real
Process Timer/TaskScope provider remains an integration proof in `scene_script_runtime_test`; PB0 intentionally does
not benchmark thousands of wall-clock sleeps.

## Long-lived object scene scaling

Run 2 full percentile table, milliseconds/frame:

| Scenario | Objects | median | p90 | p95 | p99 | max | mean | stddev | ns/object/frame |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| update-heavy | 2,500 | 0.0070 | 0.0074 | 0.0076 | 0.0083 | 0.2336 | 0.0072 | 0.0035 | 2.80 |
| update-heavy | 5,000 | 0.0144 | 0.0157 | 0.0164 | 0.0230 | 0.1621 | 0.0149 | 0.0043 | 2.88 |
| update-heavy | 10,000 | 0.0634 | 0.0666 | 0.0679 | 0.0887 | 0.3889 | 0.0594 | 0.0164 | 6.34 |
| update-heavy | 20,000 | 0.1106 | 0.1496 | 0.1594 | 0.2450 | 0.9045 | 0.1236 | 0.0388 | 5.53 |
| gameplay-mixed | 2,500 | 0.1100 | 0.1134 | 0.1188 | 0.1868 | 0.6230 | 0.1135 | 0.0227 | 44.00 |
| gameplay-mixed | 5,000 | 0.2265 | 0.2364 | 0.2701 | 0.4731 | 1.0394 | 0.2351 | 0.0474 | 45.30 |
| gameplay-mixed | 10,000 | 0.4624 | 0.5096 | 0.5616 | 0.9410 | 1.8492 | 0.4799 | 0.0835 | 46.24 |
| gameplay-mixed | 20,000 | 0.9632 | 1.0715 | 1.2049 | 1.6290 | 2.1211 | 0.9927 | 0.1277 | 48.16 |

Run 1 gameplay-mixed medians were 0.1116/0.2260/0.4571/0.9678 ms and p95 values were
0.1178/0.2710/0.5349/1.2425 ms. The mixed scenario is stable and near-linear. Update-heavy is close to clock/cache
noise and varied more between sizes/runs, so it should not become a tight regression
threshold without affinity/power-controlled measurement.

At 10k and 20k long-lived objects, the realistic mixed baseline is therefore about 0.46 ms and 0.96 ms median,
with 0.56 ms and 1.20 ms p95 on run 2.

## Suspended-idle and resume storm

- 20k suspended-idle: both runs median/p95/p99 = 100 ns per stable point.
- 50k suspended-idle: both runs median/p95/p99 = 100 ns per stable point.
- The largest isolated idle outlier was 40.0 microseconds. No expiry path scans all suspended instances.

50k suspended, 10k READY, budget 2k/frame:

| Frame | Run 1 stable point | Run 2 stable point | cumulative resumed | queue remaining |
|---:|---:|---:|---:|---:|
| 0 | 0.2991 ms | 0.2751 ms | 2,000 | 8,000 |
| 1 | 0.2913 ms | 0.2534 ms | 4,000 | 6,000 |
| 2 | 0.2907 ms | 0.2581 ms | 6,000 | 4,000 |
| 3 | 0.2842 ms | 0.2558 ms | 8,000 | 2,000 |
| 4 | 0.2889 ms | 0.2520 ms | 10,000 | 0 |

The burst drains in exactly five frames and never bypasses the configured resume budget.

## Delay scheduler

Idle stable-point cost stays approximately 0.005–0.012 ms at 10k–100k pending waits. Expiration is
output-sensitive: the implementation marks every now-eligible wait and enqueues it, while continuation execution is
still budgeted at 2k/frame.

| Scheduler | Eligible | Run 1 expiry | Run 2 expiry | resumed now | queued |
|---|---:|---:|---:|---:|---:|
| NextStep | 10,000 | 1.5818 ms | 1.5537 ms | 2,000 | 8,000 |
| NextStep | 50,000 | 6.8667 ms | 7.3649 ms | 2,000 | 48,000 |
| Simulation delay | 1,000 | 0.2353 ms | 0.2335 ms | 1,000 | 0 |
| Simulation delay | 10,000 | 1.5504 ms | 1.4666 ms | 2,000 | 8,000 |
| Simulation delay | 50,000 | 7.2435 ms | 7.2539 ms | 2,000 | 48,000 |
| Simulation delay | 100,000 | 15.7529 ms | 14.6614 ms | 2,000 | 98,000 |

There is no per-frame full pending-delay scan. A deliberately simultaneous 100k expiry is itself 100k units of
output and costs about 15 ms; gameplay should avoid manufacturing that burst or choose an admission/deadline policy
in a later, evidence-driven optimization wave.

## Object lifecycle and churn

20k-object phase cost, nanoseconds/object:

| Backend/owner | create + initialize | BeginPlay | steady call | EndPlay + destroy |
|---|---:|---:|---:|---:|
| C++ Static run 1 / run 2 | 68.14 / 69.78 | 4.46 / 4.60 | 3.92 / 4.61 | 16.08 / 17.74 |
| Lua run 1 / run 2 | 574.35 / 572.45 | 33.16 / 32.68 | 21.12 / 22.86 | 45.34 / 46.04 |
| Native run 1 / run 2 | 181.46 / 173.10 | 1.88 / 1.84 | 1.69 / 1.70 | 13.95 / 13.22 |
| ScriptSystem synthetic admission | 410.88 / 388.48 | included | n/a | 113.41 / 111.84 |

The C++ Static, Lua and Native backend pools allocate their capacity before the timed lifecycle phases and recorded
zero timed allocations. The synthetic ScriptSystem backend intentionally uses one object plus three prepared-call
wrappers per admitted incarnation, so its admission records four allocations/object; this is harness behavior, not
the C++ Static slab behavior.

For 100 retire/rematerialize operations per frame, the stable-point median was 0.030–0.032 ms and p95
0.031–0.033 ms across 2.5k, 5k, 10k and 20k steady populations. Attachment-signal work was about 0.006 ms. The cost
tracks the 100 changed objects rather than total population. Timed allocations are 400/frame from the synthetic
backend's four allocations/incarnation.

An early PB0 draft appeared linear in total population because the benchmark's UUID construction collided under its
local unordered-map lookup. PB0 corrected the harness to decode the deterministic object index directly, reran every
final scenario twice from `4a6f5072`, and did not change production ScriptSystem to hide the measurement bug.

## Runtime-owned capacity and memory observations

- ScriptInstanceId, ScriptContinuationId and ScriptAwaitableId remain generational `{slot,generation}` values; all
  instance/continuation/awaitable/resume/NextStep/Simulation-delay capacities are explicit in ScriptRuntimeLimits.
- PB0 exercised up to 100k awaitables/delay entries and 50k live continuations. Resume queue high-water reached the
  expected 10k in the storm, never exceeding configured capacity.
- The benchmark C++ object payload is 8 bytes; Native fixture state is 64-byte size/alignment; Lua recorded one
  independent instance table per object. No portable process-RSS facility exists in the repository, so PB0 does not
  claim OS working-set bytes or introduce an OS-specific production profiler.

## Observed bottlenecks and complexity conclusion

The top observed costs are:

1. simultaneous deadline promotion: about 15 ms for 100k eligible waits;
2. mixed gameplay dispatch/await work: about 0.96 ms median and 1.20 ms p95 at 20k objects;
3. Lua object construction: about 573 ns/object, versus about 69 ns C++ Static and 177 ns Native in this fixture.

No architecture complexity violation remains in the final data: suspended-idle does not scale with suspended count,
churn does not scale with total population, resume storms obey budget, and delay idle cost does not scan all timers.
PB0 therefore does not justify a runtime optimization wave before S3.

## Correctness qualification at the benchmark revision

Clean `4a6f5072`, Windows RelWithDebInfo:

```text
Default Developer: 177/177 CTest passed
PLAYER:            177/177 CTest passed
TOOLCHAIN:         164/164 CTest passed
EDITOR:            190/190 CTest passed on the complete rerun
Second all build:  ninja: no work to do (all four profiles)
```

At the final revision, 100 consecutive repeat-until-fail runs passed for:

```text
simulation_script_lifecycle_test
simulation_script_continuation_test
scene_script_runtime_test
script_runtime_benchmark_smoke
```

Fresh installed Developer/Toolchain/Editor surfaces passed source and installed architecture validators. Fresh
relocated `scene-script-runtime`, `simulation-composition`, `system-hook-script-binding` and
`script-ability-codegen` consumers configured, built, executed and received no-work second builds. Android was not
configured or built.

## Explicitly not done

- S2.4 AssetLoad is omitted because the script-visible residency-backed Asset handle contract remains blocked.
- S3 FlowForge coroutine lowering is **not implemented**.
- S4 Lua coroutine/yield-resume is **not implemented**.
- S5 Event.await and production Physics/Navigation Abilities are **not implemented**.
- S6 C++ coroutine ergonomics/static specialization is **not implemented**.
- Python runtime is **not implemented**.
- Temporary Script activation/deactivation lifecycle is **not implemented**.
- Async BeginPlay and async EndPlay are **not implemented**.

Recommendation: **Ready for S3 without a runtime optimization wave: YES.** This checkpoint stops before S3.
