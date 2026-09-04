# L5 v3 Script S6 optimized performance baseline — 2026-09-04

## Environment and raw data

- Production SHA: `647b80d6fdc4472840cdf23be3c35ab5b42d5ab0`.
- Windows x64, MSVC 19.44.35228.0, RelWithDebInfo, 24 logical CPUs.
- Seed: `1592598566` (`0x5EED2026`).
- Raw CSV and dependency inventories:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/s6-performance/`.
- Portable Lua artifact SHA-256:
  `6DE9CB310C30DB6A6970070BD72A3E49BA4DC35F16FD69014201DF908D667082`.

## Bounded C++ coroutine frames

The first implementation scanned the physical block list and measured 178.7 ms to start 10k frames. The final
free-span index makes sequential allocation O(available spans), reducing 10k start to 0.836 ms.

| Invocations | start total | ns/start | resume+destroy total | ns/resume | arena bytes | allocations |
|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 0.836 ms | 83.6 | 0.465 ms | 46.5 | 5,120,000 | 0 |
| 50,000 | 3.940 ms | 78.8 | 2.479 ms | 49.6 | 25,600,000 | 0 |
| 100,000 | 8.131 ms | 81.3 | 4.463 ms | 44.6 | 51,200,000 | 0 |

Scaling is linear and repeated resume does not allocate a new frame.

## Synchronous baseline and FlowForge regression

| Path | scale | p50 | p95 | unit p50 | allocations |
|---|---:|---:|---:|---:|---:|
| direct C++ benchmark call | 100k | 0.141 ms | 0.142 ms | 1.41 ns | 0 |
| generated dynamic Ability thunk | 100k | 0.139 ms | 0.150 ms | 1.39 ns | 0 |
| BoundScriptCall | 100k | 0.075 ms | 0.101 ms | 0.75 ns | 0 |
| FlowForge AOT update | 20k objects/frame | 0.234 ms | 0.301 ms | 11.7 ns/object | 0 |
| FlowForge suspend | 10k | 2.255 ms | — | 225.5 ns | 0 |
| FlowForge steady resume batch | 2k/frame | 0.299 ms | 0.314 ms | about 149 ns | 0 |

PB1 recorded FlowForge 20k update at 0.241/0.289 ms p50/p95 and 10k suspend at 2.81–3.12 ms. The optimized result
retains the same linear/idle semantics and removes Native frame heap allocation.

## Lua portability comparison

| VM policy | 20k update p50 | p95 | 10k coroutine start | steady 2k resume p50 | Engine allocations |
|---|---:|---:|---:|---:|---:|
| LuaJIT JIT-on | 1.020 ms | 1.460 ms | 7.295 ms | 0.421 ms | 0 |
| LuaJIT interpreter-only | 1.072 ms | 1.507 ms | 6.735 ms | 0.373 ms | 0 |
| Lua 5.4.8 | 2.299 ms | 3.421 ms | 18.177 ms | 0.570 ms | 0 |

LuaJIT/Lua54 VM allocations remain outside the Engine C++ allocation counter. The backend records VM coroutine
creation separately and deliberately does not pool/restart dead coroutine threads. With 100k instance capacity,
256 catalog methods and configured actual prepared capacity four, sparse Ability backing storage is 240 bytes,
instead of scaling with `instance_capacity * catalog_capacity`.

## Event and Physics gameplay baseline

- 100k idle Event waiters over 5,000 stable points: p50/p95/p99 100 ns, zero dispatch visits and zero allocations.
- Physics, 100k real `overlapsBox()` queries:

| boundary | p50 | p95 | ns/query p50 | allocations |
|---|---:|---:|---:|---:|
| direct `Physics2DSystem` | 3.723 ms | 4.062 ms | 37.23 | 0 |
| prepared dynamic Ability | 3.761 ms | 4.019 ms | 37.61 | 0 |
| generated static specialization | 3.714 ms | 4.041 ms | 37.14 | 0 |

The dynamic boundary remains about 0.38 ns/query over direct in this run. Static specialization is within timing
noise of direct and does not justify replacing prepared dynamic runtime dispatch.

The final 20k mixed Simulation splits long-lived objects approximately 25% each among C++ sync, C++ coroutine,
FlowForge and Lua, and combines real Physics QUERY, normal Event callback, Event.await and Delay. Over 1,000 measured
frames after 300 warmups: p50 7.788 ms, p95 21.506 ms, p99 22.536 ms, max 24.770 ms, zero measured C++ allocations.
The bimodal high percentile is the expected ready/resume frame; resume budget and queue high-water remain bounded.

## Findings

1. The fixed free-span index removed an accidental O(N²) coroutine-frame admission path.
2. Lua coroutine creation remains the largest frontend-specific start cost; no portable pooling contract exists.
3. Lua 5.4 remains roughly 2.25x LuaJIT for the 20k synchronous update and roughly 2.5x for 10k starts.
4. No idle full scan, per-resume allocation, runtime provider discovery or per-call Contract/Method string lookup was
   observed.

This is a recorded baseline, not an absolute performance gate.

