# Render commands and owner-thread Server

Applications use Platform IPC directly with neutral `render_commands` declarations.
They choose a RequestId, encode the discovered CommandId with `commandTag()`, send
a typed Request and receive its declared typed Response. There is no RenderClient,
client pending registry or Command wire codec. Pure command consumers need neither
Vulkan nor stdexec. Server-only installation uses the selected Native provider.

`RenderServer::Impl` owns one `CommandRegistry`, `ResponseOutbox`, stable pending
operation table, completion queue, Responder and DeviceSession. Scene owns View.
Effect owns its immutable CompiledGraph and four-field execution groups:
SceneId, ViewId, TargetId and ExecutionContext pointer.

## Registration and dispatch

CommandRegistry owns the CommandId table and HandlerId owner table. Each Handler
object is destroyed once; active command routes contain just `{invoke, object}`.
Cold registration compares complete declarations and installs typed member thunks.
Clients explicitly query ListCommands/ListCommandBindings and may select a binding.
Selection affects future dispatch, never an already-started operation.

`CommandMethod<C, &H::method>` accepts exactly one of these noexcept signatures:

```cpp
void H::method(const C::Request&, C::Response&) noexcept;
ConcreteSender H::method(C::Request&&) noexcept;
```

Responses use `RenderResult<Success>`. Synchronous dispatch creates an error-initialized
Response directly in an Outbox message before entering the Handler. No temporary
Response or PendingOperation is needed. Dispatch is not reentrant: a Handler may
install declarations but must not recursively dispatch or flush the Outbox.

An asynchronous thunk first creates a stable PendingOperation, then invokes the
Handler, connects its concrete Sender and starts the operation owned by that slot.
The Sender moves every source owner needed after the Handler returns. Its generic
Receiver maps value, RenderError, CommandFailure and stopped into one typed result
and publishes the existing completion event. RenderThread moves ready results to
Outbox, then releases the ended operation. Publication is the producer's final
state access; host scheduler/module lifetimes cover callback tails.

Outbox alone publishes finished results, with bounded fair retries when a reply
segment cannot grow. A closed requester cannot hold ready output indefinitely.
The source route is independent of input ownership. Invalid CommandId or wrong
request type produces one raw RenderError(kProtocolContract); known commands
produce only their declared Response. Accepted uncertain effects use kOutcomeUnknown,
never a guaranteed cancellation.

## Native work and shutdown

CreateEffectSender owns the complete compile transaction. Its worker only calls
compileEffect with the moved request, stable Feature borrows and Device. CompletionPost
returns the candidate to RenderThread, which alone allocates EffectId and produces
success. Graph analysis/compiler and the existing Native retirement algorithms are
unchanged. GPU upload senders retain source Payload through actual Transfer completion;
Copy/Acquire/publication determines success. Readback waits for its own GPU work.

Stop closes dispatch, requests operation stops, drains accepted CPU/GPU work and
publishes/discards ready output before resetting the Responder. Native objects retire
by real LastUse tickets before Device/Instance destruction. The host must stop and
join run before destroying Server, and keep its ExecutionRuntime and code modules alive
through accepted callback tails. Disconnection is not GPU cancellation.

Storage Factory installation remains explicit. Attach prepares the context once;
stable frames use prepared pointers without Feature dispatch, name/schema lookup,
Graph analysis or Shader/Pipeline compilation. Resource/Graph/Material architecture
and historical un-attributed performance qualifications are outside this refactor.

The single RenderThread alternates bounded request, completion and response work with ready View execution.
There is no unconditional sleep: successful work starts the next round immediately. Only a round with no progress
waits on IpcCore's epoch; requests, CPU completion and stop wake it. A requested 1ms idle deadline observes GPU fences
and unvisited Links in the bounded IPC roster. This is an idle observation deadline, subject to OS scheduling, not a rendering rate limit.
