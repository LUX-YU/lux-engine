# Material authoring and compilation

`material_graph` owns material semantics. The shared `graph` module owns node/pin
identities, membership, directions, links, layout and identity high-water marks.
Material node payloads do not carry a second graph identity or topology.

`MaterialGraph` stores registered `MaterialNode` values by `NodeId`, and editable
pin metadata by `PinId`. Pin metadata lookup is an average O(1) hash lookup;
`pinId(node, semantic)` is a linear convenience lookup, not a second identity map.
Structural records and layout are exposed read-only. Field edits that change a
node's schema use a replacement candidate, retaining explicit identities where
the registered schema still accepts them.

`MaterialGraphEdit` borrows the graph exclusively for a synchronous transaction.
Prepare clones inserted payloads and builds the existing `graph::GraphEdit`
candidate. All explicit restored identities precede fresh issuance, including in
mixed batches. Abandoning a prepared candidate never recycles issued identities.
Commit transfers prepared map nodes and swaps the prepared structure; it does
not call extension code. Removed payloads remain owned by the edit until its
destruction, outside commit. No parallel history algorithm is introduced.

`extractNode()` transfers the actual payload into a snapshot together with pin
records, metadata, links and optional layout. Restoring a snapshot uses its
explicit identities; unrelated future creation remains above the high-water
marks. Cloning a graph is fallible because a registered payload clone may reject.

Registration defines authoring pin schema, compile validation, lowering and
optional paired source codecs. Authoring schema validation remains separate from
compile eligibility: disconnected nodes, missing surface output, cycles and
finite but uncompilable drafts remain saveable. The compiler validates all node
bindings, builds temporary indexes, and lowers reachable inputs through registered
callbacks. Dynamic schemas and multiple outputs use semantic PinIds; there is no
closed builtin node switch in lowering.

Source encoding writes version 2: canonical node type name/version, registered
payload bytes, explicit node/pin identities and pin metadata. Decoding accepts
version 1 builtin files through the same builtin payload codecs. The old Math
output hint is normalized to its actual result type when it matches the original
operand hint. Old source bytes are not re-emitted unchanged; identity, editable
state and compiled behavior are the migration contracts. Structural links are
restored independently of compile eligibility.

External source decoding requires an explicit `MaterialNodeCatalog`; the
builtin-only overload creates a local builtin catalog. Missing codecs, unknown
types, version mismatches and callback failures return owned diagnostics.
Graphs, snapshots and payloads retain their registered definition/code lease, so
destroying the caller's catalog or library handle cannot unload code needed by
later cloning, compilation or destruction.

The Material migration does not complete the corresponding Flow migration or
qualify a new graph-editor UI. Those are separate remaining MA08 requirements.
