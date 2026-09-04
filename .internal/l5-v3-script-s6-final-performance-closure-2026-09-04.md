# L5 v3 Script S6 final performance closure — 2026-09-04

## Environment and method

- Baseline revision: `53609e74dc989d2c771c1fd889bfbd720dd688f9`.
- Runtime/test production revision: `d21a36f9669b832ec63ee318ff974e2cd73f5890`.
- Windows 11 x64, Intel Raptor Lake desktop, 24 logical CPUs, MSVC 19.44.35228.0, RelWithDebInfo.
- Deterministic seed: `1592598566` (`0x5EED2026`).
- Raw CSV: `E:/SyncForder/CodeRepos/build/RelWithDebInfo/s6-final-closure-performance/`.
- VTune 2026.3 software-sampling results:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/s6-vtune-analysis/`.

Representative runtime scenarios use 10k and 20k populations. The 50k/100k cases are retained only for one-time
capacity, idle-complexity and storm verification. Every reported timed path was repeated twice with the same seed.
Hardware sampling was unavailable, so this evidence does not make cache/TLB or branch-misprediction claims.

## Before/after profiler result

Before the closure, the 10k Lua synchronous profile spent 17.7% and 13.3% of CPU time in the recursive-mutex lock
guard destructor and constructor. Both symbols disappear after the single-owner change. The after Lua Ability sample
places 19.5% in `LuaAbilityProjectionAccess::current`; this is the direct backend-local active-execution/prepared-slot
bridge, with no lock, vector scan, string lookup or provider discovery. The rest of the leading samples are LuaJIT
VM/generated-code functions.

Before Native ABI v5, FlowForge synchronous sampling spent 33.9% in
`NativeScriptBackend::State::invokeAbility`, with another 7.7% in prepared-vector size handling. Both disappear.
After v5, samples are led by the generated AOT module (37.7%), `invokePrepared` (18.2%) and ScriptSystem invoke
(17.7%). No generic erased Ability invoke remains on the source-composed FlowForge path.

The earlier mixed owner profile exposed `Mtx_unlock` at 6.5%. It is absent from the final leading samples. The final
synthetic mixed profile reports `malloc/free` at about 2% each because that benchmark's synthetic backend explicitly
allocates a `Continuation` object per suspension; dedicated CppStatic, Native, Event, NextStep and ingress scenarios
all report zero Engine C++ allocations.

## Lua 10k/20k representative results

Round-two values are shown; round one preserved the same ordering and scale.

| VM | scenario | 10k p50 / p95 | 20k p50 / p95 | Engine allocations |
|---|---|---:|---:|---:|
| LuaJIT JIT-on | update | 0.22 / 0.23 ms | 0.44 / 0.46 ms | 0 |
| LuaJIT JIT-on | Ability | 0.83 / 0.85 ms | 1.79 / 1.95 ms | 0 |
| LuaJIT JIT-on | coroutine | 0.92 / 1.85 ms | 1.44 / 2.03 ms | 0 |
| LuaJIT JIT-on | Event | 5.55 / 8.12 ms | 12.62 / 22.64 ms | 0 |
| LuaJIT interpreter | update | 0.30 / 0.32 ms | 0.64 / 0.74 ms | 0 |
| LuaJIT interpreter | Ability | 0.80 / 0.94 ms | 1.66 / 1.91 ms | 0 |
| Lua 5.4.8 | update | 0.46 / 0.57 ms | 1.06 / 1.36 ms | 0 |
| Lua 5.4.8 | Ability | 1.23 / 1.42 ms | 3.16 / 3.38 ms | 0 |
| Lua 5.4.8 | coroutine | 3.75 / 8.70 ms | 3.78 / 9.87 ms | 0 |
| Lua 5.4.8 | Event | 20.73 / 22.70 ms | 81.60 / 104.16 ms | 0 |

LuaJIT update now costs about 22 ns/object/frame and its typed Ability workload about 83 ns/object/frame. Lua 5.4
Event's non-linear 10k-to-20k growth repeated in both rounds. Runtime counters show zero Engine allocations and
output-sensitive waiter visits; the remaining growth is Lua 5.4 VM coroutine allocation/GC debt. No VM-specific
coroutine pool is added because a common safe restart contract does not exist.

## FlowForge ABI v5

| path | population | round 1 p50 | round 2 p50 | unit cost | allocations |
|---|---:|---:|---:|---:|---:|
| sync AOT | 10k | 0.090 ms | 0.090 ms | 9.0 ns/object | 0 |
| tiny typed Ability QUERY | 10k | 0.085 ms | 0.081 ms | 8.1–8.5 ns/object | 0 |
| suspend | 10k | 2.091 ms | 2.098 ms | 209–210 ns/suspend | 0 |

The steady-resume benchmark records promotion/drain by frame and is unsuitable for interpreting its near-zero p50
as an individual resume cost; no such claim is made. VTune does prove the intended stack shape: generated module,
prepared Native invoke, ScriptSystem invoke, with no erased value-slot Ability marshalling.

## C++ coroutine, owner scheduler and ingress

| scenario | population | measured total / frame | allocations | structural result |
|---|---:|---:|---:|---|
| C++ coroutine start | 100k | 8.22 ms | 0 | bounded frame arena |
| C++ coroutine resume+destroy | 100k | 5.38 ms | 0 | same frame, exact release |
| idle Event waiters | 100k | below timer resolution p50 | 0 | zero waiter visits |
| ready storm | 100k total, 10k ready | 0.24 ms p50 | 0 | 2k budget, five drains |
| NextStep idle | 100k | 0.009 ms | 0 | no population scan |
| NextStep expire+resume | 100k | 20.70 ms | 0 | O(ready), owner direct |

Owner-direct NextStep completion reduces the same 100k expire/resume case from 24.86 ms to 20.70 ms and keeps the
external-ingress high-water at zero. External storm qualification reaches a queue high-water of 10k, drains under
the existing 2k resume budget and performs no heap allocation.

## Physics and source-composed mixed composition

Real Physics2D `overlapsBox()` at 10k queries:

| boundary | round 1 | round 2 | allocations |
|---|---:|---:|---:|
| direct domain | 37.16 ns/query | 37.20 ns/query | 0 |
| prepared dynamic Ability | 37.64 ns/query | 37.58 ns/query | 0 |
| generated static specialization | 37.23 ns/query | 37.05 ns/query | 0 |

The dynamic boundary adds roughly 0.4 ns/query in these runs. Static/IPO is within measurement noise of the direct
domain path and remains an optional source-composed policy rather than a second runtime dispatch architecture.

The final real mixed composition contains approximately equal C++ sync, C++ coroutine, FlowForge and Lua
populations plus Physics QUERY, normal Event callback, Event await and Delay:

| population | round | p50 | p95 | p99 | max | Engine allocations |
|---:|---:|---:|---:|---:|---:|---:|
| 10k | 1 | 2.02 ms | 4.33 ms | 4.57 ms | 8.90 ms | 0 |
| 10k | 2 | 2.69 ms | 4.23 ms | 4.48 ms | 5.21 ms | 0 |
| 20k | 1 | 5.14 ms | 8.74 ms | 9.20 ms | 11.86 ms | 0 |
| 20k | 2 | 4.50 ms | 8.78 ms | 9.15 ms | 10.09 ms | 0 |

The p50 noise is larger than the individual frontend micros, but p95/p99 scale approximately linearly and the
composition remains allocation-free on the Engine side.

## Acceptance result and remaining costs

- Lua hot stacks contain no mutex, execution vector/scan or prepared ordinal hash table.
- FlowForge source-composed calls contain no `NativeScriptBackend::invokeAbility` or erased slot marshalling.
- NextStep has no heap and no idle scan; owner Event/Simulation Delay do not traverse external ingress.
- Scalar prepared runtime paths allocate no Engine heap; all new storage is bounded.
- Runtime provider/Contract/Method strings are absent from call-time dispatch.

The three largest remaining observed costs are Lua VM coroutine creation/GC, the Lua active-execution bridge, and
generic ScriptSystem/Native prepared invocation around very small FlowForge work. These are measured debts, not
complexity violations. This document records a performance baseline; it does not establish an absolute frame-time
gate or declare the framework frozen.
