# Shared workbench module declarations

`editor_extensions` is the common workbench module provider, physically adjacent to EditorContext and UiRegistry. Its target, logical includes and installed package retain one definition. The declaration generator belongs to this same provider; Scene-specific exports live in `workbench/scene/contributions`.

The composition boundary combines immutable service, UI, command, content-factory and settings declarations. The current product still consumes the previous view-factory entries until the EC4 M6 migration; new UI factories use UiRegistry. It does not own author sources, History, saves, Run sessions or rendering resources. `ContributionRegistry::enqueue()` prepares a bounded candidate; `applyPending()` publishes a fixed batch at the owner safe point. The participating CommandRegistry grants a narrow batch scope before callbacks. Ordinary command publication is BUSY through candidate cleanup, old-value disposal and notification. The prepared scope performs its one-use commit only after reflection validation; the contribution owner never unlocks and re-enters ordinary publication. Notifications follow publication. Requests made by a notification belong to the next batch. Failed candidates leave the visible catalog unchanged.

`lux_editor_exports_v10` is the formal SDK entry. Header size, interface version, fingerprint and advertised counts are checked before invoking the contribution callback. The existing project plugin loader verifies the binary and pins its runtime dependencies. Old V6/V8/V9 exports are rejected; the independent game plugin protocol is unchanged.

Static modules declare a `LUX_META(luxmodule)` free function returning `const EditorModuleDescriptor&`
with no arguments and `noexcept`. Keep that declaration header light: it needs only Marker.hpp and a
forward declaration of EditorModuleDescriptor. `engine_declare_editor_module` records this header on
the module target; the product selects module targets through `engine_target_add_editor_modules`.
The original MetaUnit is validated and rendered by the installed inja generator into strong function
references. No module factory runs during static initialization. `loadStaticEditorModules()` and the
DLL loader use one export-table validator and the same contribution normalization. Static descriptors
and their code must live for the process lifetime; a dynamic library must use the pinned DLL loader.

Pure `contribute()` runs without live application services. Optional `activate()` declares separate
counts and requirements for `SessionActivities`, `ProjectActivities` and `WorkbenchAccess`. Missing
required groups fail before the foreign callback, and unrequested groups are not supplied. Both
results pass through the same code-owner normalization and atomic contribution publication.
Activation input group pointers are callback-only; closures capture explicit provider references
and own their activation state. They must not start unowned asynchronous work during preparation.
The application keeps the actual providers alive until accepted work, views and closures retire.
This is lifetime-aware dependency injection, not a security sandbox or an arbitrary hot-unload API.

`editor_extensions` has no concrete tool UI, renderer implementation or LLVM dependency.
Each Scene, Material and Flow provider declares and binds its own factories and commands.
The product selects providers; domain implementations do not depend on the application to declare their modules. Control factory descriptions live in `scene_control_api`; using them does not import a concrete Inspector or viewport.
LuxWindow's Vulkan surface type declarations remain a header-only platform dependency.

The runtime fingerprint covers modules/engine public contracts; the Editor fingerprint additionally covers the formal five-layer public contracts and toolchain. They are independent compatibility checks, not a guarantee of cross-compiler C++ ABI compatibility. External extensions use the same configured compiler, CRT and SDK.

## Lifetime

Factories return a complete, detached view or an owning decoded input. Host adoption and session publication are separate operations. A view closing does not close content. An accepted save can finish after its role or view disappears; publication facts survive, while invalid adoption is rejected by the original SaveService.

Code must outlive the virtual destructor **and the shared control block's disposal call**. A plugin's object holding its own lease is insufficient. Drafts own outer code pins. At snapshot creation, the receiving module wraps foreign shared entries with `object::pinCodeOwner`; disposal of the foreign value finishes before releasing that outer pin. Published weak aliases refer to the receiving module's control block. These are cold registration allocations, not per-frame allocations. Do not bypass host-side snapshot preparation by returning a pre-published catalog from an extension.

When a later snapshot shares an unchanged entry, normalization unwraps its prior receiving-module wrapper and keeps the same underlying entry. It does not retain a chain of prior wrappers. The deleter clears the value before its code pin at the last strong release; expired weak aliases retain neither the value nor the DLL. External code remains responsible for any separate foreign weak records it creates outside this publication contract.

The SaveService registration directory contains weak records. Its existing state allocator is therefore selected by the module that constructs the service. A plugin calling the statically linked SDK cannot place a weak control block with a plugin-local deleter in the host's directory. Installation handles also hold a local code pin while releasing their shared data. No DLL is kept alive merely to retain already-expired weak records.

Configuration values retain the shared reflection environment independently of their code lease. The value is destroyed before reflection and plugin code. Reflection candidates use the existing registry draft/commit mechanism.

Scene configuration editors and Inspector components are declared by the Scene domain through
`SceneEditorCatalog` service entries. ContributionDraft/Snapshot no longer contain these fields or
interpret their payloads. The domain validator runs against the complete staged reflection registry
under the original command/service/UI publication guards. Duplicate or colliding component identities,
invalid configuration schemas and mismatched reflection reject the whole candidate. The domain's
immutable backing is shared by metadata readers and its lazy service, with no copied mutable catalog.
The generic extension library no longer links scene_control_api; actual domain producers and consumers
link it explicitly.

## Actual consumers

`session_factories` provides Save/Undo/Redo; each concrete author activity provides its source factory
and creation command. Each workbench tool constructs its own detached view and signal connections.
There is no central built-in factory archive. External extensions use the same immutable descriptors
and Entry binding API, retaining their original code lease and activation dependencies.

The installed `lux_editor` uses EditorApplication and the same formal providers. Original Context,
mutable registration aggregates and V6 assembly have been deleted. Standalone V10 view factories take
`std::monostate`; content tools use explicit typed bindings. Product Window commands resolve those
immutable factory entries and reuse an existing standalone tool of the same ViewTypeId.

## EC3 settings

Settings contributions join the same immutable publication, code normalization and guarded reflection
batch. A SettingsEntry references one fixed descriptor or freezes dynamic text/configuration in one
backing. It owns its exact application callback and CodeLease; pages/values retain these owners.
Default decoding and pure validation run before reflection/catalog commit, under both existing guards.
A rejected default leaves the previous catalogs intact. Reading a document or preparing a draft never
applies a value or invents a persistence receipt. SettingsDocument/TOML is provided by layout_model;
only materializing ConfigurationValue requires editor_configuration and reflection.
