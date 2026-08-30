# ADR: Resource Semantic Layering and 3D Toolchain Recovery Review

- Status: Accepted
- Date: 2026-08-30
- Implementation SHA: `23b3195925b80e2df11c5c79076350ec1281256b`

## Resolved

- Asset identity is owned by the installable `resource/identity` leaf. Resource Description depends on identity, never
  on Asset storage/runtime.
- Scene, TextureAtlas, Flipbook, Material, MaterialInstance, and Model descriptions express cross-Asset relations with
  direct AssetIds. `OpaqueAssetId`, ordinal-plus-UUID-side-table relations, and Asset-layer Model/Material payloads are
  absent from active production.
- `ModelAsset` owns `rdesc::ModelDescription` through `TAsset<T>`. Model wire v2 persists exact node, primitive,
  Mesh/Material, Skeleton, and AnimationClip semantics and does not read or emulate v1 positional fallback.
- MaterialGraph is Authoring-owned. Its texture slots directly retain Texture AssetIds; Toolchain lowering emits the
  existing inline GBuffer/Forward SPIR-V and ShaderInfo runtime semantics.
- The concrete Model cooker produces independently loadable typed Assets with deterministic sub-identities. Assimp,
  stb, meshoptimizer, shaderc, rgbcx, and bc7enc remain Toolchain-private.
- `lux_asset_packer --type model` publishes a transactional typed Asset directory and the existing Pak path reloads the
  exact direct relationship graph through Provider/AssetVfs.
- World partition loading now retains O(extent-count) metadata and validates each extent against root volume chunk
  metadata before issuing IO; it no longer expands a partition-sized chunk vector.

## Explicitly held

- Asset residency/demand, streaming ownership, and a generic Resource graph remain unapproved.
- Material continues to carry the mature inline shader payload. Converting Material to Shader AssetIds requires a
  separate runtime-lifetime review.
- ScriptSystem Asset/backend capability injection remains Held.
- Full 3D Streaming, generic timing/ingress markers, GPU CI, ETC2/ASTC cooking, and Jolt broadphase region policy are
  outside this wave.

No Manager, Context, Services, Registry, Bridge, compatibility shim, or generic cook framework was introduced.
