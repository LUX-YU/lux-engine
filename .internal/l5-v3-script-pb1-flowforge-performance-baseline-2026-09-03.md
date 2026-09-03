# L5 v3 Script PB1 FlowForge Performance Baseline — 2026-09-03

## Reproducibility

- Exact benchmark SHA: `2ef5d62a6dea4fd7102c47d09ac9ee2e2addde1c`.
- Build: Windows RelWithDebInfo, MSVC 19.44.35228, x64.
- Machine: Intel Core i7-13700KF, 16 physical / 24 logical cores, Windows 11 Pro 10.0.26200.
- Seed: `1592598566` (`0x5EED2026`).
- Scene runs: 300 warm-up frames plus 5,000 measured frames per run; every final scenario was run twice.
- Raw CSV: build artifact under `lux-engine-s3-q/pb1/round5` and `round6`; raw data is not committed.
- PB0 regression CSV: `pb0-regression-round3` and `pb0-regression-round4` from the same SHA.

The executable compiles the FlowGraph once during setup, loads the produced `NativeModuleScript`, creates real
NativeScriptBackend instances, mounts them through ScriptSystem, then measures only runtime frames/stable points.
Compiler/link time is excluded. Every ScriptInstance has its own native state block; the sync and mixed graphs update
that state across frames. The benchmark does not use the JIT helper or a handwritten function pretending to be
FlowForge.

## Micro results

| Scenario | Scale | Run 1 p50 | Run 2 p50 | Approximate unit cost | Frame bytes |
|---|---:|---:|---:|---:|---:|
| Ability QUERY through FlowForge AOT + ScriptSystem | 10k calls | 0.0651 ms | 0.0688 ms | 6.5–6.9 ns/call | 0 |
| Sync state update + COMMAND + QUERY | 10k objects | 0.1032 ms | 0.1009 ms | 10.1–10.3 ns/object | 0 |
| Suspend (eager completion, then NextStep) | 10k objects | 2.8063 ms | 3.0884 ms | 281–309 ns/suspension | 40 |
| Steady resume/destroy batch | 2k/frame | 0.259–0.283 ms | 0.279–0.291 ms | about 129–146 ns/resume | 40 |

The first 10k-ready resume frame also promotes/enqueues all eligible waits and costs 1.90–2.04 ms; later budgeted
2k resume frames are the steady values above. Sync paths allocate no continuation or awaitable. Suspending paths pay
for the existing awaitable/completion adapters plus one bounded native continuation frame; resume does not allocate a
new frame.

The compiled sync fixture is 3,584 bytes and the async fixture is 4,608 bytes on this toolchain.

## Scene scaling

Combined two-run percentiles, milliseconds/frame:

| Scenario | Objects | p50 | p90 | p95 | p99 | max | ns/object at p50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| update-heavy | 2,500 | 0.0243 | 0.0279 | 0.0284 | 0.0355 | 0.3043 | 9.72 |
| update-heavy | 5,000 | 0.0503 | 0.0598 | 0.0612 | 0.0964 | 0.8520 | 10.06 |
| update-heavy | 10,000 | 0.0989 | 0.1231 | 0.1265 | 0.2028 | 1.1299 | 9.89 |
| update-heavy | 20,000 | 0.2413 | 0.2556 | 0.2885 | 0.5534 | 1.4209 | 12.07 |
| async-heavy mixed | 2,500 | 0.8681 | 0.9362 | 1.0273 | 1.4355 | 2.2604 | 347.2 |
| async-heavy mixed | 5,000 | 0.8194 | 0.9658 | 1.0642 | 1.5188 | 2.5158 | 163.9 |
| async-heavy mixed | 10,000 | 0.8576 | 1.0667 | 1.1355 | 1.6125 | 2.6041 | 85.8 |
| async-heavy mixed | 20,000 | 0.9381 | 1.1309 | 1.2391 | 1.6892 | 2.5988 | 46.9 |

The update-heavy curve is near-linear. Relative to PB0's 20k synthetic update p50 of 0.1106 ms, the complete
FlowForge AOT graph adds about 0.131 ms, or 6.5 ns/object/frame, for native instance dispatch, state update and two
prepared Ability calls.

The mixed fixture deliberately applies eager completion + NextStep + state/COMMAND work to every instance; it is an
async-pressure profile, not PB0's six-cohort percentage mix. Its fixed 2k resume budget caps actual continuation work
per frame, so ns/total-object is not a linear per-object metric. PB0's original gameplay-mixed scenario was therefore
rerun separately for regression comparison.

## Suspended idle

| Suspended FlowForge invocations | p50 | p95 | p99 | maximum |
|---:|---:|---:|---:|---:|
| 10,000 | 0.0001 ms | 0.0001 ms | 0.0001 ms | 0.0012 ms |
| 20,000 | 0.0001 ms | 0.0001 ms | 0.0001 ms | 0.0633 ms |

The idle stable point does not scan continuation frames. This preserves PB0's output-sensitive scheduler result.

## Resume storm

50,000 suspended/READY FlowForge invocations, budget 2,000/frame:

| Phase | Run 1 | Run 2 | Observation |
|---|---:|---:|---|
| Promote 50k READY + first 2k resumes | 9.41 ms | 9.49 ms | queue high-water 50k |
| Later 2k resume frame, typical | 0.27–0.42 ms | 0.27–0.42 ms | no frame allocation |
| Frames to drain | 25 | 25 | exactly budget-constrained |

The first-frame spike is dominated by 50k units of real output (ready transition + enqueue), not a scan of idle
continuations. Queue depth then falls 48k, 46k, ... to zero while exactly 2k generated invocations finish per frame.

## PB0 regression at the S3 SHA

| Existing PB0 group | Scale | p50 | p95 | PB0 reference | Result |
|---|---:|---:|---:|---:|---|
| gameplay-mixed | 20k | 0.9591 ms | 1.1831 ms | 0.9632 / 1.2049 ms | stable |
| suspended-idle | 50k | 0.0001 ms | 0.0001 ms | 0.0001 / 0.0001 ms | stable |
| resume storm | 50k suspended, 10k ready | 0.2575 ms | 0.2685 ms | 0.252–0.299 ms/frame | stable |
| simulation-delay expiry | 100k eligible | 14.84–14.96 ms | n/a | 14.66–15.75 ms | stable |
| object churn stable point | 20k/100 churn | 0.0310 ms | 0.0328 ms | 0.030–0.033 ms | stable |

## Bottleneck conclusion

The three largest observed costs are:

1. promoting a deliberately simultaneous 50k ready burst (about 9.5 ms before budgeted draining);
2. async-heavy mixed stable-point work (20k total objects, p50 0.94 ms / p95 1.24 ms);
3. suspension admission and completion adapters (about 281–309 ns/invocation).

No architecture complexity violation was observed: sync scaling is near-linear, idle suspended cost is flat,
continuations are not scanned, the storm obeys the resume budget, and the PB0 delay/churn/runtime groups do not
regress. PB1 records a baseline; it does not establish an absolute shipping performance threshold.

## Scope not measured/implemented

- AssetLoad is omitted because S2.4 remains blocked by the Asset handle contract.
- There is no FlowForge Event.await, Lua coroutine, C++ coroutine or shipping static-specialization benchmark.
- Compile/link time and artifact size are recorded separately from frame timings.
- No OS-specific RSS instrumentation was added to production code.

