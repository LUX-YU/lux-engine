# Semantic commands

Commands carry pure descriptors, owning arguments and fixed targets. Query reports availability; execute reports immediate completion or an accepted operation's existing identity. This receipt does not duplicate the operation's terminal state.

Default `PINNED` invocations retain the accepted entry. Explicit `CURRENT_REGISTRATION` re-resolves the ID and checks descriptor/input compatibility. Dispatcher capacity is bounded; BUSY preserves the queue head, payload and target. Recursive drain/publication and foreign-thread access are rejected. A local strong entry and outer code lease remain alive across callbacks and result construction.

Save fixes SessionId but freezes the current committed content at actual admission. A strict edit retains its original `based_on`; it rejects intervening content instead of rebasing. DeleteSelection owns its original object set, independent of later selection. View targets retain their provider's immutable typed value and defining code through the existing
CommandArguments owner. `CommandDescriptor::target_type` states that contract; `forView` captures it once,
and `view<T>()` exposes a const value only for the matching type. Root-based windows use the original
PaneHandle, including its attachment epoch. The command layer has no ViewHost ID or UI-header dependency.
Wrong target types are rejected before query/execute, and CURRENT_REGISTRATION also compares target type.
UI owner/generation checks still occur in the workbench receiver at actual execution. The workbench menu captures invocation targets at opening; shortcuts use the same route. Registry updates cannot redirect an already pinned invocation.

The command layer does not include Menu, Context, Pane or concrete models. Domain interaction remains direct where a command adds no value. Application-level open/exit/product orchestration is a P12 responsibility, not an Unsupported placeholder registered here.

## Declared dependencies

Module constants can supply dependencies and a `CommandDescriptor::create` factory. Registration validates
the declaration without constructing services. A registry bound to a `ServiceRegistry` and lexical
`ServiceScope` resolves those declared dependencies only on first use. The factory returns a noncopyable
`CommandBinding` containing the two accurate callbacks; it never receives an EditorContext. Directly bound
callbacks remain appropriate for an existing object's commands. Mixing both forms, partial callbacks,
or dependencies without a factory is rejected during catalog preparation.

Bindings are bounded by the registry's explicit capacity and keyed by the original entry in its fixed scope.
Stable calls use that binding without interpreting names. Old pinned entries keep their original receiver;
catalog replacement does not redirect them to another service. Expired entries are collected before a new
binding is admitted, under the command and service publication guards. Capacity or BUSY leaves the existing
bindings unchanged. Factory failure destroys its candidate within the original service callback protection.
The external code pin outlives the binding's callbacks and their disposal return. The factory receives
that same code lease explicitly. Owning work that escapes the command (for example a SessionPreparation)
retains it independently; dropping the catalog and binding must not unload accepted work.

The scope and service infrastructure outlive the command registry. Closing the scope refuses new calls,
including when a query closes it before execute. Cancelling close keeps the same binding. The command owner
releases its bindings before final service drainage; it does not cancel accepted operations or own their
completion state. Static module and V10 DLL consumers use this same path with a real Process task.

## Compound publication

`readBatch()` pins the participating owner's publication boundary for a fixed factory/read batch; it does not reserve a revision. `preparePublication(snapshot)` additionally checks revision capacity and owns a single-use candidate. The returned move-only `CommandRegistry::Batch` borrows its fixed-address registry, which must outlive it. `commit()` swaps the prepared candidate without allocation, callbacks or ordinary failure, and returns the previous snapshot. The scope remains active until destruction, including abandoned-candidate destruction. Reusing a consumed commit permission violates the contract.

Ordinary `publish`, a publication batch, and dispatcher drain return BUSY while a scope exists. Read scopes may nest, including inside an executing command that needs a fixed catalog for recovery or UI construction. The final read scope releases only its publication hold; command calling and dispatcher state remain independent. A publication scope still rejects new reads through its candidate cleanup and notification interval. Read-only snapshots, pinned query/execute, and dispatcher enqueue retain their existing behavior; recursive command execution is still rejected. Failed entry never clears another owner's scope. Commands do not depend on the application or know about contribution registries. Accepted task completions remain the responsibility of their original services.
