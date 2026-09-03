# L5 v3 Script S5.0 Event.await Runtime Qualification Evidence — 2026-09-03

## Qualified revision

- Production implementation and regression tests: `221b668c2597c49e5a21618329f5ef38f29415f7`.
- Qualification used the clean detached worktree
  `E:/SyncForder/CodeRepos/lux-engine-s50-qualification` at that exact revision.
- `ValidateTrackedSnapshot.cmake`, `git status --short`, and `git diff --exit-code` confirmed the tracked snapshot was
  clean. The developer checkout's unrelated `.gitignore`, `CppStaticScriptBridge.hpp`, and `WorldPartition.hpp`
  modifications were excluded.

## Runtime topology

S5.0 reuses the existing endpoint and continuation path:

```text
EventPoint
    -> one ScriptEventEndpoint connection per ScriptSystem EventBucket
    -> normal bound callbacks + bounded one-shot Event waiters
    -> existing ScriptAwaitableCompletion / AwaitableIngress
    -> existing ResumeRing
    -> ScriptSystem::executeStablePoint()
```

`ScriptStepContext::event_waits` is the backend-neutral registration seam. A request contains only
`SystemInstanceId`, `EventPointId`, and `EEventRoute`. Entity-targeted waits are self-only: registration derives the
exact generational target from the current `EntityScriptScope::self`; Simulation-scoped instances fail with
`SCOPE_MISMATCH`. No string lookup, Event manager, language-specific state, second scheduler, or per-waiter
`EventPoint::connect()` was introduced.

## Bounded waiter and ownership model

`ScriptRuntimeLimits::event_wait_capacity` bounds a private generational `EventWaiterId` SlotMap. Each record owns:

- the registering `ScriptInstanceId` and existing `ScriptAwaitableId`;
- its EventBucket and broadcast/no-target or exact targeted Entity route key;
- a monotonic registration sequence and `ACTIVE`/`CLAIMED` state;
- intrusive route and per-instance ownership links;
- pre-sized owned resume payload storage.

The route index is pre-reserved to the configured waiter capacity. Dispatch performs one route lookup plus work
proportional to matching eligible waiters. `ScriptRuntimeStats` exposes active/high-water waiter counts, dispatch
visits, and per-instance waiter/awaitable/continuation cleanup visits.

Awaitables and continuations now also maintain intrusive per-instance ownership lists. Instance retirement no longer
scans global SlotMaps. The tested destruction order is:

```text
stop admission
    -> erase the InstanceRecord and invalidate its generation
    -> cancel owned Event waiters
    -> cancel owned Awaitables
    -> destroy owned backend Continuations
    -> EndPlay
    -> destroy the backend object
```

All creation, completion, rollback, cancellation, failure, unmount, and shutdown paths unlink each ownership record
exactly once. Late Event occurrences and completion leases cannot resume a stale incarnation.

## Dispatch cutoff and nested occurrences

An Event occurrence captures the current registration sequence, unlinks and marks all matching pre-cutoff waiters
`CLAIMED`, and stores their generational IDs in a pre-reserved scratch segment. Existing normal callbacks then run.
After callbacks, each claim revalidates the waiter, ScriptInstance generation, and active mount before payload
publication.

Because claims leave the route index before callbacks, a nested occurrence cannot double-claim them. Scratch segment
boundaries make nested dispatch independent. A callback-created waiter is newer than the outer cutoff; it cannot
consume the outer occurrence, but it may consume a later, independently dispatched nested occurrence. Fault or
retirement during a callback makes the mount non-active immediately while deferring handler-storage mutation to the
stable point.

## Payload ownership and stable-point-only resume

`ScriptEventEndpointDescriptor` now carries a canonical owned payload layout and a typed, `noexcept` copy thunk.
Supported scalar ABI payloads receive automatic value-copy projection. `STRUCT_REF` has no raw-memory fallback and
requires an endpoint-owner-provided typed copy. Missing, mismatched, oversized, or failing projections reject the
wait registration or fail it closed.

The borrowed Event call frame is copied into `ScriptOwnedResumeValue` storage before READY publication. Neither the
payload pointer nor a Script ABI slot address survives dispatch. Source mutation after dispatch therefore cannot
alter the resumed value.

Event dispatch only claims, copies, marks the existing awaitable READY/FAILED, and enqueues through the existing
bounded ingress. It never calls a backend continuation, Lua, FlowForge, or `executeStablePoint()`. Tests assert zero
resume count after dispatch and resume only after an explicit production stable-point drain, including eager,
nested, fan-out, and resume-budget cases.

## Complexity proof

The production runtime test creates 10,000, 50,000, and 100,000 pending waiters separately and executes four stable
points with no Event occurrences. In every case:

- active waiter count remains unchanged;
- dispatch visits remain unchanged;
- waiter, awaitable, and continuation cleanup visits remain unchanged;
- the endpoint still has exactly one ScriptSystem connection.

A second instrumented case creates 100,000 global waiters, with one waiter/awaitable/continuation owned by the
instance being retired. Retirement decrements the active count by one and increments each of the three instance
cleanup visit counters by exactly one. An unrelated route occurrence records zero visits, while a matching
occurrence records exactly its matching waiter count.

These counter assertions prove zero-event stable points do not scan waiters and one-instance retirement is
`O(owned waiters + owned awaitables + owned continuations)`, independent of global population.

## Correctness qualification

Windows 11 Pro 10.0.26200, MSVC 19.44.35228, x64 RelWithDebInfo, Ninja `all -j 4 -k 0`:

| VM / profile | CTest | Final build |
|---|---:|---|
| LuaJIT / DEVELOPER | 185/185 | `ninja: no work to do` |
| LuaJIT / PLAYER | 185/185 | `ninja: no work to do` |
| LuaJIT / EDITOR | 198/198 | `ninja: no work to do` |
| VM-independent / TOOLCHAIN | 169/169 | `ninja: no work to do` |
| Lua 5.4 / DEVELOPER | 185/185 | `ninja: no work to do` |

The focused S5.0 suite covers callback-only, waiter-only, coexistence, self-target filtering, one-shot fan-out,
multiple occurrences, registration cutoffs, nested dispatch, claimed-waiter retirement, payload ownership, stable
point ordering, resume budgets, all relevant capacity failures, rollback/copy failure, shutdown, stale generations,
rematerialization, and the instrumented complexity cases.

Regression coverage includes NextStep, Simulation and real-time Delay, eager completion, continuation generation and
lifetime, Hook single-flight, Event multi-flight, FlowForge resumable AOT execution, Lua coroutine execution,
retirement/late completion, LuaJIT JIT-on, LuaJIT interpreter-only, and Lua 5.4.

One hundred repeat-until-fail runs passed for:

- LuaJIT: Event wait, continuation, lifecycle, runtime, Lua coroutine JIT-on/JIT-off, and Scene Lua JIT-on/JIT-off;
- Lua 5.4: Event wait, Lua coroutine JIT-on/JIT-off policy paths, and Scene Lua JIT-on/JIT-off policy paths;
- TOOLCHAIN: FlowForge runtime integration.

`ValidateSourceArchitecture.cmake`, `ValidateSourceStyle.cmake`, and `ValidateTrackedSnapshot.cmake` passed from the
clean source tree.

## Installed closure

Separate full install prefixes were produced for LuaJIT and Lua 5.4 and both passed
`ValidateInstalledArchitecture.cmake`:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s50-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s50-lua54
```

Against each prefix, fresh relocated consumers configured, built, ran successfully, and produced a no-work second
build:

- `system-event-await-runtime`;
- `scene-script-runtime`;
- `system-hook-script-binding`;
- `script-ability-codegen`.

The new consumer executes a real Event start callback, registers a waiter through `ScriptStepContext`, dispatches a
typed payload, proves no immediate resume, then resumes with the copied value at the stable point.

## Explicitly not done

- S5.1 FlowForge Event-await node and Lua Event-await authoring surface are not implemented.
- Physics and Navigation production Abilities are not implemented.
- S2.4 AssetLoad remains blocked by the script-visible residency-backed Asset handle contract.
- PB3 is not recorded.
- C++ coroutine ergonomics are not implemented.
- Arbitrary-entity Event subscriptions and waiter rebinding across rematerialization are not implemented.

S5.0 Event.await runtime semantics are qualified. No S5.1 implementation was started.
