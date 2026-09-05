# S6 safety / TaskGraph / HookChannel performance evidence — 2026-09-05

Status: **baseline recorded; qualification candidate awaiting independent review**. No absolute frame-time gate or
framework-freeze declaration is made.

## Reproduction and measurement policy

- B0: `8354ae10e5d247cbc69746ae1f79c97ddfdd5ab9`, safe serial execution only.
- B1: `da5e29b65f293f68c6c42231ce576cfa85618d2f`, G0 provenance/external-admission closure.
- B2: `5f03e9b156421583ae81857025ec6156ad0e0f05`, final implementation/test snapshot.
- Windows 11 Pro 10.0.26200; Intel Core i7-13700KF, 16 cores / 24 logical processors; MSVC 19.44.35228.0,
  RelWithDebInfo; LuaJIT 2.1.1771261233, JIT-on for paired measurements.
- Raw data: `E:/SyncForder/CodeRepos/build/RelWithDebInfo/hook-closure-performance/`.
- Reproduction: `.internal/Run-HookClosurePerformance.ps1`, `.internal/Summarize-HookClosurePerformance.ps1`.
- Five independent processes per point/scenario, counterbalanced B0/B1/B2 order, seed `1592598566`.
  Each has 100 effective warmup frames and 1,000 retained measured frames. For older runtime executables without
  a warmup argument, 5 natural warmups + the first 95 discarded CSV rows provide that exact effective warmup.
  Physics mixed uses its explicit `--warmups 100`. Frames within a process are not treated as independent repetitions.
- Scalar Engine allocation instrumentation follows the existing benchmark convention. Lua VM allocation tracing
  is a separate diagnostic run and its times do not enter the paired baseline. No build/test runs overlap sampling.
- Process priority/CPU affinity/power policy were not changed. Scheduling/thermal/VM state noise remains possible;
  all rounds are retained rather than selecting the fastest result.

## Paired results

Values are the median of five process p50s / median of five process p95s, milliseconds.

| Workload | B0 | B1 | B2 |
|---|---:|---:|---:|
| Lua update, 10k | 0.276 / 0.300 | 0.277 / 0.308 | 0.264 / 0.302 |
| Lua Ability, 10k | 0.866 / 0.949 | 1.028 / 1.146 | 1.016 / 1.138 |
| Lua coroutine, 10k | 1.013 / 2.340 | 0.995 / 2.169 | 1.044 / 2.234 |
| Lua Event, 10k | 8.556 / 14.297 | 8.888 / 14.696 | 8.798 / 14.762 |
| Real Physics mixed, 10k | 2.758 / 4.303 | 3.127 / 7.279 | 3.104 / 7.411 |
| Real Physics mixed, 20k | 7.216 / 15.347 | 6.872 / 16.303 | 8.063 / 16.761 |

Every paired row reports zero Engine C++ allocations. Final Physics query counts and C++ checksums are identical
across points/rounds: 10k has 3,750,000 queries / checksum 2,252,500,000; 20k has 7,500,000 queries /
checksum 4,505,000,000. Active populations and the Lua observations also match. B2 records actual backend
sync/step calls, resume calls and Event occurrences; older nominal `script_calls` must not be compared as if they
were those actual counters.

The main repeatable Lua Ability increment occurs at G0 (about 16 ns/object/frame), not at graph integration.
G0's status/yield wrappers and provenance validation are required safety behavior. Mixed 10k p95 increases
substantially at G0. Mixed 20k process p50 is noisy: B0 spans 5.920–7.468 ms, B2 6.557–8.241 ms. This is recorded as
a cost/noise investigation point, not hidden as “no regression” and not attributed conclusively to GC or scheduling.

## Other backends, domain and worker comparison

Five final FlowForge AOT processes, 10k objects, 100 warmup / 1,000 measured frames:

| FlowForge workload | process p50 range | process p95 range |
|---|---:|---:|
| update-heavy | 0.064–0.068 ms | 0.082–0.095 ms |
| typed Ability QUERY | 0.052–0.063 ms | 0.068–0.087 ms |
| gameplay mixed | 0.634–0.645 ms | 0.832–0.866 ms |
| Event await | 0.882–0.914 ms | 0.985–1.012 ms |

FlowForge mixed at 20k spans p50 0.714–0.761 ms and p95 0.930–0.964 ms. The fixed 2k resume budget makes this a
budgeted throughput workload, not a claim that doubling all active work is free. All report zero Engine allocations.

At 10k queries, Physics direct / prepared / static p50 is 36.48 / 36.96 / 36.48 ns/query (30 samples).
The real domain implementation remains the main cost; no fake Physics provider substitutes for it.

The one-time 100k C++ coroutine observation records 8.626 ms start and 5.384 ms resume+destroy, zero Engine
allocations. These are aggregate operations, not statistical latency percentiles from one sample.

Final 20k mixed worker comparison (five processes each): worker-0 p50 spans 6.587–8.564 ms; worker-4 spans
6.563–7.826 ms. Their median process p50 is 6.767 versus 6.845 ms; median p95 is 16.633 versus 16.806 ms. The
no-Script/empty-Script-Hook frame p50 is 0.4 microseconds with 0 workers, 1.1 microseconds with 4 workers.
The graph remains three tasks / two dependencies in this concrete composition.

The separate 4-worker 10k mixed timing probe records:

| Interval | p50 | p95 |
|---|---:|---:|
| frame start → Hook entry | 11.45 us | 91.23 us |
| incoming lifecycle | 0.10 us | 2.80 us |
| direct dispatch + Channel delivery | 1.494 ms | 5.664 ms |
| bounded resume | 1.395 ms | 1.858 ms |
| structural commit gap | 0.10 us | 0.20 us |
| outgoing lifecycle | 0.10 us | 0.50 us |
| Hook end → execute return | 0.70 us | 1.40 us |

The pre-Hook interval includes native producer work and scheduling, not pure barrier wait. The post interval
includes reset/handoff/return. Do not label either as an isolated mutex/barrier cost. Trace timings are diagnostic,
not included in the uninstrumented paired table.

Typed Channel micro at 10k records: append p50 53.9 us, seal+native scan 1.9 us, reset below the timer's useful
resolution, all zero Engine allocations. Script payload ownership copy is additionally exercised by Event delivery.

## Complexity and budget observations

- 10k/50k/100k idle waiters: zero dispatch visits; p50 at/below 0.1 us, zero Engine allocations. Structural tests
  independently assert unchanged visit counters across stable points.
- 100k population, 10k targeted deliveries: exactly 10k visits, 3.615 ms aggregate delivery, zero allocations.
- 100k fan-out delivery: 10.170 ms once; 2k budget produces 50 drains, p50 drain 0.241 ms. No direct event resume.
- 100k suspended / 10k READY / 2k budget external storm:

| drain | cumulative resumes | queue remaining | CPU ms |
|---:|---:|---:|---:|
| 1 | 2,000 | 8,000 | 0.6672 |
| 2 | 4,000 | 6,000 | 0.2393 |
| 3 | 6,000 | 4,000 | 0.2360 |
| 4 | 8,000 | 2,000 | 0.2353 |
| 5 | 10,000 | 0 | 0.2326 |

- One-instance retirement in the 100k structural fixture visits exactly one owned waiter, Awaitable and continuation.
- NextStep is the existing generation FIFO; the older CSV string `bounded-heap` in its shared scheduler micro is
  a historical label, not its implementation. Simulation Delay retains the deadline heap. No idle-population scan,
  per-waiter connection or unbounded ready queue is introduced.

## LuaJIT allocator and profiler evidence

The separate 10k Event diagnostic interval (99 consecutive measured-frame deltas) reports 990,000 coroutine
creations, 990,000 resumes and 990,000 registry releases; 1,980,000 VM allocation calls and 1,960,540 frees;
475,200,000 requested and 470,529,600 released logical allocator bytes. This is approximately 4.8 MB requested per
frame / 480 logical bytes per coroutine in this fixture, not RSS or OS allocation traffic. Reallocations account for
the old/new logical request sizes. VM startup before installing the tracker is outside these counters.

Frames with VM free activity average 8.719 ms (29 frames); those without average 6.062 ms (70 frames). This supports
a correlation between VM memory work and tails; it does not establish full causation or explain Lua 5.4. Lua 5.4
was not built/tested/sampled in this wave. JIT-off focused final measurements remain semantically identical; its
10k Ability / coroutine / Event p50 is 1.049 / 1.020 / 8.710 ms, zero Engine allocations.

VTune 2026.3.0 software sampling is under `.../hook-closure-performance/vtune/`. Normal timing above comes from
non-sampled runs. Hardware counters were not enabled; no cache/TLB/branch-miss claim is made.

- Lua QUERY: `LuaAbilityProjectionAccess::current` is 16.93% of sampled CPU (B0 23.13%); absolute sample CPU and
  total runtime differ, so this percentage reduction is not a speedup claim. No Lua recursive mutex or execution
  vector/scan appears in the hot path.
- FlowForge QUERY: generated AOT code, ScriptSystem invoke and Native prepared invoke lead the gameplay samples.
  The short profile also includes `CryptAcquireContextW` startup (~28%); it is not gameplay dispatch overhead.
  No erased `NativeScriptBackend::invokeAbility` path appears.
- C++ Physics: `Physics2DSystem::overlapsBox`, Box2D overlap/dynamic-tree query and validation dominate.
- 4-worker mixed: ScriptSystem resume/NextStep/identity/owner completion and VM work dominate. Task executor
  samples total about 0.040 s of 3.239 s sampled CPU; CppStatic, Native and Lua adapters are all present.

The main remaining observed costs are Lua VM coroutine/memory work, safe Lua prepared-entry validation/wrappers,
and generic continuation/resume bookkeeping. They are not justification for another scheduler/backend or a
VM-specific coroutine pool. No further optimization is performed in this wave.
