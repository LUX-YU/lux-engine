# v11 G4 — CPU completion events

Parent S1 checkpoint: `222a90730dbb077372f0f19a10283a20076c4a7a`.
This document records the passed internal G4 construction gate. S2 and independent final Verification remain incomplete.

## Owner and handoff

The Server's prepared StableSlotMap is the sole in-flight Command owner. Its original ReceivedRequest, typed result,
finite Sender operation and Native continuation remain address-stable there. The bounded CPU queue holds only a
Server-local generation/epoch notification; it does not own another request or payload. One admitted slot reserves one
possible unread event before work starts. Physical capacity N is completion_capacity + compile_capacity, preserving the
previous two logical admission budgets within one owner. The existing compiling count and slots.size() - compiling enforce
their respective bounds; there is no second index or request counter. A Transfer has at most one CPU chunk in flight and
cannot start the next chunk until its previous event is consumed, so the notification capacity bound is the same N.

The host supplies one CpuSchedule backed by the existing ExecutionRuntime::CpuScheduler. Compile and staging preparation
share it; Render does not link Engine or create a pool. The host-TU forwarding receiver holds Runtime and provider code
pins on its own stack until the downstream callback returns, including when that callback permits operation destruction.
The borrowed scheduler and its factory's host module must outlive all accepted work and callback stacks. Normal callbacks
only publish events; RenderThread alone adopts Native results and replies. Synchronous completion is consumed only after
start returns to the explicit owner safe point. Shutdown retains the original request and Native backing until CPU events
and genuine GPU obligations settle before Responder reset.

GPU business polling remains the single explicit transition in G4. G5 replaces it with physical Queue-frontier callbacks;
this checkpoint cannot claim that GPU polling or the old TextureUpload implementation has already been removed.

The finite Sender adapter owns its operation, not an arbitrary external execution host. A sender supplied by unloadable
code must execute under a host that retains that provider module until its terminal callback stack has returned. Actual
compile/transfer work uses the CpuSchedule host trampoline and its two pins. A pin local to a DLL's own terminal callback
would not prove that its final return instruction is safe; this implementation does not claim otherwise.

Queue publication has its own bounded call-tail count in the existing Impl. BlockingRingQueue unlocks before notifying;
therefore consuming the final event alone cannot authorize destruction of its queue. Shutdown waits for both the pending
slots and already-entered publishers to finish. The event publisher caches its queue and counter before publication and
never touches its slot after publication. This is not another request owner or a completion scan.

## Evidenced PortSender correction

An independent executable using the real Engine components and stdexec 0.10.0 reproduced a synchronous-completion memory
error under MSVC ASan: its receiving completion legally deleted the operation; start then accessed phase_. Original
failure and binaries are retained at `E:/RenderV2Rebuild/V11/experiments/port-sender/`.

The correction stores the outcome during SUBMITTING and delivers only after submit returns. ACCEPTED asynchronous
completion delivers as its final state access. No second Owner, worker, queue or TaskScope behavior was introduced.
The corrected external ASan build, second no-work build and all 6 applicable component tests passed, including 128
controlled overlap cases. This focused result does not replace the complete G4 build/test/installation matrix.

The host fixture now injects a structured CPU scheduling error for the old rejection branch. Its assertion checks
kCpuScheduleFailure and the original CAPACITY_EXCEEDED code. The separate actual Server pending-capacity assertion stays
kRuntimeCapacity; the two failure sources are no longer conflated. Controlled injection is not reported as real scheduler
queue saturation.

## Regression findings retained

The complete Release runs caught and retained actual implementation failures: an internal builtin thunk still used the
former invocation-owner cast; absent upload configuration allocated four unintended staging pages; stopping preparation
before its first GPU submit was incorrectly treated as an invalid Ticket; and the first unified slot bound displaced the
previous simultaneous two Compile plus two GPU/Readback contract. These are repaired in production, not by loosening the
original buffer, capacity or terminal assertions. The explicit Native work_stopped reason prevents publication but does
not claim to cancel already accepted GPU work. Its original failure wins, and existing Tickets must still complete.

The TextureRendering fixture previously had two compile workers plus one upload worker. Its shared host now has those same
three workers: the test deliberately blocks both compile workers while requiring upload to proceed. Render still creates
no workers. Installed consumers now request the actual process_execution component and copy only their real DLL closure.
All 79 baseline test names remain present; the current matrix contains 94 tests. Failed attempts remain in the G4 logs.

Debug also exposed a real shutdown deadlock: pending GPU work was polled before the final native drain. A controlled
Fence observation hold therefore prevented reaching the real wait. Shutdown now stops/cancels new work, drains actual
submitted Tickets while every CPU/Request/Native owner is retained, then consumes remaining completions. No wait was
added to tick. The test now starts its hold immediately after a successful Submit and requires this shutdown's wait count
to increase; it does not relax its timeout or release assertions.

## G4 evidence

The serial matrix in `E:/RenderV2Rebuild/V11/runners/g4-matrix.ps1` configured and built `all -j 4 -- -k 0` twice,
then ran `ctest --output-on-failure -j 1 -R .`. Every second build reported `ninja: no work to do`.

| Configuration | Full CTest | Test wall time |
|---|---:|---:|
| RelWithDebInfo | 94/94 | 91.01 s |
| Debug | 94/94 | 93.04 s |
| MSVC ASan `/fsanitize=address /Zi` | 94/94 | 121.59 s |

ASan used `alloc_dealloc_mismatch=1`. Public-header builds, relocated installed consumers, actual external Buffer,
0/1/16/128 finite CPU completions, synchronous completion, DLL callback-tail unloading, true GPU and shutdown tests passed.
The actual GPU is NVIDIA GeForce RTX 4070 Laptop GPU, Vulkan API integer 4211029, driver integer 2559967232.
Validation and SyncValidation reported no errors; existing duplicate EOS overlay-layer warnings remain in the raw logs.
Binding remains 32 B, PreparedStorageBinding 64 B, RetirementEntry 72 B; the 10,000-record ledger/binding case remains
1,343,640 bytes and 61 cold allocations. Stable texture sampling still reports first-party new=0. These are regression
observations, not evidence that the unfinished S2 implementation is faster.

Protection checks passed: 719 frozen Legacy blobs, historical evidence, the two original user changes, dependency SHAs,
and the frozen IPC physical transport. The 228-file evidence index includes failed attempts, full logs, CSV samples and
the independent PortSender reproducer: `E:/RenderV2Rebuild/V11/evidence/g4-evidence-index.json`,
SHA-256 `8fd281bc3c7147f449d0f11a63a7002f180a1474dee7fad995c729e1b2bfe57e`.
This is not final independent V; G5 GPU callbacks and unified upload are the next authorized gate.
