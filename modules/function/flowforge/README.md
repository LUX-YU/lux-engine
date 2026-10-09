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

This lifetime contract does not complete the broader Flow graph migration: the existing
Node/Pin structure and NodeRegistry palette are still active. The final domain catalog,
compiler extension surface and removal of duplicate structural authority remain pending.
