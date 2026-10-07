# Object execution bridge

`lux-engine-process-object-execution` installs the STATIC `object_execution` component. It composes the existing
Process senders with core Object lifetime endpoints; Object itself has no Process/stdexec dependency.

Create `objectScheduler(target)` on the Object owner thread. It retains the target's existing ObjectState, not the
LuxObject. Starting its schedule sender may happen on a worker. A live target completes with `set_value` on the owner
thread, under the same callback protection as Object events/signals. Only the immediate synchronous continuation may
borrow that target; worker inputs and any later asynchronous work must own their data.

A target destroyed or closed before delivery completes with `set_stopped`. Receiver cancellation before start does
not enqueue; cancellation after acceptance marks only the operation and waits for the original queue delivery.
A full queue completes immediately with `EExecutionError::CAPACITY_EXCEEDED`, without retry or waiting.

As with other stdexec operation states, the caller must keep a started operation alive until its terminal completion.
The receiver may destroy that operation from the completion callback. No operation fields are accessed afterwards.
Only `set_value` advertises ObjectScheduler as its completion scheduler: rejected/stopped starts can complete on the
calling thread.

The host still stops and joins Process/Renderer producers before process ObjectRuntime shutdown. Accepted target
envelopes remaining at shutdown complete with nullptr/stopped on the owner; ordinary queued signals remain discarded.
This does not change TaskScope's existing blocking destructor or the current Project transition protocol. Those are
separate review-gated stages in the terminal architecture specification.
