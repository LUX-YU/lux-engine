# IPC convergence checkpoints

I0 baseline is recorded separately. I1 `0342794fc66e17408d5b3b0035443d8b37e3a290`
was checked in `i1-source`: full all build, second no-work, 3/3 affected Runtime tests
including relocated installed consumers. Evidence `E:/RenderV2Rebuild/IPCConvergence/evidence/I1`.
Legacy and four user files unchanged. Exact I1 public header backed up/synchronized to three SDK prefixes.

## I2 implementation boundary

IpcMessage is 128 B / align16, SBO remains80 B. Private template move/destroy table only.
No pin, public TypeOps, ABI negotiation, erased external construction or automatic original return.
Requester receives IpcMessage directly. Reply full retains both ReceivedRequest and actual result.
ReceivedRequest reset ends transport use and does not manufacture a business outcome.

This intermediate checkpoint intentionally builds real IPC/Core/Data/Protocol, not unmigrated Runtime.
External checkpoint CMake selects actual components and enables all their tests, with no stub targets.
Runtime source consumers are migrated in I4; this checkpoint is not a full-product PASS.
I3 alone changes queue storage. Channel/Link/name retirement algorithms remain unchanged in I2.

### Behavior migration

- Lifecycle, unique Core across two DLLs, reader grace/requeue, final reply-before-close, publishing/close
  overlap, retained source, exact destruction, FIFO, two-client progress, assets, metadata, source routing,
  independent installed headers, relocated SDK and compatible DLL allocation/deletion remain tested.
- Canonical ABI/storage-policy negative plugins: SUPERSEDED by same-controlled-build precondition.
  `AbiPlugin.cpp` removed; actual module-specific allocation/deletion and host-ordered unload remain tested.
- Pins: SUPERSEDED; module stays loaded until all objects/destructor stacks return, explicitly tested.
- Automatic original_request/ABORTED and reply-credit admission: SUPERSEDED. Input dies after completed
  business use. Explicit business results survive roles. Failure retains both inputs for publication retry.
- No invented wrong-T runtime rejection: get<T> correct type is the trusted C++ caller precondition.
  Invalid command identity remains an independent protocol boundary test.
- Historical performance unchanged. Current large-asset handoff includes server-side destruction;
  it is not equivalent to historical automatic-return timing. No speed claim follows from smaller sizeof.

Implementation attempts and raw samples are retained in `evidence/I2`; initial fixture compile failure
(incorrect SlotKey bool conversion) and three obsolete-contract assertions were corrected in source.
All 12 IPC tests passed before the final move-counter instrumentation cleanup. Independent exact-SHA
verification follows this implementation commit; do not infer full Runtime qualification from IPC tests.

## I2 independent checkpoint / I3 implementation

I2 `65be2c1749ebb2d4f61d0c441c227c5efafeee19`: no-hardlinks exact clone `i2-source`, tracked snapshot,
Release and ASan all builds, second no-work, 12/12 IPC tests in each, relocated consumers included.
Evidence `evidence/I2`; ASan error=0. Whole Runtime deliberately not qualified at this intermediate boundary.

I3 replaces the private per-Link queue storage, retaining Channel/name lifecycle and source routing.
Initial segment size is not queue capacity; request/reply grow independently, with recoverable allocation
failure before message move. Consumer release gates producer reuse; immutable ownership chain frees all
segments only at cold Link destruction. No shared business queue, per-message reference count or lock.

Targeted tests now inject allocation failure instead of expecting a fixed-ring admission limit.
They retain exact source/result/backing assertions and explicit retry. Backlog257 with initial size4,
20 repeated cycles: 130 live segments (both directions), zero further EXE/DLL allocations. A controlled
consumer pause before recycle rejects producer growth under injected OOM, then permits reuse without
allocation after release. FIFO test streams one million requests/replies. All12 source IPC tests passed.
Independent I3 Release/Debug/ASan, repeated concurrent stress and comparable I2/I3 cost runs follow commit.
This is an IPC gate only; full Render API consumers remain the next authorized I4 migration.
