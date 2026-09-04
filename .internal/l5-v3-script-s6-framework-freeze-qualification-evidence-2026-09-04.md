# L5 v3 Script S6 / Framework Freeze qualification evidence — 2026-09-04

> Superseded for current qualification. This file remains the immutable evidence for `718425883a695c26008fa600ae196b60d8738644`.
> The final hot-plane closure candidate is recorded separately and is awaiting independent review; this historical
> revision must not be used to claim that the current Script framework is frozen.

## Qualified revision

- Baseline: `48a14af5e88d722435458a5b2d29d2c55043bcf3`.
- Production and qualification code: `718425883a695c26008fa600ae196b60d8738644`.
- Source: clean `codex/s6-script-freeze` worktree; `ValidateTrackedSnapshot` passed at the qualified revision.
- Build: Windows x64, MSVC 19.44.35228.0, RelWithDebInfo, Ninja, `-j 4 -k 0`.

## Frozen physical topology

- Physics2D is owned by `engine/domain/simulation/builtin/physics2d`; the repository has no top-level
  `extensions/` directory. Public namespace, headers, targets, package and Script Ability IDs remain unchanged.
- `simulation_script_cpp_static` remains the only C++ runtime backend. `ScriptCoroutine`, its promise and C++
  coroutine handles are backend-private to that adapter; ScriptSystem sees only `ScriptBackendContinuation`.
- Native and C++ coroutine frames use the same owner-free `BoundedFrameStorage` primitive. The primitive owns one
  preallocated byte arena plus bounded metadata and a free-span index; it has no per-frame heap fallback.
- Lua remains one backend for LuaJIT and Lua 5.4. Prepared Ability/Event entries use fixed sparse ordinal tables,
  not `instance_capacity * catalog_capacity` matrices.
- `ScriptOwnedResumeValue` uses 32 inline bytes and a bounded large-value spill. Scalar Event and Ability resume
  values remain inline.

## S6 coroutine contract

`CppStaticScript` wire/schema was hard-cut to Script schema 11 / LXSA wire 9 and now records sorted
`suspension_capable_exports`. `makeCppStaticCoroutineExport<&Behavior::task>()` projects the visible semantic
signature while hiding `ScriptCoroutineContext&` and the `ScriptCoroutine` return type. Lifecycle overlap,
mutable/rvalue/pointer parameters and malformed signatures fail at projection/compile time. Value arguments are
copied by the coroutine frame; `const T&` arguments are copied into a bounded invocation-owned argument block before
user code runs, so no call-frame reference survives suspension.

One invocation owns one bounded coroutine frame and persistent `ScriptCoroutineContext`. The context retains only
backend/instance identity and bounded frame-storage access. `ScriptStepContext*` and resume-packet views are active
only inside start/resume calls and are cleared before returning to ScriptSystem. READY resumes the coroutine;
FAILED/CANCELLED terminate through the backend-neutral failure path. Retirement destroys every backend
continuation before EndPlay and object destruction.

Generated `ScriptAbilityCoroutine<Ability, Context>` provides QUERY/COMMAND prepared dispatch and typed
ASYNC_OPERATION awaiters. BORROWED_STEP synchronous results are copied into owned values. Event awaits consume
`CppScriptEventSource<Payload>` and the existing `ScriptEventWaitFactory`. Delay shorthand is supplied by
`ScriptDelayCoroutine.hpp` and uses the canonical Delay Ability. No provider, Event payload pointer or
`ScriptStepContext*` survives suspension.

Generated `ScriptAbilityStatic<Ability, Provider>` is an optional product-side adapter. Creation checks provider
conformance, exact receiver address, ContractId and schema. It is absent from ScriptArtifact and provider identity
is not persisted. The generic backend continues to use prepared dynamic semantics.

## Correctness coverage

Focused tests prove:

- C++ synchronous completion, Event owned result, Delay-shaped suspension, typed Ability QUERY/COMMAND,
  BORROWED_STEP copy, async READY/FAILED/admission rejection, sequential re-suspension and multi-flight capacity;
- lifecycle/coroutine-role rejection, exactly-once frame destruction and zero backend frame heap allocations;
- BoundedFrameStorage alignment, fragmentation, coalescing, stale/double release and deterministic capacity failure;
- Lua sparse prepared storage at 100,000 configured instances and 256 catalog methods with only two actually
  prepared method entries; backing storage is 240 bytes for configured prepared capacity four;
- Native generated start/resume/re-suspend/destroy with `heap_frame_allocations == 0`;
- Physics direct, dynamic prepared and static-specialized result/schema parity;
- ScriptArtifact schema 11 / wire 9 C++ coroutine-symbol round trip.

## Build and CTest matrix

| VM | Profile | CTest | second build |
|---|---|---:|---|
| LuaJIT 2.1.1771261233 | DEVELOPER | 190/190 | no work |
| LuaJIT 2.1.1771261233 | PLAYER | 190/190 | no work |
| LuaJIT 2.1.1771261233 | EDITOR | 203/203 | no work |
| VM-independent | TOOLCHAIN | 172/172 | no work |
| Lua 5.4.8 | DEVELOPER | 190/190 | no work after selection build |
| Lua 5.4.8 | PLAYER | 190/190 | no work |
| Lua 5.4.8 | EDITOR | 203/203 | no work |
| LuaJIT, Physics2D OFF | DEVELOPER | 187/187 | no work |

The LuaJIT interpreter-only focused VM/coroutine/Scene suite passed 100 repeats per test. The equivalent LuaJIT
JIT-on suite passed 100 repeats per test. Under Lua 5.4, the C++ coroutine, bounded-frame, Native, Lua coroutine,
continuation, Event waiter and lifecycle set (7 tests) passed 100 repeats per test.

## Install and external closure

Independent prefixes:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s6-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s6-lua54
```

Both passed `ValidateInstalledArchitecture`. The Lua54 installation stages its selected `lua.dll`; the selected VM
remains an implementation/link policy and is not ScriptArtifact identity. Against each prefix, nine consumers
fresh-configured, built, ran and reported no work on the second build:

- `physics2d-script`;
- `system-event-await-runtime`;
- `flowforge-compiler`;
- `lua-script-packager`;
- `scene-script-runtime`;
- `script-ability-codegen`;
- `system-hook-script-binding`;
- `cpp-coroutine-script`;
- `script-static-ability-specialization`.

The four changed Modules public headers were synchronized byte-identically to Debug, RelWithDebInfo and Android
install include prefixes. Android was not configured or built.

## Validators and verdict

`ValidateTrackedSnapshot`, `ValidateSourceArchitecture`, `ValidateSourceStyle` and both installed-architecture
validations passed. No Coroutine/Ability/Event/Physics manager, service locator, provider registry, second scheduler,
second backend or runtime string dispatch was introduced.

**S6 PASS. SCRIPT FRAMEWORK FROZEN.** R1 was not started.
