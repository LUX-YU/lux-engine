# Flow authoring and compilation values

`NativeCallDefinition::create` freezes the invocation signature used by `NativeFuncCall`.
The returned immutable owner retains all signature names, parameter and result types,
record identity/ancestry and value operations. Record projections deliberately omit
reflected fields/methods: inspection and reflective discovery use the original metadata
provider. The explicit `CodeLease` covers the invoker and any copied type operations.
`CodeLease::builtin()` is only for process-lifetime code; dynamic callers supply its real
code owner. No module loader or second reflection registry is created here.

A node retains this definition before its pin owners. Rebinding keeps the previous
owner until old pins and default values are destroyed. Node/exec identity and the existing
pin reconstruction order remain unchanged; rebuilding data pins still removes their
old links. The source codec and reflection palette construct the same definition.

`FlowAnalysis::create` owns the domain checks previously embedded in Toolchain: Ability
and Event requirements, transitive suspension through graph function calls, borrowed-step
values crossing suspension and synchronous lifecycle exports. It accepts the real FlowGraph
and immutable catalog views without LLVM, Process or Editor. The compiler uses this same
analysis before lowering and when checking generated async markers; there is no second
Toolchain implementation of these rules.

The result owns requirement values and an execution-reachability projection keyed by the
input graph's stable IDs. It does not retain Node/Pin pointers, catalog views or reflection
metadata. Graph and catalog destruction is safe after analysis. This is a disposable
compilation result, not an editable graph or a live cache: after editing a graph, analyze
the new input again. Queries return the lowest reachable suspension NodeId deterministically,
including recursive functions, or an invalid ID when no suspension is reachable. The caller
must pair analysis and lowering from the same unchanged graph input.

This lifetime contract does not complete the broader Flow graph migration: the existing
Node/Pin structure and NodeRegistry palette are still active. The final domain catalog,
compiler extension surface and removal of duplicate structural authority remain pending.
