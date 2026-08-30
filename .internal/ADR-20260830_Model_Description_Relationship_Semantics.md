# ADR: Model Description Relationship Semantics

- Status: Accepted
- Date: 2026-08-30
- Baseline: `d48a034d`

## Context

The retired importer built an in-memory `ModelNode` tree whose mesh and material fields were ordinals into separate
AssetId arrays. The cooked Model wire persisted only those flat arrays, not the tree. After reload, editor spawning and
thumbnail code therefore fell back to positional `mesh[i] -> material[i]` pairing. The importer also copied node names
but never preserved `aiNode::mTransformation`.

Assimp nodes can reference multiple meshes, and the same source mesh can be referenced by multiple nodes. Treating each
node occurrence as a new MeshAsset loses reuse; persisting only flat Mesh/Material arrays loses the binding. Conversely,
no active Player consumer uses Model node names. Root display naming is already carried by `AssetInfo`, while bone names
remain part of `Skeleton` where animation lookup requires them.

The existing skeleton extraction produces one skeleton for the imported scene. Every animation track resolves a bone
ordinal against that skeleton, and animation extraction is skipped when no skeleton exists. This is direct evidence for
model-level Skeleton and AnimationClip relationships rather than primitive-level ownership.

## Decision

`rdesc::ModelDescription` is the complete runtime Model semantic. It contains:

- a table of reusable primitives; each primitive directly stores non-null Mesh and Material AssetIds;
- a flat node table with one root ordinal;
- an asset-local `Eigen::Affine3f` transform per node;
- node-local child and primitive ordinals;
- one optional model-level Skeleton AssetId;
- a model-level ordered list of AnimationClip AssetIds.

Node and primitive names are not part of the runtime description. A node may reference multiple primitives. The same
primitive may be referenced by multiple nodes. The decoded node table must form an exact-cover tree rooted at the stated
root; every primitive must be referenced at least once.

Cross-Asset relationships always decode to AssetId. Local node, child, and primitive relationships use ordinals. The
wire may use local tables internally but may not expose out-of-band AssetId arrays in the decoded Description.

Model inner wire advances directly to v2 and is fully recooked. There is no v1 compatibility reader or positional
fallback. The generic cooked-asset outer envelope and the `lux.model` identity remain unchanged.

## Consequences

- `ModelAsset` becomes `TAsset<rdesc::ModelDescription>` and `ModelAssetData` is deleted.
- Source node transforms and Mesh/Material bindings survive Pak/VFS reload.
- Importers mint stable sub-asset identities before constructing the Model relationship graph.
- Skeleton and AnimationClip remain independently loadable Assets; Model does not own their payloads.
- This ADR does not authorize Asset residency, a Resource graph manager, an Asset context, or a generic cook registry.
