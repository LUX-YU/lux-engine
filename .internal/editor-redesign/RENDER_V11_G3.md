# v11 G3 — S1 migration checkpoint

Parent G2: `1302d56aa8483a61537d5f8001e4624a543e7387`. This is an internal implementation gate;
the final independent S1/S2 verification has not run. S2 is not implemented at this checkpoint.

## Production migration

- The sole Server Command SlotMap replaces the previous Operation table. Thirteen builtins cover Scene/View create and
  remove, Target create/resize, Effect create/remove, attach/detach, readback, stop and stats. Texture has four neutral
  typed declarations (set/region/query/release); its professional handlers are in Native. External Buffer registration
  uses the same frozen provider seam without Server business branching.
- There is no public Operation/BoundOperation/submitEncoded or Command-specific Wire codec. Runtime Operations.hpp,
  Operations.cpp, Wire.hpp, Wire.cpp and its old TextureStorage.cpp were deleted in G2. Protocol now directly depends
  only on Core and lux-cxx core; a relocated pure Protocol consumer does not load Data or IPC.
- Remaining Data/Wire consumers are real author/configuration/binding contracts. In particular Native prepare validates
  Storage binding bytes once at attachment; that is not a Command decoder. Feature explicitly includes its configuration
  dependency. Data still owns immutable author bytes and typed RenderPayload asset ownership.
- IPC no longer exposes byte/attachment adapters, InlineBytes/OwnedBytes, or corresponding TypeOps callbacks. It owns
  a constructed C++ object, metadata and its code pin. Existing physical directory, SPSC, Link, credits and closeout
  sections remain byte-for-byte unchanged. Historical byte-fixture reports remain untouched and describe their own commits.
- All old IPC tests keep their names and effective ownership/ordering contracts; messages and assets now use concrete
  test values. The attachments test proves typed SharedBytes address identity, failed-send retention, deferred reply,
  ABORTED return and exactly-once final release. It does not reintroduce a production attachment transport.

## Layout decision and measured costs

All candidates retain the same 40-byte metadata, static TypeOps pointer and code pin. Actual production libraries were
rebuilt from three source snapshots differing only in object capacity. The measurement project uses the real IPC cost
fixture and the real Runtime FeatureOperations test, not a substitute queue. It is not a stage test qualification.

| Message / object capacity / Reply | 64-slot Request+Reply element arrays | 8/32/64/128-byte object allocations per roundtrip |
|---|---:|---|
| 96 / 32 / 208 B | 19 KiB | 0 / 0 / 2 / 2 |
| **144 / 80 / 304 B** | **28 KiB** | **0 / 0 / 0 / 2** |
| 208 / 144 / 432 B | 40 KiB | 0 / 0 / 0 / 0 |

Selected 144 bytes: both release and Debug vector Batch envelopes fit, including the real budget lease and code pin.
The 96-byte candidate fails the required common Batch SBO contract; shrinking away code lifetime protection is not an
option. The 208-byte candidate spends another 12 KiB per 64-slot Link to inline 128-byte objects. Such objects remain
supported with one external owner each, and their allocations are reported. Ring headers and allocator padding are
excluded from the table's precisely stated element-array footprint.

Experiment root: `E:/RenderV2Rebuild/V11/experiments/sbo/`. Source manifest, build/configure logs (including the first
measurement-project missing-export/GAPI failure), source snapshots, scripts and all CSV/outliers are retained. The
unused initial standalone Cost.cpp is not a source of reported results. Six alternating candidate/old-baseline runs
used candidate order 96,144,208,208,144,96 on the same i9-13900H/MSVC machine. Physical core masks 1 and 4 were discovered
by the actual fixture. Each candidate has 20,000 samples per object size and execution mode, and 2,000 full Command samples;
the prior exact-baseline executable has 6,000 interleaved full Command samples. CSV timings have 100 ns granularity here.
IPC roundtrip timing ends after result validation, before final Reply destruction; large-message release is measured
separately. These numbers are not a complete Owner teardown benchmark.

For the selected layout (nanoseconds, p50/p95/p99/max):

| Measured range | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| 32 B typed IPC, single thread | 200 | 200 | 300 | 62,000 |
| 64 B typed IPC, two physical cores | 600 | 800 | 900 | 357,200 |
| 128 B typed IPC, two physical cores | 800 | 1,000 | 1,000 | 756,000 |
| Full Client submit + construction | 300 | 800 | 1,200 | 13,700 |
| Server handler + reply publication | 600 | 1,800 | 5,200 | 10,200 |
| Successful completion consume + typed access | 500 | 1,800 | 8,800 | 121,100 |
| Waiting + successful consume | 1,991,300 | 2,055,200 | 2,268,600 | 3,175,100 |

The existing sleep_for(1ms), OS scheduling and waiting dominate the last row and are not IPC latency. The 96-byte
experiment also contains a 21.6922 ms two-core outlier; it is retained, not explained speculatively. These samples do not
establish a renderer throughput improvement. Small full Command EXE and Client DLL allocation counters are separately
zero. No claim is made that all object sizes, author encoding, Debug STL internals or Native work allocate zero.

The Batch fixture keeps 160 LOCAL samples and adds 160 RANDOM samples with fixed seed 0x61a7b3c9, both Scenes and
1/100/1000/10000 entries. Input/permutation preparation is outside timing; original vector backing/capacity is reclaimed
and reused. Invalid batches remain all-or-nothing, duplicates last-win, revision n does not acknowledge n+1, and Session
rebuild restores a snapshot. Actual EXE/DLL allocations and comparison with equivalent direct STL moves are preserved.

## Cross-module external allocation correction

A real EXE/DLL reproducer (large and over-aligned values with class-specific allocation provenance) found that public
bound emplace allocated the external T in the caller but deleted it through canonical DLL TypeOps. Both cases failed
origin assertions even though payload bytes and deallocation were otherwise valid. The Runtime erased move path already
allocated through canonical construct_from and passed both cases. Original failing sources, binary and log remain at
`E:/RenderV2Rebuild/V11/experiments/canonical-owner/`.

The bounded correction keeps inline construction unchanged and uses canonical construction for external bound values.
Bound construction requires a nothrow-movable T; local unbound emplace retains its existing immovable/throwing-move object
support because message movement only moves the external pointer. No extra owner/table or queue/lifecycle change is needed.
The permanent DLL test proves allocation and destruction use the same module, including over-aligned storage. External bound construction adds one real T move; it does not deep-copy backing. Inline and Runtime erased construction are unchanged.

This only establishes the outer object's allocation origin. Nested C++ resources require compatible CRT/allocator ABI;
plugin resources must retain their actual allocator/deleter and CodePin. Type metadata cannot prove arbitrary custom
allocator behavior or arbitrary object graphs. No claim to that effect is made.

## Gate status

Final selected-layout matrix and immutable evidence index are recorded below after execution. No S2 qualification is
inferred from S1. Native TextureUpload, existing finite Transfer and CPU/GPU completion scans remain the one explicitly
identified implementation to replace in G4/G5; they are not a second newly introduced subsystem.


G3 internal gate: PASS. Final corrected source was built with all -j 4 -- -k 0, rebuilt with no work,
and ran every test serially in RelWithDebInfo, Debug and MSVC ASan: **87/87 each** (83.61/83.87/109.30 seconds).
The original 79 test names remain. Public-header and relocated installed consumers are included in those builds/tests;
GPU Validation/SyncValidation and ASan report no errors. Existing real warning counts remain in the raw logs.
The new canonical DLL provenance assertions pass in all three configurations. Legacy 719, both user edits, historical
files, dependencies and exact frozen physical IPC source protection pass. Final independent V and SDK prefix sync remain
G6 work, not qualifications claimed here. Root TOOLCHAIN historical BLOCKED and IPC R2 PARTIAL are unchanged.

Evidence: `E:/RenderV2Rebuild/V11/evidence/g3-evidence-index.json`, 193 entries,
SHA-256 `3d45c5873c3bdf52f5750223e7d34610e1c68109c65ac9367048c7287d31cda3`.
This indexes raw build/CTest logs, pre-fix failure, final protection, per-configuration CSV copies, alternating SBO/Command
samples and source snapshot manifests. Historical failed/earlier attempts are preserved and not relabeled as final runs.
