# Render commands and owner-thread Server

Applications use Platform IPC directly with neutral `render_commands` declarations.
They choose a RequestId, encode the discovered CommandId with `commandTag()`, send
a typed Request and receive its declared typed Response. There is no RenderClient,
client pending registry or Command wire codec. Pure command consumers need neither
Vulkan nor stdexec. Server-only installation uses the selected Native provider.

`RenderServer::Impl` owns one `CommandRegistry`, `ResponseOutbox`, counting scope,
completion queue, Responder and DeviceSession. Scene owns View.
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
Response or asynchronous state is needed. Dispatch is not reentrant: a Handler may
install declarations but must not recursively dispatch or flush the Outbox.

An asynchronous thunk acquires a result delivery record before invoking the Handler.
Standard error/stopped adaptors normalize its concrete Sender to one typed Response;
`stdexec::spawn` alone owns and destroys the connected OperationState. The Server's
`counting_scope` supplies one shared stop token. No per-request stop source, Pending
identity table or type-erased operation deleter remains.

The record owns only routing, completed output and the existing completion queue node.
Native intermediate continuations borrow the same node without transferring operation
ownership. RenderThread alone moves finished output to Outbox and deletes the record.
Scope completion and IPC delivery are distinct; an operation may end before its result
can be sent. Host scheduler/module lifetimes still cover external callback tails.
Outbox alone publishes finished results, with bounded fair retries when a reply
segment cannot grow. A closed requester cannot hold ready output indefinitely.
The source route is independent of input ownership. Invalid CommandId or wrong
request type produces one raw RenderError(kProtocolContract); known commands
produce only their declared Response. Accepted uncertain effects use kOutcomeUnknown,
never a guaranteed cancellation.

## Native work and shutdown

CreateEffectSender connects a standard schedule/then/continues_on/then transaction;
spawn owns its connected state. Its worker only calls compileEffect with the moved
request, stable Feature borrows and Device. A private RenderScheduler borrows the
existing CompletionPost landing place and returns the candidate to RenderThread,
which alone allocates EffectId and produces success. There is no separate compile
Operation owner or CpuReceiver. Graph analysis/compiler and Native retirement are
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
