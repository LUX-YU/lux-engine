# R2-FIX — Independent qualification

## Result and commit identity

**PASS: R2 reply lifetime closure and the authorized architecture amendment.**
R3 remains unapproved pending user review. No future-stage production code was added.
The historical `R2_VERIFICATION.md` still records its original PARTIAL result.

- Verified remote/base: `2e09985290ee630af0b524bd55d100c02c424d57`.
- Implementation I: `4ae55059cca949178a8a1f2eaffe32f720384e8d`.
- Independent verification V: the subsequent commit adding only this report.
- Original R2 implementation / benchmark before: `c33ae729016ed7cfbee1218d660dedb59a327157`.
- Frozen V1: `a669409a289a6fa4092f21176397795b1cdb7f3e`.
- Date: 2026-10-10. Branch: `codex/render-v2`.

## Exact modification list

Implementation I changes these twelve paths; V adds only `docs/render-v2/R2_FIX_VERIFICATION.md`:

```text
docs/render-v2/03_CORE_AND_TRANSPORT.md
docs/render-v2/05_BACKEND_RUNTIME_FRAME_TARGET.md
docs/render-v2/07_RENDER_SYSTEM_PROJECTION_RESOURCE.md
docs/render-v2/09_IMPLEMENTATION_PLAN.md
docs/render-v2/10_LLM_CONTRACT.md
docs/render-v2/R2_FIX_WORK_ORDER.md
modules/function/render/transport/README.md
modules/function/render/transport/include/lux/engine/render/transport/Replies.hpp
modules/function/render/transport/src/Replies.cpp
modules/function/render/transport/test/CMakeLists.txt
modules/function/render/transport/test/ReplyLifetime.cpp
modules/function/render/transport/test/verify_r2_fix.py
```

Root/product CMake and bootstrap are unchanged. Core, lux-cxx, Legacy, Engine,
Scene/ECS, Editor/UI, Graph, Vulkan, Runtime and Feature implementations are unchanged.

## Reply ownership and concurrency

The user has explicitly accepted pre-reserved completion cells. The new public
operation is `RenderReplyInbox::abandon(ticket)`. A numeric ticket identifies one
reception obligation; copies alias that obligation. The receiver must poll to a
terminal result, including a cancellation error, **or** explicitly abandon.
Losing the ticket without either action is a caller lifetime/capacity leak, not
normal backpressure. The inbox owns the arena; tickets do not hold per-request
shared ownership. Inboxes and deferred promises can safely outlive Transport.

Packet reset/destruction or deferred promise destruction cancels the operation
but preserves a readable terminal result. It does not silently abandon reception.
An unread completed/cancelled result legitimately retains capacity until the
receiver consumes or abandons it. Abandon waives reception; it does not undo the
side effects of an already accepted operation. Future domain owners must retain
their own execution/resource lifetime responsibilities.

| Race / state | Linearization and reclamation |
| --- | --- |
| Pending abandon vs completion/cancellation | CAS either advances generation directly or sees the writer's WRITING claim |
| Abandon while WRITING | CAS to ABANDONED_WRITING; no byte access or early recycling by the receiver |
| Writer publication vs abandon | Publication CAS either exposes VALUE/ERROR or detects ABANDONED_WRITING and recycles after its final byte access |
| Completed/Cancelled abandon vs take | One CAS wins; a reader owns READING until its copy/error extraction finishes |
| Stop vs completion | Existing PENDING -> WRITING claim permits only one terminal writer |
| Stop vs abandon | Same generation/state arbitration; stop cannot overwrite an abandoned/reused slot |
| Old promise completion/destruction vs new reservation | Every claim includes generation; stale CAS fails before accessing cell fields |

FREE publication is release-ordered after prior cell accesses; reservation claims
the next generation before initializing its fields. Generation exhaustion retires
the cell instead of wrapping. WRITING storage cannot be reused prematurely.
Abandon follows at most three strong CAS attempts across PENDING -> WRITING ->
terminal; it does not spin waiting for a writer or take a mutex. Production adds
no heap allocation or shared-owner increment. Normal completion adds one atomic
publication CAS so the writer can safely accept reclamation responsibility.

Foreign tickets return `kTransportWrongOwner`; stale, consumed or abandoned
tickets return `kTransportReply`. A concurrent reader claim returns
`kTransportBusy` without discharging reception. The receiver should serialize
poll/abandon for a logical request. A not-ready poll retains its obligation.
`Packet.hpp`, `Transport.hpp` and generated operation traits were checked and need
no API/layout change. Generated traits match the prior output after checkout-path
normalization. Original consumer tests remain unchanged.

## Independent qualification and new tests

Source: `D:/LuxQualification/render-v2-r2-fix-4ae55059cca9/source`.
Build: `D:/LuxQualification/render-v2-r2-fix-4ae55059cca9/build`.
Both the implementation checkout and independent clean clone passed tracked
snapshot validation before/after qualification. Qualification made no source fix.

Windows x64; MSVC 19.44.35228.0 / toolset 14.44.35207; CMake 4.1.2; Ninja 1.11.1;
C++20 RelWithDebInfo `/EHsc /DNDEBUG`; Python 3.13. The sole package prefix is
`E:/SyncForder/CodeRepos/install/Framework-v2-dependencies` with no Engine SDK
headers. Package registries and normal Engine SDK/build PATH entries are excluded.
lux-cxx source: `bf779515a120350c7c5412c366eea73afdf59ccf`; toolset source:
`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`. Nine consumed generic headers match
source; package files, generator executable and reflection DLL hashes are archived.

| Check | Result |
| --- | --- |
| Full `all -j 4 -- -k 0` | PASS, 34 steps |
| Second full build | PASS, `ninja: no work to do` |
| CTest | PASS, 27/27, zero failures; original 23 preserved |
| Six Transport + five Core standalone header TUs | PASS |
| Actual CMake source/include/link closure and compiler deps | PASS, 195 compiler input paths |
| Codegen closure and trait completeness | PASS, 129 generator input paths; existing luxop tool reused |
| Test hook isolation | PASS, define only on `render_transport_reply_lifetime`, absent from production and benchmark |
| Scope / frozen Core and Legacy | PASS, Git object identity and 719/719 blob/mode/size |
| Historical R0/R1/R2 qualification facts | PASS, unchanged Git objects |
| Original user checkout | PASS, HEAD, staged/unstaged patches, status and six file-byte hashes unchanged |
| Three-prefix header synchronization | PASS, 18 exact copies checked; only Replies.hpp changed in each prefix |

New tests and raw results (`ctest.xml`, `07-ctest.log`):

| Test | Cases | Result |
| --- | --- | --- |
| `render.transport.reply_lifecycle` | replies=1; 10,000 cancel/reset/abandon/reuse cycles; unread completion retains capacity; foreign/duplicate/stale tickets; pending abandon and late completion; promise destruction; inbox/promise after Transport stop/destruction | PASS, 0.03 s |
| `render.transport.reply_writing` | Real completion paused after claiming WRITING; abandon succeeds but reservation still fails until writer exits; next generation accepts no late completion | PASS, 0.01 s |
| `render.transport.reply_cancelling` | Same deterministic schedule for cancellation through promise destruction | PASS, 0.01 s |
| `render.transport.reply_races` | 1,000 rounds of completion/cancellation vs abandon, concurrent stop, followed by reuse/late-generation checks | PASS, 0.26 s |

The deterministic pause is compiled only into an internal test executable's copy
of Replies.cpp; it links the remaining normal Transport archive objects. It is
not a public hook or alternate production protocol. The ordinary tests still
exercise production code without instrumentation. No sanitizer result is claimed.

## Same-machine before/after performance

Intel Core i7-13700KF, 16 cores/24 logical processors. The unchanged ten-case
benchmark runs five alternating before/after pairs with affinity mask `0xffff`
and above-normal process priority. Each case performs one million operations.
Sequential cases warm up 10,000 operations; contention uses four producers and
two consumers whose threads and scratch storage exist before measurement.
Capacities, fixtures, compiler configuration and benchmark source are unchanged.

Before executable is the independently qualified clean R2 I build; its source
remains clean at `c33ae729016ed7cfbee1218d660dedb59a327157`. After is clean R2-FIX I.
Both binary hashes, all ten raw trial logs and the comparison script are archived.
The table uses the median of five trial p50/p95 values; each trial percentile is
of 1,000-operation batch-average ns/op, not individual request latency.

| Case | p50 before -> after | p95 before -> after | p95 change |
| --- | ---: | ---: | ---: |
| V1 storage primitive reference | 5.6 -> 5.6 | 5.6 -> 5.6 | 0.00% |
| Empty candidate reset | 6.3 -> 6.3 | 6.4 -> 6.4 | 0.00% |
| Empty PROGRAM submit/dispatch | 66.6 -> 66.9 | 68.1 -> 68.8 | +1.03% |
| POD encode | 33.5 -> 33.5 | 34.3 -> 34.9 | +1.75% |
| Bulk 256 bytes | 34.9 -> 34.1 | 35.3 -> 34.9 | -1.13% |
| Blob 256 bytes | 38.2 -> 38.0 | 38.2 -> 39.3 | +2.88% |
| PROGRAM submit/dispatch | 111.1 -> 112.6 | 115.0 -> 115.6 | +0.52% |
| CONTROL request/reply | 195.8 -> 199.7 | 203.7 -> 209.0 | +2.60% |
| Full PROGRAM retained retry | 43.6 -> 43.2 | 44.7 -> 43.9 | -1.79% |
| UPLOAD MPMC per-producer batches | 3075.8 -> 2942.9 | 5169.5 -> 5328.8 | +3.08% |

All 100 case samples recorded **0 C++ allocations / 0 allocated bytes**, including
aligned new. The maximum median p50/p95 regression is 3.0816%, below the 5% gate.
The request/reply path now performs an additional publication CAS; no speedup is
claimed. Cold assembly and OS/CRT allocations are outside this measurement, and
these numbers do not qualify GPU/frame-time or full V1/V2 rendering performance.

## Authoritative architecture amendment locations

Locations below refer to implementation I and are independently reviewable:

| File and exact location | Contract |
| --- | --- |
| `03_CORE_AND_TRANSPORT.md:333`, section 13 | Approved pre-reserved reply exception, consume/abandon/writer reclamation |
| `05_BACKEND_RUNTIME_FRAME_TARGET.md:336`, section 18 | Independent Render Progress; pacing, persistent state, separate time, Graph three-part boundary; R5 real GPU gates |
| `07_RENDER_SYSTEM_PROJECTION_RESOURCE.md:372`, section 21 | Simulation/Publication/Rendering responsibility separation; revisions, backpressure and R8 frequency test |
| `09_IMPLEMENTATION_PLAN.md:88` | R3 topology/plan/bindings reuse and no FrameLoop |
| `09_IMPLEMENTATION_PLAN.md:137` | R5 real independent backend progression, stop/failure/in-flight gates |
| `09_IMPLEMENTATION_PLAN.md:194` | R7 one DebugColorData publication supports multiple frames |
| `09_IMPLEMENTATION_PLAN.md:213` | R8 low-frequency Simulation, higher Render target, retained revisions |
| `10_LLM_CONTRACT.md:7` | Highest-priority global Simulation/Render Independence HARD GATE |

These are authorized revisions to five architecture chapters, not retroactive
changes to R0 import hashes or historical qualifications. No clock/interpolation
framework, second renderer, frame loop or future-stage target was implemented.

## Evidence and handoff

Evidence directory:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R2-FIX/4ae55059cca949178a8a1f2eaffe32f720384e8d/`.
Manifest: `EVIDENCE_MANIFEST.json`, 213 files including 13 command/exit-code
records, raw tests, paired benchmarks, File API, generated traits, dependency
hashes, frozen-object comparison, install backups/synchronization and user snapshots.
Manifest SHA-256:
`7a1001b62bb486376ea56f7293b574df57d94977d8716324d0876f6d305d835a`.

NOT_RUN: GPU, product builds, V1 full matrix, Android, Linux, sanitizers, installed
SDK, cross-DLL duplicate-static-component interoperability and final rendering
performance comparison. Generic error header installation remains a later SDK
closure obligation. Existing Core/Legacy limitations were not reclassified.

R3 may begin only after explicit user review approval of this I/V pair. Its work
order must remain logical Graph-only, with no Vulkan/Runtime/Scene dependency or
FrameLoop, and include the new topology-versus-bindings gates. No additional R2
production fix is identified by this qualification.

```text
R2_FIX = PASS
R2_FINAL = PASS / implementation qualification, pending user review
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_STAGE = R3 only after explicit user authorization
STOP
```
