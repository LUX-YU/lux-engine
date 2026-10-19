# Render commands and owner-thread Server

Applications use Platform IPC directly with neutral `render_commands` declarations.
They choose a RequestId, encode the discovered CommandId with `commandTag()`, send
a typed Request and receive its declared typed Response. There is no RenderClient,
client pending registry or Command wire codec. Pure command consumers need neither
Vulkan nor stdexec. Server-only installation uses the selected Native provider.

`RenderServer::Impl` owns one `CommandRegistry`, `ResponseOutbox`, counting scope,
Core execution::RunLoop, Responder and DeviceSession. Scene owns View.
Effect owns its immutable CompiledGraph. The Attachment table owns each actual
Effect/Scene/View/Target relationship; those objects keep non-owning relation indices.

## Registration and dispatch

CommandRegistry owns the CommandId records and stable HandlerId objects. Each command
has one declaration, a binding array and one selected array index; it does not cache
a second selected HandlerId or active call. Dispatch resolves CommandId and that index
directly, copying the selected `{invoke, object}` before invoking the Handler. This
also permits installation to grow the directory during a Handler call.

Full-name command discovery and Handler type/instance discovery use ordered indices.
Handler selection uses the ordered, append-only HandlerId bindings and verifies the
full generation. Individual Handler unloading is not supported; clear removes all
indices before destroying Handler objects. Built-in control calls remain unselectable.
A temporary RegistrationBatch collects new Handler objects/member calls, Storage factories
and Feature declarations. Installation checks the complete batch against existing entries
and against its other entries before publishing anything. Unpublished objects are destroyed
on failure. Existing Handler type/instance pairs are reused without reconstruction; explicit
Storage configuration wins over a Feature dependency's default configuration.

Native Providers explicitly implement `createHandler(DeviceSession&, ...)`, naming the
actual constructor arguments. Runtime never infers dependencies from constructor shapes.
The batch is discarded after installation; it owns no running work or persistent directory.
Publication performs no extension callbacks after the final recoverable capability/config
check. Host heap exhaustion remains fatal, including installation-time container growth.
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
Intermediate CPU continuations use the same Core RunLoop scheduler; result records do not
carry a second continuation protocol. RenderThread alone moves finished output to Outbox and deletes the record.
Scope completion and IPC delivery are distinct; an operation may end before its result
can be sent. Host scheduler/module lifetimes still cover external callback tails.
Outbox alone publishes finished results, with bounded fair retries when a reply
segment cannot grow. A closed requester cannot hold ready output indefinitely.
The source route is independent of input ownership. Invalid CommandId or wrong
request type produces one raw RenderError(kCommandContract); known commands
produce only their declared Response. Accepted uncertain effects use kOutcomeUnknown,
never a guaranteed cancellation.

## Native work and shutdown

CreateEffect directly returns a standard schedule/then/continues_on/let_value transaction;
spawn owns its connected state. Its worker only calls compileEffect with the moved
request, stable Feature borrows, qualified Storage names and immutable device facts. Workers never read the mutable ResourceRegistry. Program author names resolve once to GraphPassId indices. The same Core RunLoop that delivers completed
Replies schedules candidate adoption on RenderThread, which alone allocates EffectId.
The owner handoff cannot swallow a determined error on cancellation; successful candidates
check stop before adoption. No private RenderScheduler or CreateEffectSender remains.
Graph analysis/compiler and Native retirement remain independent. Upload tasks own the admitted request, while its Source span borrows caller memory until the typed Response. Failed enqueue preserves the caller message; accepted failure/cancellation does not return the original Request. After abandon/disconnect, the Host must drain Server and producers before releasing borrowed Source memory; unpublished candidates remain operation-owned and released physical objects move into retirement. Copy/Acquire admission and
professional publication determine upload success. There is no production Readback command.

Stop closes dispatch, requests operation stops, drains accepted CPU/GPU work and
publishes/discards ready output before resetting the Responder. Native objects retire
by Native queue completion before Device/Instance destruction. The host must stop and
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

## View and Attachment inputs

A View contains identity and scoped resources; it does not own Camera matrices. `CreateView`
accepts a SceneId. `Attach` takes explicit owned parameter bytes and a normalized
`RasterViewport`, and returns the generational `AttachmentId`. The compiled Native layout
checks the complete parameter byte count. A graph without context parameters takes an empty
payload. Mesh callers can construct `MeshViewParameters` and use `meshViewParameterBytes()`;
other Features define their own layouts without changing Runtime.

`UpdateAttachmentInputs` addresses that AttachmentId directly. Invalid viewport or parameter
size leaves its prior inputs unchanged. Success commits inputs for future recording, not a
completed GPU frame. Different Attachments of one View can have independent parameter layouts
and values. Resize preserves each Attachment's bytes and normalized viewport.

## Scoped resource instances

`PrepareStorage` resolves the actual Global/Scene/View/Attachment StorageSet and idempotently creates
its type/name/configuration instance. `FindStorage` returns the corresponding identity. `RemoveStorage`
revokes that identity immediately; it is not a decrement of a hidden preload count. A new request using
the old ID fails. StorageSet is the unique instance owner. The caller must finish related uploads and
detach its consumers before removing Storage. Normal Server shutdown settles accepted operations
before destroying scoped Storage and DeviceSession; arbitrary early removal is not a cancellation API.

Each Target has one Attachment. Feature composition and pass order are fixed by its compiled graph;
Runtime does not coordinate multiple independent Effects writing the same Target. Resize prepares a
replacement Context before publishing it and preserves the old association on failure.
