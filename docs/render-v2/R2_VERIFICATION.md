# R2 — Transport Foundation qualification

## Decision and identity

**R2 overall: PARTIAL — one specification decision remains open.** The independent
technical checks below passed. This report does not grant R2 acceptance or R3
authorization. The reply algorithm difference must be accepted explicitly or
replaced and requalified; passing tests does not waive chapter 03 section 13.

- Branch: `codex/render-v2`.
- Accepted R1 base: `14319331d616dbfe0318352857cf34676df6b564`.
- R2 implementation I: `c33ae729016ed7cfbee1218d660dedb59a327157`.
- Frozen V1 source: `a669409a289a6fa4092f21176397795b1cdb7f3e`.
- Date: 2026-10-10. The subsequent verification commit V adds only this report.
- Implementation scope: 28 files; Transport, bootstrap entry and R2 work order.
  Core, Legacy, architecture chapters, root/product build and consumers are unchanged.

## Open specification decision

[03_CORE_AND_TRANSPORT.md](03_CORE_AND_TRANSPORT.md) section 13 says to migrate,
not rewrite, the mature algorithms and specifically lists `pending reply retry`.
The current implementation instead reserves a bounded completion cell **before**
appending a request. An accepted request therefore already owns response capacity;
unconsumed completion data remains in its cell. Capacity pressure occurs before
acceptance. This is an algorithm replacement, not a literal migration of V1's
response ring and retained pending-publication retry.

The replacement was identified during self-review and presented to the user as a
concrete choice after development tests. No acceptance has been received as of
this report. The authoritative chapter remains unchanged. Choose either explicit
acceptance of the reservation design or restoration of the V1 algorithm followed
by a new implementation commit and independent qualification. The implementation
README records the same difference; neither this report nor the work order
silently authorizes it.

## Delivered contracts

Six public headers under `lux/engine/render/transport/`:

| Header | Public surface |
| --- | --- |
| `Error.hpp` | Static error descriptor span and twelve protocol error IDs |
| `Operation.hpp` | Lane/kind, owned value constraints, blob/attachment references, operation metadata |
| `Routes.hpp` | `RenderRouteTable`, `BoundRenderRoute<T>`, local route index/generation |
| `Replies.hpp` | Typed ticket, inbox, move-only deferred promise, epoch wake |
| `Packet.hpp` | Typed move-only lane packets, owned bytes, borrowed consumer view |
| `Transport.hpp` | Bounded submission/consumption, stop and drain observation |

Owner identity is checked before local slot/generation access. Two simultaneous
instances with identical local IDs reject each other's routes, removals, packets
and replies. Moving a table clears the source identity, preventing reuse to create
a second Transport with the same scope. No Core handle or global registry changed.
Registration is cold and frozen at transport creation; live Feature installation
is not implemented. Independently duplicated static components across DLLs are
outside the qualified linkage configuration and need an explicit later SDK policy.

PROGRAM/CONTROL enforce one producer thread and one shared consumer thread.
They reuse `BoundedSpscFrameRing`; UPLOAD adapts the V1 bounded ring/byte accounting
with try-lock admission and concurrent consumers. Busy/full/budget errors preserve
the candidate. Accepted storage, copied blobs, retained attachment backing and
reply completion have explicit independent owners. Stop closes `AdmissionGate`,
wakes waiters and competes atomically with completion. Hosts must join callers
before destruction. Lanes have no total order: the CPU readiness test deliberately
consumes PROGRAM before an earlier UPLOAD, then waits for its receipt before the
next publication. GPU resource readiness/retirement itself is not implemented.

The existing luxop parser and generator produce field ownership checks and typed
operation/reply traits. There is no new parser, per-message string lookup,
lane/kind switch, virtual Feature hierarchy or Runtime-like host. Normal inline
packet/reply operations use pre-reserved storage and no shared-owner increments.
Cold packet creation, explicit attachment pinning and deferred completion remain
separate ownership operations. See the component README for algorithm provenance.

## Independent execution and closure

Source: `D:/LuxQualification/render-v2-r2-c33ae729016e/source`.
Build: `D:/LuxQualification/render-v2-r2-c33ae729016e/build`.
Both source and implementation checkout passed `ValidateTrackedSnapshot.cmake`
before/after qualification. No implementation file changed during qualification.

Windows x64, MSVC 19.44.35228.0 / toolset 14.44.35207, CMake 4.1.2, Ninja 1.11.1,
C++20 RelWithDebInfo, `/EHsc`, `/DNDEBUG`, Python 3.13. Sole package prefix:
`E:/SyncForder/CodeRepos/install/Framework-v2-dependencies`, containing no Engine
SDK headers. Package registries and normal Engine SDK/build PATH entries were
excluded. lux-cxx source: `bf779515a120350c7c5412c366eea73afdf59ccf`;
toolset source: `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`.
Nine consumed lux-cxx headers match source after CRLF normalization. Package files,
generator executable and its three reflection DLLs are hash-indexed; the generator
was reused, not independently rebuilt in this qualification.

| Check | Result | Evidence |
| --- | --- | --- |
| Clean tracked I / independent clone | PASS | `01`, `04`, `10`, `11` command logs |
| MSVC/Ninja configure | PASS | `05-configure.log` |
| Full `all -j 4 -- -k 0`, first round | PASS | `06-build-1.log`, 31 steps |
| Second full build | PASS | `06-build-2.log`, `ninja: no work to do` |
| Standalone public-header compilation | PASS | 6 Transport + 5 unchanged Core TUs |
| CTest | PASS | 23/23; 8 unchanged Core + 15 Transport |
| Thread/concurrency/lifetime contracts | PASS | SPSC FIFO, 4p/3c exact-once UPLOAD, byte retirement, deferred replies, stop races |
| Compiler negatives | PASS | 13 total; Transport rejects lane/kind/payload/pointer and Vulkan/Runtime/Scene inputs |
| Generated metadata completeness | PASS | Six operation traits, one reply trait, seven ownership assertions |
| Real source/include/link/codegen closure | PASS | File API, graph, 195 compiler header paths, 129 generator input paths |
| Mature reply algorithm migration | PARTIAL | Replacement described above; acceptance not inferred |
| Frozen Legacy mode/blob/size | PASS | 719/719 files; 720 entries with guard |
| Three-prefix public-header sync | PASS | 18/18 exact copies from clean I, all destinations previously absent |
| Original user checkout | PASS | Same HEAD, staged/unstaged patches, status and six byte hashes |
| Bootstrap install boundary | PASS | No production install target; no installed SDK qualification claimed |

Transport links only Core plus generic concurrent headers. Production Engine
includes are Core, Transport and generic error value headers; the synchronized
ErrorRegistry is absent. Test/codegen additionally consume the existing generic
annotation header, without a type_info runtime link. No legacy, old Engine SDK,
Vulkan, Scene, Graph, Runtime or Feature implementation supplies the build closure.

## CPU transport baseline

Same machine: Intel Core i7-13700KF, 16 cores / 24 logical processors. Each case
has 1,000,000 operations; p50/p95 are **1,000-operation batch-average ns/op**.
Raw maximums and allocation bytes are also archived. Sequential cases warm up
10,000 operations; MPMC uses four producers/two consumers with worker and scratch
construction before a start barrier. Default packet limits are 256 records,
64 KiB copied bytes and 8 attachments; queues are 2 PROGRAM / 2 CONTROL / 64
UPLOAD, 256 reply slots and a 256 MiB UPLOAD byte budget.

| Case | p50 ns/op | p95 ns/op |
| --- | ---: | ---: |
| V1 storage primitive reference | 5.9 | 6.0 |
| Empty candidate reset | 6.3 | 6.4 |
| Empty PROGRAM submit/dispatch | 65.7 | 71.2 |
| POD encode | 33.9 | 38.3 |
| Bulk 256 bytes | 34.1 | 34.9 |
| Blob 256 bytes | 38.5 | 38.7 |
| PROGRAM submit/dispatch | 110.6 | 117.1 |
| CONTROL request/reply | 196.5 | 229.7 |
| Full PROGRAM retained retry | 43.0 | 43.9 |
| UPLOAD MPMC per-producer batches | 2984.0 | 4743.2 |

MPMC aggregate wall time was 774.371 ns/op. All ten measured cases recorded
**0 C++ heap allocations / 0 allocated bytes**, including aligned allocation.
This excludes cold assembly and OS/CRT internals; it is not a frame/GPU allocation
claim. The V1 primitive performs less work than the complete V2 protocol; these
numbers do not establish a V1/V2 percentage regression or improvement. An equivalent
full-stack before/after comparison, CPU branch profile and product performance
remain NOT_RUN. This establishes a reproducible R2 microbenchmark baseline.

## Evidence and stop

Evidence directory:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R2/c33ae729016ed7cfbee1218d660dedb59a327157/`.
`EVIDENCE_MANIFEST.json` indexes 180 files, including 13 command/exit-code records,
raw tests/benchmarks, CMake graph and File API, compiler/codegen dependencies,
generated traits, package/header hashes, header synchronization and user protection.
Manifest SHA-256:
`6bf9c152da29ced975903a5b9fdbac512f7bd918b68c27ef9f0c8c84d81e9763`.

NOT_RUN: V1 full matrix, GPU/render behavior, ASan, Linux, Android, installed SDK,
cross-DLL duplicate-component interoperability and equivalent full-stack V1/V2
performance. Generic error value header installation closure remains deferred.
No historical qualification result or known V1 issue was changed.

```text
R2_TECHNICAL_CHECKS = PASS
R2_STAGE = PARTIAL / reply algorithm decision pending
V1_REFERENCE = REFERENCE_WITH_KNOWN_LIMITATIONS
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_ACTION = review reply algorithm; no R3 authorization
STOP
```
