# Services

`services` is the common, UI-independent factory provider. A module declares a constant
`ServiceDescriptor` and binds it with `ServiceEntry::bind<descriptor>(code)`. Runtime descriptors use
`ServiceEntry::create`, which freezes the names, type-token names, contracts and dependencies once.
Registration does not construct instances. The provider is shared because real DLL consumers must
use one registry identity source and host-resident allocation/deletion bridges.

Factories receive only a `ServiceResolver`. Its indexed `get<T>` and `require<T>` access the declared
shared and borrowed dependencies, respectively. Shared dependencies may use the same scope, its
parent or its root; a long-lived scope cannot resolve a descendant through this API. The dispatcher
is the explicit creation foundation for thread-affine objects. Services have no mandatory base class.

Resolve a `ServiceHandle` at a creation boundary and keep actual `shared_ptr<T>` dependencies after
creation. A handle pins one immutable definition generation. Contract projections alias the same
allocation, identified by definition generation, scope generation and qualifier. Different
configuration for a live key is an error; configuration is not a second instance key.

`SHARED` records are weak. `SCOPED` retains that same control block in its scope. Scope review blocks
new work and can be cancelled; release commits logical closure. Already accepted tasks and callers
can retain the allocation. `drained()` reports physical destruction, not reference counts or task
completion. A Registry and its borrowed foundation must outlive its scopes and undrained instances.
Explicit release checks admission. A scope's destructor only releases existing retention and keeps
any outer cleanup guard in force; it cannot admit work or dispatch business callbacks.

Factories, projections, validation and cleanup execute under the provider's existing callback guard.
Ordinary reentrant requests fail with `BUSY`; declared dependency construction is the bounded internal
path. A creating key reports a dependency cycle. A weak-expired allocation waiting for reclamation
reports `RETIRING` until the original Object dispatcher has physically destroyed it. No replacement
for that key can be published in between.

Warm instance hits cannot run unrelated cleanup. Cold construction pins the definition, scope and
configuration before releasing retired records. Cleanup may close a lexical scope or invalidate a
caller's handle; it cannot invalidate the operation's owned input. Scope admission is checked again
after foreign validation, construction and projection. Rejected allocations are cleaned under the
same callback guard, without publishing into a closed scope.

Owner-affine last references enqueue a preallocated reclamation through `ObjectDispatcher`; no
service-specific queue or executor exists. Host code holds the deletion bridge and code lease through
the virtual destructor, custom destruction callback and callback return. Plain, non-affine objects
can be destroyed on the last releasing thread. Each borrowed foundation remains the scope owner's
lifetime responsibility, rather than a no-op-deleter shared pointer.

The core tests include lazy and same-key construction, contract aliasing, scope/configuration refusal,
cycles, cleanup reentry, dependency failure and bounded scope churn. `services.plugin` loads a real
DLL and verifies destructor-tail and weak-control-block cleanup after unloading. `services.tasks`
uses the original Process submission, cancellation, transport collection and business dispatch paths.
The installed `services` consumer independently exercises core, tasks and three actual author models.
