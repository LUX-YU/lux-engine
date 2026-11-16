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

Registration defines authoring pin schema, compile validation, direct Slang emission and
optional paired source codecs. Authoring schema validation remains separate from
compile eligibility: disconnected nodes, missing surface output, cycles and
finite but uncompilable drafts remain saveable. The compiler validates all node
bindings, builds temporary indexes, and emits reachable inputs through registered
callbacks. Dynamic schemas and multiple outputs use semantic PinIds; there is no
closed builtin expression instruction set.

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

## Direct Slang compilation

`material_graph` owns author data, source codecs and node callbacks, without Slang dependencies.
`material_compiler` validates all payload/binding declarations, selects surface inputs for each purpose,
traverses reachable nodes with an explicit stack and emits each shared value once. Inputs to `EmitSlang`
are stable text views; outputs name typed locals already declared by the traversal. Empty expressions
represent optional or unused inputs. Plugin callbacks may emit trusted Slang statements from their
validated payloads; untrusted source strings must not become identifiers or executable snippets.

Node callback ABI 2 replaces the instruction-emission callback table. Binary hosts must resolve a
versioned entry point or negotiate ABI 2 before constructing/copying registrations. Catalog admission
also accepts an explicit ABI number and rejects mismatches before inspecting the span. The DLL sample
uses `materialNodeRegistrationV2`. This does not change node type identities or source codec v1/v2.
CodeLease continues to cover callbacks, cloning, destruction and return from plugin code.

`deriveMaterial(compiler, graph, options)` borrows a stable graph and an existing shared ShaderCompiler.
It generates SPIR-V or DXIL with the corresponding reflection, using separate Texture2D/SamplerState
bindings and the existing material ABI. It supports opaque/masked Forward Unlit, Unlit/PBR GBuffer and
Shadow. Unsupported models/purposes fail explicitly. Opaque Shadow traverses no coverage dependencies.
When no requested purpose has a material record, only the derived defaults and texture associations are
cleared; the author graph is untouched. Record-using purposes retain one common target layout.

The shared ShaderCompiler owns its GlobalSession. Calls on an instance are serial; parallel workers own
separate instances. A synchronous batch borrows immutable include files and source, sharing Sessions for
matching target/profile/search paths. Generated module identities include content and a collision-safe
batch ordinal. Sessions end after owned results are copied; the next batch reloads changed dependencies.
Successful warnings and post-compilation use metadata are returned separately from declared GPU layouts.
There is no compiler thread pool or global cache. Runtime defaults and texture AssetIds are not code keys.

`MaterialAsset` stores only target `MaterialDescription`. Cooked v11 and the existing v9 SPIR-V reader
remain supported. The former v10 expression payload returns UnsupportedVersion and must be rederived
from a retained author source, or exported by a fixed old external tool. Deleting the old compiler does
not delete assets or promise recovery of an editing graph from expression bytes.

```text
lux_material_tool derive source.luxmaterial target.material spirv gbuffer,shadow
lux_material_tool derive source.luxmaterial target-dxil.material dxil gbuffer,shadow
```

Compiler binaries, licenses and exports are installed with the module SDK. The old
`engine/toolchain/material` compiler and its Backend/ShaderEmitter consumers remain out of scope and
require a later migration to this API; they are not supported through forwarding headers. Shared legacy
GLSL sources still consumed there remain frozen, while this module uses its single Slang source set.
