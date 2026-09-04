# L5 v3 Script PB3 Event/language subset performance baseline

Date: 2026-09-04

This is the PB3 Event/language subset, not a complete Physics-inclusive PB3. Physics rows are intentionally absent
because no canonical production Physics provider can be published from the current extension ownership.

## Reproducibility

- Measurement revision: `c9e32413e5c1c19c1d8519df07350f6416523d80`.
- Final qualification-only consumer include revision: `830aa708a7409c52ac003149b718ea0af2007f08`.
- Build: Windows RelWithDebInfo, MSVC 19.44.35228.0.
- CPU: Intel Core i7-13700KF, 16 physical cores, 24 logical processors.
- Seed: `1592598566` (`0x5EED2026`).
- LuaJIT: 2.1.1771261233, JIT enabled and interpreter-only policy.
- PUC Lua: 5.4.8.
- Scene policy: 300 warmup frames and 5,000 measured frames.
- Raw CSV directory:
  `E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-s5-pb3/`.

Every scenario validates its deterministic observation/checksum and exits non-zero on count, capacity, budget, or
lifetime mismatch. Timing and allocation instrumentation follow the existing PB0/PB1/PB2 benchmark conventions.

## Event waiter micro

10,000 waiters, one measured operation batch:

| Operation | Total ms | ns/waiter |
|---|---:|---:|
| Register | 3.1501 | 315.0 |
| Deliver + owned i32 copy + enqueue | 1.6922 | 169.2 |
| Stable-point resume | 1.5341 | 153.4 |
| Instance retirement/cancel | 2.6124 | 261.2 |

Delivery copied 40,000 payload bytes, visited exactly 10,000 matching waiters, and produced a queue high-water of
10,000. No per-waiter heap-owned Event connection is involved.

## Idle and output-sensitive complexity

| Idle waiters | p50 ms/stable point | p95 | p99 | Dispatch visits over 5,000 frames |
|---:|---:|---:|---:|---:|
| 10,000 | 0.0001 | 0.0001 | 0.0001 | 0 |
| 50,000 | 0.0001 | 0.0001 | 0.0001 | 0 |
| 100,000 | 0.0001 | 0.0001 | 0.0001 | 0 |

The timer resolution floors most samples at 100 ns; the relevant proof is the unchanged zero dispatch-visit counter,
not the absolute sub-microsecond value.

For 100,000 targeted waiters with 10,000 matching routes, delivery took 3.0291 ms, visited exactly 10,000 waiters,
copied 40,000 bytes, enqueued 10,000 resumes, and left 90,000 waiters untouched. This is output-sensitive in `K`,
not a global waiter scan.

## Fan-out and resume budget

10,000 broadcast waiters, resume budget 2,000/frame:

| Phase/frame | ms | Resumed total | Queue depth after frame |
|---|---:|---:|---:|
| Deliver/copy | 1.6499 | 0 | 10,000 |
| Resume 0 | 0.3313 | 2,000 | 8,000 |
| Resume 1 | 0.3279 | 4,000 | 6,000 |
| Resume 2 | 0.3051 | 6,000 | 4,000 |
| Resume 3 | 0.2988 | 8,000 | 2,000 |
| Resume 4 | 0.3117 | 10,000 | 0 |

The queue drains in exactly five stable points. Event dispatch performs no Script resume.

The production FlowForge Event storm shows the same budget behavior: 0.4175, 0.3940, 0.3692, 0.3529, and
0.3521 ms for queue depths 8,000, 6,000, 4,000, 2,000, and 0. The compiler-known continuation frame is 48 bytes;
the benchmark artifact is 3,584 bytes.

## FlowForge Event.await scaling

These rows measure the production AOT/native backend with a 2,000-resume/frame budget. Above 2,000 objects the
steady cycle is budget-limited, so the population primarily changes suspended backlog rather than work admitted in
one frame.

| Objects | p50 ms | p90 | p95 | p99 | max |
|---:|---:|---:|---:|---:|---:|
| 2,500 | 1.1804 | 1.2391 | 1.2927 | 1.4121 | 2.0245 |
| 5,000 | 1.2145 | 1.2746 | 1.3417 | 1.5595 | 10.6274 |
| 10,000 | 1.2612 | 1.3474 | 1.3906 | 1.5327 | 2.7578 |
| 20,000 | 1.3525 | 1.4242 | 1.4672 | 1.5741 | 1.8503 |

With 20,000 suspended FlowForge Event continuations and no occurrence, stable-point p50/p95/p99 are all 0.0001 ms
and dispatch visits remain unchanged. There is no continuation-frame scan.

## Portable Lua Event.await scaling

Each frame invokes every independent long-lived Lua object, registers one waiter, delivers one owned i32 payload,
and resumes the same coroutine at the stable point.

| VM | Objects | p50 ms | p90 | p95 | p99 | ns/object at p50 |
|---|---:|---:|---:|---:|---:|---:|
| LuaJIT JIT-on | 2,500 | 1.9045 | 2.4822 | 2.6776 | 2.9941 | 761.8 |
| LuaJIT JIT-on | 5,000 | 3.7728 | 5.0406 | 5.3687 | 5.9052 | 754.6 |
| LuaJIT JIT-on | 10,000 | 7.8505 | 10.9957 | 11.8179 | 13.2877 | 785.1 |
| LuaJIT JIT-on | 20,000 | 16.2886 | 23.4429 | 24.7975 | 26.9404 | 814.4 |
| LuaJIT interpreter | 2,500 | 1.8551 | 2.3061 | 2.4227 | 2.6778 | 742.0 |
| LuaJIT interpreter | 5,000 | 3.7518 | 5.0223 | 5.3177 | 5.8573 | 750.4 |
| LuaJIT interpreter | 10,000 | 8.3426 | 11.2847 | 12.0800 | 13.5910 | 834.3 |
| LuaJIT interpreter | 20,000 | 16.6982 | 23.7657 | 25.2068 | 27.2806 | 834.9 |
| Lua 5.4 | 2,500 | 4.9188 | 5.6028 | 5.8902 | 6.2657 | 1,967.5 |
| Lua 5.4 | 5,000 | 10.7191 | 12.3869 | 13.3993 | 15.4301 | 2,143.8 |
| Lua 5.4 | 10,000 | 21.6706 | 22.9149 | 23.4395 | 29.0798 | 2,167.1 |
| Lua 5.4 | 20,000 | 45.3647 | 50.7982 | 52.9523 | 61.5032 | 2,268.2 |

One-batch 10,000-object Event cycles measured 718.7 ns/object for LuaJIT JIT-on, 747.5 ns/object for LuaJIT
interpreter, and 3,110.2 ns/object for Lua 5.4 after the common 300-frame warmup. The language performance
difference is not a semantic failure.

## Regression and limitations

Focused PB0/PB1/PB2/PB2-P groups were rerun:

- PB0 gameplay-mixed and 100,000 suspended-idle;
- PB1 FlowForge update-heavy, suspend, and resume;
- PB2/PB2-P Lua update-heavy, Ability, and coroutine for JIT-on, interpreter-only, and Lua 5.4.

All checksum/capacity invariants passed. No idle scan, global retirement scan, unbounded ready storage, direct Event
resume, per-call provider lookup, or per-call Contract/Method string lookup was observed.

This subset does not report a single Physics-inclusive mixed composition. Existing C++ Static, FlowForge, Lua,
Delay, Ability, and Event workloads were measured as independently reproducible groups; a full cross-domain mixed
PB3 is blocked until production Physics has an approved provider owner. Navigation and AssetLoad are also absent for
the contract reasons recorded in the S5 qualification evidence. No fake provider was used to fill those rows.

PB3 Event/language subset: RECORDED.

