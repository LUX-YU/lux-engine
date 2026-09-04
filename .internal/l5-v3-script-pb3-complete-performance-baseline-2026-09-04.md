# L5 v3 Script PB3 complete gameplay performance baseline

Date: 2026-09-04

## Scope and reproducibility

This document completes PB3 by combining the already recorded Event/language measurements with a new production
Physics-inclusive C++ Static / FlowForge / Lua / Event.await / Delay composition.

- Final Physics-inclusive measurement revision:
  `feb19bfb7e3558e8efd85609e83bc143f33a460c`.
- Event/language measurement revision retained from the earlier subset:
  `c9e32413e5c1c19c1d8519df07350f6416523d80`.
- Build: Windows RelWithDebInfo, MSVC 19.44.35228.0.
- CPU: Intel Core i7-13700KF, 16 physical cores, 24 logical processors.
- Seed: `1592598566` (`0x5EED2026`).
- LuaJIT: 2.1.1771261233, JIT-on and interpreter-only policies.
- PUC Lua: 5.4.8.
- Scene measurements: 300 warmup frames and 5,000 measured frames per run.
- Every Physics-inclusive size/VM combination was run twice; tables aggregate all 10,000 frame samples rather than
  selecting the faster run.
- Raw Physics-inclusive CSV directory:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/s5f-pb3-final/`.
- Focused PB0/PB1/PB2 regression CSV directory:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/s5f-pb3/regression/`.
- Event/language subset CSV directory:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-s5-pb3/`.

The benchmark validates active counts, Physics query counts, Script call counts, lifecycle/checksum observations,
Event occurrences, capacity/budget invariants, and final object state. A mismatch exits non-zero.

## Production Physics Ability boundary

The query is the real `Physics2DSystem::overlapsBox()` implementation over the private Box2D world. Each sample
contains 100,000 queries; 60 samples combine two independent runs.

| Path | Mean ms / 100k | Mean ns/query | p50 ms | p95 | p99 | max | C++ allocations |
|---|---:|---:|---:|---:|---:|---:|---:|
| Direct domain provider | 3.686 | 36.86 | 3.65 | 3.83 | 3.94 | 4.06 | 0 |
| Prepared Script Ability | 3.743 | 37.43 | 3.70 | 3.96 | 4.07 | 4.13 | 0 |

The prepared dynamic receiver/thunk boundary adds about 0.57 ns/query, or 1.5% of this very small real domain
operation. It performs no per-call provider discovery, Contract/Method string lookup, continuation allocation, or
heap allocation.

## Complete mixed gameplay scenario

One Simulation owns the real Physics2D provider and one ScriptSystem owns all Script runtime state. Population is
split as evenly as possible:

- C++ Static objects: long-lived entity object state and normal Hook callbacks;
- FlowForge AOT objects: Physics QUERY -> broadcast Event.await -> Delay.nextStep;
- portable Lua objects: the same Physics QUERY -> Event.await -> NextStep semantics.

Each frame executes the Simulation step, invokes the Hook, records and drains one broadcast Event occurrence, and
runs one Script stable point. Event dispatch only marks READY. FlowForge/Lua resume at the stable point, re-suspend
on NextStep, and complete after the following Simulation step. The reported timing includes the complete sequence.

### LuaJIT JIT-on

| Objects | p50 ms | p90 | p95 | p99 | max | mean | p50 ns/object |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2,500 | 1.1613 | 1.5626 | 1.6067 | 1.7497 | 2.7962 | 1.0236 | 464.5 |
| 5,000 | 2.4221 | 3.2639 | 3.3514 | 3.5389 | 4.6714 | 2.1118 | 484.4 |
| 10,000 | 5.2645 | 6.8215 | 8.3886 | 9.4095 | 13.8215 | 4.5196 | 526.4 |
| 20,000 | 9.6508 | 14.3316 | 14.7760 | 18.9911 | 22.2104 | 9.4058 | 482.5 |

### LuaJIT interpreter-only

| Objects | p50 ms | p90 | p95 | p99 | max | mean | p50 ns/object |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2,500 | 1.3672 | 1.5869 | 2.0325 | 2.3317 | 2.8720 | 1.0651 | 546.9 |
| 5,000 | 2.2563 | 3.2601 | 3.3420 | 3.5548 | 4.3423 | 2.1009 | 451.3 |
| 10,000 | 4.6734 | 6.8322 | 7.0125 | 7.3343 | 18.4966 | 4.4117 | 467.3 |
| 20,000 | 9.7105 | 14.2336 | 14.5934 | 18.4656 | 22.6366 | 9.3226 | 485.5 |

### Lua 5.4

| Objects | p50 ms | p90 | p95 | p99 | max | mean | p50 ns/object |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 2,500 | 2.4646 | 3.6666 | 3.8915 | 4.8358 | 6.2644 | 2.0298 | 985.8 |
| 5,000 | 4.1934 | 7.0876 | 7.3055 | 7.7771 | 9.3544 | 4.0039 | 838.7 |
| 10,000 | 9.2828 | 14.5998 | 15.1963 | 20.0717 | 30.4294 | 8.3982 | 928.3 |
| 20,000 | 18.0100 | 29.8776 | 31.0564 | 40.8830 | 64.6536 | 17.4285 | 900.5 |

At 20,000 objects the measured state is identical in every VM configuration:

```text
active ScriptInstances:       20,000
maximum continuations:        13,334
maximum NextStep waits:       13,334
resume queue high-water:      13,334
Physics queries / 5,000 run:  33,335,000
Hook targets / 5,000 run:     100,000,000
C++ object checksum:          93,340,665,000
measured C++ allocations:     0 per frame
```

Event waiters are claimed and removed before the post-stable-point row is sampled, so `active_event_waiters` is zero
in those rows; the dedicated Event measurements below instrument registration, dispatch visits, payload bytes, and
queue behavior directly.

The two-stage Event/NextStep workload is intentionally bimodal, so p50 can sit near the boundary between the heavy
Event-resume frame and the lighter NextStep-completion frame. The two-run 20k p50 difference was 2.20% for LuaJIT
JIT-on, 2.21% for LuaJIT interpreter-only, and 8.08% for Lua 5.4. No run was discarded.

## Event waiter and payload baseline

The dedicated Event measurement remains valid because the final closure did not change S5.0 waiter storage or
dispatch. For 10,000 waiters:

| Operation | Total ms | ns/waiter |
|---|---:|---:|
| Register | 3.1501 | 315.0 |
| Deliver + owned i32 copy + enqueue | 1.6922 | 169.2 |
| Stable-point resume | 1.5341 | 153.4 |
| Instance retirement/cancel | 2.6124 | 261.2 |

Delivery copies 40,000 payload bytes, visits exactly 10,000 matching waiters, and creates one bounded queue
high-water of 10,000. Callback + waiter coexistence and nested dispatch are included in the benchmark correctness
suite; neither creates per-waiter EventPoint connections.

## Idle, sparse, and storm complexity

| Idle waiters | p50 ms/stable point | p95 | p99 | dispatch visits over 5,000 frames |
|---:|---:|---:|---:|---:|
| 10,000 | 0.0001 | 0.0001 | 0.0001 | 0 |
| 50,000 | 0.0001 | 0.0001 | 0.0001 | 0 |
| 100,000 | 0.0001 | 0.0001 | 0.0001 | 0 |

For 100,000 targeted waiters with 10,000 matching routes, delivery visits exactly 10,000 waiters, copies 40,000
bytes, and leaves the other 90,000 untouched. The final focused rerun again recorded zero visits for 100,000 idle
Event waiters and zero visits for 100,000 idle generic continuations.

For 10,000 READY waiters and `resumes_per_stable_point = 2,000`, dispatch performs no resume and the queue drains in
exactly five stable points. Generic resume frames are about 0.30 ms each; the production FlowForge Event storm was
0.35-0.42 ms/frame. The ready queue never grows beyond the configured bound.

## FlowForge and portable Lua Event projection

Dedicated Event-await scene results from the S5.1 production paths remain:

| Frontend / VM | Objects | p50 ms | p95 | p99 |
|---|---:|---:|---:|---:|
| FlowForge AOT | 20,000 | 1.3525 | 1.4672 | 1.5741 |
| LuaJIT JIT-on | 20,000 | 16.2886 | 24.7975 | 26.9404 |
| LuaJIT interpreter | 20,000 | 16.6982 | 25.2068 | 27.2806 |
| Lua 5.4 | 20,000 | 45.3647 | 52.9523 | 61.5032 |

FlowForge suspended-idle at 20,000 remains at the 0.0001 ms timer floor with no continuation-frame scan. The Lua
gap is VM/language cost, not a semantic or complexity failure.

## PB0/PB1/PB2 focused regression

The final revision reran and validated the existing representative groups:

- PB0 gameplay-mixed 20k: p50 1.00 ms, p95 1.14 ms;
- PB0 suspended-idle 100k: timer-floor p50/p95 with zero Event visits and zero allocations;
- PB1 FlowForge update-heavy 20k: p50 0.23 ms, p95 0.30 ms;
- FlowForge suspend/resume diagnostic groups;
- Lua update, Ability, coroutine, and Event groups for JIT-on, interpreter-only, and Lua 5.4.

All deterministic checksums, counts, capacities, and resume-budget assertions passed. No generic Script runtime
regression is attributed to the optional Physics2D source package.

## Observed costs and verdict

The three largest measured costs are:

1. Lua 5.4 coroutine/Event/NextStep gameplay at 20k: 17.43 ms mean and 31.06 ms p95;
2. LuaJIT coroutine/Event/NextStep gameplay at 20k: about 9.3-9.4 ms mean and 14.6-14.8 ms p95;
3. high fan-out Event payload delivery plus stable-point resume: about 323 ns/waiter combined before backend work.

The real Physics prepared boundary is not a dominant cost: it adds roughly 0.57 ns/query. Scaling is near-linear,
idle suspended work is not scanned, route delivery is output-sensitive, retirement follows per-instance ownership,
and all resume work remains budgeted. No architecture complexity violation was observed.

PB3 complete gameplay baseline: **RECORDED**. This is a baseline, not an absolute performance acceptance threshold.
