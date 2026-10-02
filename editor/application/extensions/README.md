# Formal Editor contributions (P11)

The application combines immutable command, content-factory, view-factory and configuration contributions. It does not own author sources, History, saves, Run sessions or rendering resources. `ContributionRegistry::enqueue()` prepares a bounded candidate; `applyPending()` publishes a fixed batch at the owner safe point. The participating CommandRegistry grants a narrow batch scope before callbacks. Ordinary command publication is BUSY through candidate cleanup, old-value disposal and notification. The prepared scope performs its one-use commit only after reflection validation; the contribution owner never unlocks and re-enters ordinary publication. Notifications follow publication. Requests made by a notification belong to the next batch. Failed candidates leave the visible catalog unchanged.

`lux_editor_exports_v8` is the formal SDK entry. Header size, interface version, fingerprint and advertised counts are checked before invoking the contribution callback. The existing project plugin loader verifies the binary and pins its runtime dependencies. V6 is an explicitly rejected input; the installed product uses only V8.

Pure `contribute()` runs without live application services. Optional `activate()` declares separate
counts and requirements for `SessionActivities`, `ProjectActivities` and `WorkbenchAccess`. Missing
required groups fail before the foreign callback, and unrequested groups are not supplied. Both
results pass through the same code-owner normalization and atomic contribution publication.
Activation input group pointers are callback-only; closures capture explicit provider references
and own their activation state. They must not start unowned asynchronous work during preparation.
The application keeps the actual providers alive until accepted work, views and closures retire.
This is lifetime-aware dependency injection, not a security sandbox or an arbitrary hot-unload API.

`editor_extensions` has no concrete tool UI, renderer implementation or LLVM dependency.
`editor_builtin_contributions` holds the actual built-in factory composition. Control factory
descriptions live in `scene_control_api`; using them does not import a concrete Inspector or viewport.
LuxWindow's Vulkan surface type declarations remain a header-only platform dependency.

The runtime fingerprint covers modules/engine public contracts; the Editor fingerprint additionally covers the formal five-layer public contracts and toolchain. They are independent compatibility checks, not a guarantee of cross-compiler C++ ABI compatibility. External extensions use the same configured compiler, CRT and SDK.

## Lifetime

Factories return a complete, detached view or an owning decoded input. Host adoption and session publication are separate operations. A view closing does not close content. An accepted save can finish after its role or view disappears; publication facts survive, while invalid adoption is rejected by the original SaveService.

Code must outlive the virtual destructor **and the shared control block's disposal call**. A plugin's object holding its own lease is insufficient. Drafts own outer code pins. At snapshot creation, the receiving module wraps foreign shared entries with `contracts::pinCodeOwner`; disposal of the foreign value finishes before releasing that outer pin. Published weak aliases refer to the receiving module's control block. These are cold registration allocations, not per-frame allocations. Do not bypass host-side snapshot preparation by returning a pre-published catalog from an extension.

When a later snapshot shares an unchanged entry, normalization unwraps its prior receiving-module wrapper and keeps the same underlying entry. It does not retain a chain of prior wrappers. The deleter clears the value before its code pin at the last strong release; expired weak aliases retain neither the value nor the DLL. External code remains responsible for any separate foreign weak records it creates outside this publication contract.

The SaveService registration directory contains weak records. Its existing state allocator is therefore selected by the module that constructs the service. A plugin calling the statically linked SDK cannot place a weak control block with a plugin-local deleter in the host's directory. Installation handles also hold a local code pin while releasing their shared data. No DLL is kept alive merely to retain already-expired weak records.

Configuration values retain the shared reflection environment independently of their code lease. The value is destroyed before reflection and plugin code. Reflection candidates use the existing registry draft/commit mechanism; configuration validation runs against the prepared environment.

## Actual consumers

`BuiltinContributions` installs Save/Undo/Redo and all three real content factories. Scene, Material and Flow view factories call the existing workbench implementations. The integration harness uses those formal factories and DesktopShell command routing; installed consumers load a real V8 extension, decode a Material source on a worker, install it on the owner, edit/save it and unload all contributions safely. A separate runtime-only installed consumer imports no Editor target.

The installed `lux_editor` uses EditorApplication and the same formal providers. Original Context,
mutable registration aggregates and V6 assembly have been deleted. Standalone V8 view factories take
`std::monostate`; content tools use explicit typed bindings. Product Window commands resolve those
immutable factory entries and reuse an existing standalone tool of the same ViewTypeId.
