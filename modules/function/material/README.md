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

## IR assets and target derivation

`material_ir` contains the owned `MaterialIR`/`ShaderIR` values, SSA validation and
one fingerprint algorithm. It does not link the authoring graph, a compiler or a
device. Graph callers include `MaterialLowering.hpp` to use `lowerMaterial()`.

`MaterialAsset` stores `VMaterialAssetData`: either generic `MaterialIRDescription`
with positional texture AssetIds, or cooked `MaterialDescription`. Version 10 is
explicit, bounded, little-endian IR serialization; version 11 stores a target
program set. The version 9 cooked SPIR-V reader remains available. A cooked
program cannot be used as a generic source. Cached fingerprints are recomputed
from decoded IR, not trusted as stored identity. Parameter defaults, raw-block
language, shading model and render state remain in the source asset.

Enable `LUX_BUILD_MATERIAL_COMPILER=ON` in the module build to expose
`lux::engine::function::material_compiler`; the Render module bootstrap enables
it through its shared shader compiler target. `deriveMaterial()` takes only IR,
texture asset associations and target/purpose/profile options. The implementation
uses the shared `lux::engine::resource::shader_compiler` and the existing GBuffer
math source. It supports the existing opaque/masked Forward Unlit, Unlit/PBR
GBuffer and Shadow purposes. Unsupported models/transparent uses return errors.
RAW_EXPR accepts Slang/HLSL expressions with explicit operands; other languages
are rejected when used, never translated implicitly.

Each `MaterialProgramSet` has one target and owns that target's exact parameter
layout. Program cache identity covers emitted source, embedded shader dependencies,
IR schema, compiler versions, profile, target and purposes. Runtime parameter
defaults and texture asset associations do not change compiled code identity.
The libraries do not own a global cache or an Engine asset.

The optional command-line adapter separates authoring from derivation:

```text
lux_material_tool lower source.luxmaterial source.ir
lux_material_tool derive source.ir target.material spirv gbuffer,shadow
lux_material_tool derive source.ir target-dxil.material dxil gbuffer,shadow
```

The second and third commands read only the saved IR asset. Compiler binaries,
licenses and package exports are installed with the module SDK. Engine's former
Graph compiler/cooker and Editor consumers still need a later interface migration;
they are not part of this module adapter.
