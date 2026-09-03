# L5 v3 Script PB2-P Lua Portability Performance Baseline — 2026-09-03

## Reproducibility

- Exact production/benchmark revision: `b42e976dbcd3bc1536337ab04c217b67852aede9`.
- Source: clean detached worktree `E:/SyncForder/CodeRepos/lux-engine-s4p-qualification`.
- OS/CPU: Windows 11 Pro 10.0.26200, Intel Core i7-13700KF, 16 physical / 24 logical cores.
- Compiler/build: MSVC 19.44.35228, x64 RelWithDebInfo.
- VM configurations: LuaJIT 2.1.1771261233 JIT-on, the same LuaJIT build with interpreter-only policy, and Lua
  5.4.8.
- Seed: `1592598566` (`0x5EED2026`).
- Scene policy: 300 warm-up frames plus 5,000 measured frames, two independent runs per configuration/size.
- Raw CSV:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-s4p-pb2p/{luajit-on,luajit-off,lua54}`.

The same production-packager-generated LXSA was used in all three builds. CSV schema 2 records `lua_vm`,
`lua_version`, `jit_available`, and `jit_enabled`; those fields are benchmark metadata, not ScriptArtifact data.
The process-local allocation counter does not observe internal Lua VM allocation, so zero reported C++ allocations
must not be read as zero VM allocation.

## Micro comparison

Combined two-run medians for 10k operations:

| Path | LuaJIT JIT-on | LuaJIT interpreter | Lua 5.4 |
|---|---:|---:|---:|
| Prepared synchronous object call | 0.332 ms / 33.2 ns-call | 0.344 ms / 34.4 ns-call | 0.616 ms / 61.6 ns-call |
| Prepared Ability QUERY | 0.798 ms / 79.8 ns-query | 0.712 ms / 71.2 ns-query | 1.012 ms / 101.2 ns-query |
| Coroutine start + NextStep suspend | 6.24–7.39 ms | 6.37–7.08 ms | 19.07–19.32 ms |
| Steady 2k stable-point resumes | 0.359 ms median | 0.358 ms median | 0.738 ms median |
| Approximate steady resume cost | 180 ns/resume | 179 ns/resume | 369 ns/resume |

The coroutine start row includes Lua thread creation, registry retention, Ability call, awaitable/continuation
admission, and yield. A later resume reuses the same coroutine; it does not create a second VM thread. The first
resume frame also promotes the ready batch and is intentionally excluded from the approximate steady per-resume
cost.

LuaJIT JIT-off is semantically identical and, for these tiny C-bound hot paths, is not consistently slower than
JIT-on. The C API/table crossing dominates enough that small JIT scheduling/code-shape variation is visible. This is
measurement, not a performance gate. Lua 5.4 is about 1.85x the JIT-on plain-call median and roughly 3x the coroutine
start cost on this machine.

## Update-heavy scene scaling

Combined two-run results, milliseconds/frame:

| VM policy | Objects | p50 | p95 | p99 | ns/object at p50 |
|---|---:|---:|---:|---:|---:|
| LuaJIT JIT-on | 10,000 | 0.373 | 0.424 | 0.529 | 37.3 |
| LuaJIT JIT-on | 20,000 | 0.681 | 0.878 | 1.244 | 34.1 |
| LuaJIT interpreter | 10,000 | 0.344 | 0.409 | 0.616 | 34.4 |
| LuaJIT interpreter | 20,000 | 0.786 | 1.130 | 1.390 | 39.3 |
| Lua 5.4 | 10,000 | 0.596 | 0.767 | 1.125 | 59.6 |
| Lua 5.4 | 20,000 | 1.315 | 1.974 | 2.324 | 65.8 |

All configurations retain near-linear scaling. Lua 5.4 is about 1.9x the LuaJIT JIT-on p50 at 20k for this
long-lived-object workload. There is no per-frame coroutine allocation in this synchronous group.

## Prepared Ability scene workload

One QUERY plus one COMMAND per long-lived object, milliseconds/frame:

| VM policy | Objects | p50 | p95 | p99 | ns/object at p50 |
|---|---:|---:|---:|---:|---:|
| LuaJIT JIT-on | 10,000 | 1.144 | 1.347 | 1.682 | 114.4 |
| LuaJIT JIT-on | 20,000 | 2.350 | 2.685 | 2.987 | 117.5 |
| LuaJIT interpreter | 10,000 | 1.133 | 1.378 | 1.790 | 113.3 |
| LuaJIT interpreter | 20,000 | 2.460 | 2.954 | 3.241 | 123.0 |
| Lua 5.4 | 10,000 | 1.484 | 1.886 | 2.394 | 148.4 |
| Lua 5.4 | 20,000 | 3.105 | 3.846 | 4.393 | 155.2 |

The prepared receiver/thunk topology is unchanged. Entering C++ from Lua uses the catalog/method ordinal and the
current ScriptInstance's dense prepared table; none of these measurements include provider or string discovery.

## Coroutine, idle suspension, and churn

The 10k coroutine scene starts/yields/resumes real Lua invocations under the existing resume budget:

| VM policy | p50 | p95 | p99 |
|---|---:|---:|---:|
| LuaJIT JIT-on | 1.474 ms | 2.910 ms | 5.244 ms |
| LuaJIT interpreter | 1.479 ms | 2.901 ms | 5.453 ms |
| Lua 5.4 | 4.046 ms | 8.266 ms | 9.168 ms |

For 20k genuinely suspended Lua invocations with no ready work, all configurations measured a 100 ns p50/p95
stable point (Lua 5.4 p99 200 ns). No backend scanned VM coroutine records while idle.

At a steady 20k population with 100 retirement/rematerialization operations per frame:

| VM policy | p50 | p95 | p99 |
|---|---:|---:|---:|
| LuaJIT JIT-on | 0.124 ms | 0.322 ms | 0.601 ms |
| LuaJIT interpreter | 0.139 ms | 0.334 ms | 0.614 ms |
| Lua 5.4 | 0.155 ms | 0.225 ms | 1.085 ms |

Median churn continues to follow changed objects, not total population. No unbounded registry-reference growth or
population-wide teardown scan was observed.

## PB0/PB1 focused regression

The portability implementation does not change the generic ScriptSystem or FlowForge runtime hot path. Focused
checks at the S4-P SHA produced:

| Existing path | S4-P result | Historical range/reference |
|---|---:|---:|
| PB0 gameplay-mixed, 20k | p50 1.160 ms / p95 2.110 ms | PB0/PB2 about 0.96–0.97 ms p50 |
| PB0 suspended-idle, 50k | p50/p95 0.0001 ms | 0.0001 ms |
| PB1 FlowForge update, 20k | p50 0.213 ms / p95 0.275 ms | 0.241 / 0.289 ms |
| PB1 FlowForge suspend, 10k | 2.948 ms | 2.81–3.12 ms |
| PB1 FlowForge steady resume, 2k/frame | p50 0.289 ms / p95 0.367 ms | 0.27–0.42 ms |

The PB0 gameplay p95 was noisy in this single focused run, but its algorithm and storage path were untouched and
the scaling/idle invariants remain intact. FlowForge update/suspend/resume remained within the recorded PB1/PB2
ranges. No generic runtime complexity regression was found.

## Interpretation

The main portability costs observed on this machine are:

1. Lua 5.4 coroutine creation/suspension and subsequent resume, roughly 2–3x the LuaJIT costs;
2. Lua 5.4 synchronous object dispatch, around 60–66 ns/object versus 34–39 ns/object for LuaJIT;
3. Lua language-to-prepared-Ability crossing, 20k p50 3.10 ms on Lua 5.4 versus 2.35–2.46 ms on LuaJIT.

These are VM implementation differences, not semantic or architecture failures. JIT-on/off and Lua 5.4 all retain
bounded continuation bookkeeping, output-sensitive idle scheduling, prepared provider dispatch, and identical
stable-point behavior. PB2-P establishes a portability baseline; it does not set an absolute shipping threshold.
No coroutine pool, VM-specific scheduler, or runtime optimization wave was added.
