# Architecture convergence R3 implementation

Parent: `12f0221597f2ccca9344353cfc0b49bc1c27695e` (R2).

Production ERenderCommand, RenderCommand and fixed control Record are deleted. The Server has exactly one
SlotMap<OperationEntry> allocating OperationId for builtins and Feature handlers. Each entry contains its ID,
static descriptor, function and borrowed user context. Registration checks full contracts/duplicates before
mutation, then freezes at run. Hello is a protocol bootstrap (null operation), not a business enum. Its manifest
is derived once from that owner. Client bind compares full canonical metadata, not hashes, and produces a
BoundOperation<Request, Reply>. Its validated descriptor cannot be replaced by callers; session/ID remain
explicit values and are checked at the server's untrusted boundary. RequestId stays connection-local.

Operations.hpp/.cpp contain explicit G2 codecs for the actual builtin requests/replies, without a second parser,
reflection system or universal payload. The request frame is 48 bytes, terminal header 68 bytes. A 4-byte control
payload with a reply bound to 128 bytes uses IPC SBO; encoded/retained reply reservations stay in pending metadata
until consumption, then move into Completion. Large requests retain one unique Input envelope plus existing
asset pins. Both forms preserve outstanding budget across abandon and retained Completion lifetime. Reply
publication failure retains the owned output for cold disposal; it does not destroy a large output in the loop.

RenderRequest owns the original source-qualified ReceivedRequest. Normal handlers use the stack value; Readback
moves it into the existing bounded pending vector. Heavy Feature assembly alone promotes its real input for G2
owned references. There is no per-request shared Record or Reply. External Feature handlers are synchronous in
R3; they must complete/fail before returning. Runtime owns all bounded deferred continuations. Returning without
completion becomes an explicit failure. The server clears deferred/undelivered inputs before its Responder.

Minimal necessary G2 extension: BorrowedWireView, encodeWireInto and decodeWireBytes permit real inline messages
without fabricated SharedBytes ownership or payload copies. Owned and borrowed validation share one internal
field/range algorithm. validateBoundWire has the explicit precondition of cold-validated immutable schema; the
Runtime uses it only after registration/binding. Wire values, lengths, references and limits are still checked.
G2's owned API and lifetime contracts remain. New parity tests mutate every byte of blob/external records,
compare owned/borrowed rejection, and cover short/long/failed codecs, truncation, tails and limits.

The independent test Feature defines SetExposure and UploadCustomData. Runtime source contains neither name nor
special dispatch. Tests cover equal RequestId across two clients, wrong generation/session, schema/version
mismatch, invalid reply length, registration freeze, raw malformed payload, underfunded raw reply -> IPC ABORTED,
full 1 MiB backing accounting for a 64-byte view, unchanged asset address and completion after Client/Server death.
The old behavioral scripts retain all business/GPU assertions via a TEST-ONLY ControlFixture; it is not installed
or used by the new performance fixture. No production forwarding compatibility layer remains.

## Implementation verification

Runner: `E:/RenderV2Rebuild/ConvergenceR1/run.ps1`.

- Release + ASan full `cmake --build <build> --target all -j 4 -- -k 0`, second build no-work.
- Full `ctest --test-dir <build> --output-on-failure -j 1 -R "^(render|platform)\."`: 60/60 each.
- Final raw-peer/timing-correlation additions and underfunded-frame handling: rebuild both configurations,
  second no-work, affected Runtime 7/7 each. No ASan or Vulkan/SyncValidation error.
- Standalone headers and relocated installed Client/Runtime consumers included. No Vulkan include leaks into
  Client. Native/Graph/Feature/IPC production files unchanged in this work block; Legacy719/user/history PASS.
- Initial fixture compile failed by inheriting final Completion; corrected to composition. A test source name
  collided with the generated Operations.hpp probe; renamed FeatureOperations.cpp. Logs retained, no test disabled.

Raw performance CSV: `E:/RenderV2Rebuild/ConvergenceR1/evidence/R3-operation/` (1000 samples, Release).
The test binds once, encodes every request, executes the actual server handler, encodes every reply and decodes
consumed results. Handler sample publication is correlated before recording it; it cannot read the previous
request's timing. Existing 1 ms service-loop sleep and OS scheduling remain in the wait column.

| Measured interval | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| Submit including encode | 600 ns | 1400 ns | 1900 ns | 14200 ns |
| Wait + consume | 1981900 ns | 2151800 ns | 2550800 ns | 2662400 ns |
| Successful consume + typed decode | 1000 ns | 3200 ns | 20700 ns | 85600 ns |
| Handler decode + reply publication | 1100 ns | 3900 ns | 6600 ns | 38000 ns |

All 1000 samples have zero engine-side new calls across participating Client and Server threads. This does not
instrument driver/OS internals; unchanged IPC DLL inline allocation evidence remains the separate R2 measurement.
The whole operation is not a microsecond latency claim: the host loop wait dominates this configuration. Old
ControlFixture timings include cold binding and are explicitly not the new bound API cost.

R3 implementation gates pass. Independent final Debug/Release/ASan V, async compilation and Compiler optimizations
remain R4-R6. Root TOOLCHAIN remains separately BLOCKED; IPC historical PARTIAL is unchanged. Linux/Android/TSan
NOT_RUN and GPU timestamps NOT_MEASURED.
