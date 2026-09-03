# L5 v3 Script PB2 Lua Performance Baseline — 2026-09-03

## Reproducibility

- Production revision: `b3ddc0af0b6b393d40b925860db961f5f8b8fede`.
- Benchmark query-isolation closure: `658004517267e2712d95cc91e167282727a44fed`; this test-only commit separates
  the QUERY micro from the mixed QUERY + COMMAND scene method and does not change production runtime code.
- Clean detached source: `E:/SyncForder/CodeRepos/lux-engine-s4-qualification`.
- Raw CSV: `E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-s4q-pb2/final/{run1,run2}`.
- OS: Windows 11 Pro 10.0.26200.
- CPU: Intel Core i7-13700KF, 16 physical / 24 logical cores.
- Compiler/build: MSVC 19.44.35228, x64 RelWithDebInfo.
- Lua: LuaJIT 2.1.1771261233, JIT enabled (`SSE3`, `SSE4.1`, `BMI2` and normal optimization passes).
- Seed: `1592598566` (`0x5EED2026`).
- Scene runs: 300 warmup frames + 5,000 measured frames per size, repeated twice.

The benchmark consumes a production-packager-generated LXSA, constructs real independent Lua instance tables, and
runs through LuaScriptBackend and ScriptSystem. It does not parse source directly or hard-code a ScriptArtifact in
the benchmark. The process-local C++ allocation counter does not observe LuaJIT's internal allocator; zero timed C++
allocations must not be read as zero Lua VM allocation.

## Micro results

| Path/phase | Run 1 | Run 2 | Approximate cost |
|---|---:|---:|---:|
| Lua prepared synchronous object call, 10k | 0.34 ms median | 0.34 ms median | 34 ns/call |
| Lua prepared QUERY only, 10k | 0.814 ms median | 0.818 ms median | 81–82 ns/query |
| Lua coroutine start + NextStep suspend, 10k | 6.94 ms | 6.83 ms | 683–694 ns/suspension |
| Lua coroutine stable-point resume + destroy, 10k | 3.83 ms | 3.39 ms | 339–383 ns/resume |

PB0's equivalent tiny C++ Static steady call was about 4 ns/object, and PB1's FlowForge AOT update with two prepared
Ability calls was about 10–12 ns/object. Lua's plain prepared call is therefore roughly 8x the C++ Static method and
about 3x the complete FlowForge update fixture. The isolated Lua QUERY adds about 47–48 ns/call over the plain Lua
update, including Lua table/C-call/marshalling overhead. The scene Ability workload adds about 81 ns/object over the
plain update for one QUERY plus one COMMAND; the generated C++ thunk alone remains the roughly 1.4 ns PB0 path.

## Scene scaling

Combined two-run percentiles, milliseconds/frame:

| Scenario | Objects | p50 | p90 | p95 | p99 | max | ns/object at p50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Lua update-heavy | 2,500 | 0.0805 | 0.084 | 0.089 | 0.14 | 0.48 | 32.20 |
| Lua update-heavy | 5,000 | 0.1673 | 0.17 | 0.20 | 0.26 | 0.65 | 33.46 |
| Lua update-heavy | 10,000 | 0.3355 | 0.36 | 0.39 | 0.50 | 1.16 | 33.55 |
| Lua update-heavy | 20,000 | 0.7106 | 0.85 | 0.95 | 1.23 | 1.64 | 35.53 |
| Lua QUERY + COMMAND | 2,500 | 0.2816 | 0.30 | 0.33 | 0.47 | 1.19 | 112.64 |
| Lua QUERY + COMMAND | 5,000 | 0.5693 | 0.61 | 0.66 | 0.88 | 1.58 | 113.86 |
| Lua QUERY + COMMAND | 10,000 | 1.1503 | 1.26 | 1.34 | 1.55 | 2.28 | 115.03 |
| Lua QUERY + COMMAND | 20,000 | 2.3644 | 2.64 | 2.78 | 3.09 | 3.92 | 118.22 |
| Lua coroutine each frame | 2,500 | 1.7166 | 2.12 | 2.24 | 2.48 | 3.62 | 686.64 |
| Lua coroutine each frame | 5,000 | 3.5629 | 4.64 | 4.92 | 5.34 | 6.84 | 712.58 |
| Lua coroutine each frame | 10,000 | 7.4005 | 10.28 | 10.88 | 11.72 | 13.66 | 740.05 |
| Lua coroutine each frame | 20,000 | 16.6632 | 23.42 | 24.65 | 26.63 | 29.35 | 833.16 |

The synchronous curves are near-linear. The coroutine workload deliberately starts, suspends, resumes, and destroys
one LuaJIT thread per object per frame. At 20k it exceeds a 60 Hz frame budget at the median and is the dominant PB2
cost. This does not imply that 20k idle or intermittently awaiting Lua objects are expensive; it quantifies an
extreme all-objects-coroutine-every-frame policy.

## Suspended idle and resume storm

- 10k and 20k genuinely suspended Lua invocations: both runs p50/p95 = 100 ns per stable point.
- Maximum idle outlier: 5.1 microseconds.
- No idle continuation scan was observed.

20k READY Lua invocations, resume budget 2k/frame:

| Frame | Run 1 | Run 2 | Remaining queue after frame |
|---:|---:|---:|---:|
| 0 | 4.178 ms | 4.321 ms | 18,000 |
| 1 | 0.583 ms | 0.561 ms | 16,000 |
| 2 | 0.488 ms | 0.485 ms | 14,000 |
| 3 | 0.499 ms | 0.495 ms | 12,000 |
| 4 | 0.574 ms | 0.511 ms | 10,000 |
| 5 | 0.561 ms | 0.553 ms | 8,000 |
| 6 | 0.563 ms | 0.556 ms | 6,000 |
| 7 | 0.532 ms | 0.552 ms | 4,000 |
| 8 | 0.534 ms | 0.542 ms | 2,000 |
| 9 | 0.530 ms | 0.529 ms | 0 |

The first frame performs 20k real ready/enqueue transitions plus 2k resumes. Later frames execute exactly 2k
resumes and the burst drains in exactly ten frames. Lua does not bypass ScriptSystem backpressure.

## Lifecycle and churn

20k object phase cost, nanoseconds/object:

| Phase | Run 1 | Run 2 |
|---|---:|---:|
| create instance table + prepare | 649.26 | 640.27 |
| BeginPlay | 50.48 | 50.06 |
| steady call | 33.12 | 34.10 |
| EndPlay + destroy | 105.60 | 108.85 |

At 100 retire/rematerialize operations per frame:

| Steady population | p50 range | p95 range | p99 range |
|---:|---:|---:|---:|
| 10k | 0.121–0.122 ms | 0.325–0.338 ms | 0.592–0.617 ms |
| 20k | 0.127 ms | 0.338–0.413 ms | 0.685–0.739 ms |

The p50 follows the 100 changed objects rather than total population. PB2 found and fixed an initial O(N^2)
continuation-pool scan in instance teardown; the final numbers above are from the corrected SHA.

## Capacity and memory observations

Lua backend C++ bookkeeping is fully bounded: instance records, prepared calls, continuation records, execution
depth, and dense per-instance Ability method entries all have explicit creation capacities. For the benchmark's six
Ability methods, the dense prepared receiver/thunk table is approximately 144 bytes/instance on x64; the private
continuation record is approximately one 64-byte-class slot per admitted invocation. These are structural estimates,
not process RSS measurements. LuaJIT additionally owns one table per ScriptInstance and one thread/registry reference
per suspended invocation. The repository has no portable Lua allocator-byte/RSS probe, so PB2 does not claim exact VM
bytes and does not add OS-specific production profiling code.

## PB0/PB1 focused regression

The final S4 SHA preserved prior runtime paths:

| Existing group | S4 result | Historical baseline |
|---|---:|---:|
| PB0 gameplay-mixed, 20k | p50 0.97 ms / p95 1.12 ms | 0.96 / 1.20 ms |
| PB0 suspended-idle, 50k | p50/p95 0.0001 ms | 0.0001 / 0.0001 ms |
| PB0 resume storm, 10k ready / 2k budget | 0.26 ms typical | 0.25–0.30 ms |
| PB0 Simulation Delay, 100k eligible | 15.13 ms expiry | 14.66–15.75 ms |
| PB0 object churn, 20k / 100 changes | p50/p95 0.030/0.030 ms | 0.030–0.033 ms |
| PB1 FlowForge update, 20k | p50 0.25 ms / p95 0.30 ms | 0.24 / 0.29 ms |
| PB1 FlowForge suspend, 10k | 3.12 ms | 2.81–3.09 ms |
| PB1 FlowForge steady resume, 2k/frame | 0.27–0.28 ms | 0.27–0.42 ms |

No generic ScriptSystem, FlowForge, delay scheduler, continuation, or lifecycle regression was observed.

## Bottleneck conclusion

The top measured costs are:

1. per-frame mass LuaJIT coroutine creation/yield/resume/destruction (20k p50 16.66 ms, p95 24.65 ms);
2. Lua language-to-prepared-Ability crossings (20k QUERY + COMMAND p50 2.36 ms versus 0.71 ms plain update);
3. simultaneous readiness promotion (20k ready burst first frame 4.18–4.32 ms before budgeted draining).

After the teardown fix, no architecture complexity violation remains: synchronous scaling is near-linear, idle
suspended cost is flat, churn tracks changed objects, and resume storms obey the budget. PB2 records a baseline; it
does not establish an absolute shipping threshold. S5 may proceed without a prerequisite optimization wave, while
gameplay policy should avoid starting a coroutine on every one of 20k Lua objects every frame.

## Explicitly not measured/implemented

- S2.4 AssetLoad is omitted because the Asset handle contract remains blocked.
- S5 Event.await and Physics/Navigation production Abilities are not implemented.
- S6 C++ coroutine ergonomics/static specialization is not implemented.
- Python runtime is not implemented.
- LuaJIT internal allocator bytes and process RSS are not reported.
