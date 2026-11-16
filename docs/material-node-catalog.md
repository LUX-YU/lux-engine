# Material node contributions

`MaterialNodeCatalog` is owned by the composition that publishes it. Compose it on one thread; after publication,
workers use immutable `MaterialNodeType` definitions. Do not mutate a catalog concurrently with lookup.
Registration does not construct a payload or invoke plugin callbacks. A rejected batch publishes no definitions.
Lookup uses `graph::NodeTypeId`, derived from the canonical name using the shared lux-cxx FNV-1a algorithm.
The catalog rejects duplicate names/versions, mismatched declared hashes and hash/name collisions.

`MaterialNodeRegistration` declares an owning name/version, the payload's C++ type identity, a code lease and four
`noexcept` callbacks: create, describe pins, validate, emit Slang. The catalog owns copies of the canonical and
C++ type names. The returned shared immutable definition survives catalog destruction and keeps its callback code.
This is a domain directory, not a PluginManager or service factory.

`MaterialNodePayload` is a move-only semantic value. `make<T, cloneFunction>()` requires nonthrowing construction
and destruction and an explicit fallible, nonthrowing clone function. Invalid code leases are rejected before
construction. Ordinary heap OOM remains fatal. Payloads contain no NodeId, PinId or membership pointer.
`get<T>()` checks the actual C++ token; it does not infer a type from a node enum ordinal.

Each payload independently holds its code lease. Moving into an occupied payload destroys the old value before
releasing its code; destruction and clone return through the stable Material module while that code is held.
Cloning a moved-from payload fails. A plugin clone failure leaves the original value intact. Borrowed calls may
not overlap destruction or mutation of their definition/payload; these pure values are not LuxObject receivers.

Pin declarations are owning values generated from the payload and may have a dynamic count. Semantic pin keys
are distinct from graph-issued PinId. Pin identity, direction and value type are checked before compilation.
Emission receives stable input expression views and predeclared typed output identifiers, in declaration
order. An empty input means optional absence or UNUSED; unused dependencies are not traversed. The
callback appends Slang statements to the disposable output string. It cannot retain those views or
assume they alias the appendable destination. Type/coercion qualification remains in graph traversal.
A failed callback discards the whole source candidate and never authorizes adoption.

Callback ABI 2 requires a version handshake before an external registration table is constructed or
copied. The DLL sample exports `materialNodeRegistrationV2`; old symbols must not be invoked with the
new layout. Catalog `add` additionally rejects an explicit mismatched ABI before inspecting its span.
Author node type/version and payload codec identities remain unchanged.

The Material module does not depend on Engine, Editor, shaderc or PluginManager. It uses the existing Object
module's CodeLease and the existing graph and resource contracts. No new library or alternate graph is created.
Tests exercise dynamic schemas, immutable definition lifetime, rejected batches, clone errors, move cleanup and
an actual loaded DLL whose code outlives its catalog and is released after its last payload.

Serialized graphs carry canonical type/version information. GraphTopology remains the sole structural
authority; the catalog does not replace graph identity, editing, compilation or persistence. See the
[Material module](../modules/function/material/README.md) for the integrated authoring surface.
