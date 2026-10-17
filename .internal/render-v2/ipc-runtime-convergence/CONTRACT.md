# I1: direct-message contract and implementation boundaries

This contract records the supplied R0 decisions, not a new transport design. Implementation and validation
are pending. IPC is trusted, same-process, same approved C++ build. Module unload is a Host precondition.

## Public usage

Host creates the selected Vulkan Server with its existing ExecutionRuntime and Native device options,
registers Storage/Feature implementations, obtains `server.commandId<ListCommands>()`, then starts run().
The query searches the existing unique Command table at cold installation time. Each caller gets this
real SlotKey and opens its own IpcRequester. Query replies contain the existing command directory values.
Client code stores those IDs and constructs ordinary messages:

```cpp
IpcMessage input;
input.emplace<SetTexture::Request>(std::move(value));
input.setMetadata(RequestHeader{commands.set_texture, RequestId{sequence++}});
auto sent = requester.trySend(input);
// Failure retains input; success transfers it. Caller owns correlation and timeout policy.

IpcMessage result;
if (requester.tryReceive(result) == EIpcReceive::RECEIVED)
{
    auto header = result.metadata<ReplyHeader>();
    // Caller knows the expected command for header.request.
    if (header.outcome == ECommandOutcome::SUCCESS)
        consume(result.get<SetTexture::Response>());
    else
        report(result.get<RenderError>());
}
```

RequestId is a strong uint64, zero invalid. CommandId remains a SlotKey; ordinary messages never select a
Handler directly. Selecting an active Handler affects dispatches after that owner-thread change, not work
already accepted. Different Links may use the same RequestId. A rebuilt Server requires fresh connections
and directory discovery. No null-id Hello, canonical proof, implicit client Pending or per-message budget.

Request is destroyed when business use ends; it is not an automatic response attachment. An application
wanting reusable storage must explicitly define it in that command's Response. A closed connection says
nothing about an unobserved side effect; the application marks its own outstanding operations unknown.
Existing Native uncertainty after accepted work must not be relabeled as a guaranteed cancellation.

## Reply ownership and allocation failure

ReceivedRequest remains source-qualified under the responder lifetime precondition. A server RenderRequest
is its sole business owner. Sync dispatch does not allocate a Pending slot; true async promotion moves that
owner into an existing stable DeferredCompletion. Original input borrows are reacquired after that move.
The existing retained result field owns the actual response if tryReply cannot grow the reply queue.
The owner tick retries publication only; it does not repeat Handler invocation or business commit. Closed
source ends delivery and releases the result after GPU/CPU use is safe. Reply publication consumes route
and result once. Stop settles all source borrows before resetting Responder.

Temporary reply-queue allocation failure is recoverable. Ordinary C++ payload allocation retains the
project fatal-OOM policy. There is no promise of unlimited memory or successful allocation under pressure.

## Queue migration order

I2 first simplifies objects/replies on the fixed SPSC algorithm. Reply publication may now report full
without consuming its two inputs; credit removal is necessary when automatic aborted/original replies end.
This is a temporary IPC-only test checkpoint, not a claim that the unmigrated Render consumers compile.
No compatibility aliases or hidden original-return envelopes bridge that source break.

I3 then replaces queue storage with internal reusable Segments; Link remains its unique owner. The producer
publishes constructed slots with release; consumer observes acquire before moving/destroying them. A full
tail publishes a next segment only after allocation or safe reuse succeeds. Consumer releases an exited,
empty segment only after its last access; producer observes that release before reusing it. Queue storage
is physically destroyed only after participant/source lifetime protection excludes both accesses. Initial
segment size is not total capacity. The existing Channel/name reader grace and one-consumer roles remain.

An internal recycling link is not a second message owner. Segment growth uses explicit fallible allocation
and overflow checks; a failed allocation does not move the input. Test hooks control failures and overlap
without sleep/yield correctness patches. FIFO, steady reuse and one million streaming messages are required.

## Async notification and registration

Growing Pending records must not outrun a fixed completion ring. Keep the existing unique stable slots
and notification owner, with pre-acquired notification storage in each actual async slot. Only completed
records enter the ready chain. Existing short completion-publication synchronization remains outside IPC
SPSC. The owner removes a ready link before invoking its continuation; callback may destroy that slot.
No work scan, dropped completion, second operation owner, scheduler or IPC business queue is introduced.

One command table, one Handler owner table, actual StorageSets and DeviceSession remain. Move the selected
Native bootstrap and provider template definitions out of the generic Server header; do not create another
device owner. Existing code pins are deleted after all consumers move to Host-managed module lifetime.
Factory erased operations become immutable after typed construction. Builtins directly consume typed
arguments/replies or the accepted server-private request; they do not recover it through ReplyAccess.

These are changes inside existing owners or explicitly authorized queue records. If source implementation
requires another lasting owner/type/state machine, section 11 requires a concrete design-change report and
stop before production adaptation. This document grants no exception to that stop rule.
