# Typed Asset Wave — Before Evidence

- Revision: `de80d4449d86ea8ccac61b1c5c76635f0843cc7f`.
- Branch/remote: `main == origin/main`.
- RelWithDebInfo full build: `ninja: no work to do`.
- Full CTest: 110/110 passed.
- Existing user changes preserved outside the wave: `.gitignore` and
  `engine/domain/world/core/include/lux/engine/world/WorldPartition.hpp`.

Before AR0, public Asset decode is centered on `AssetCodecDescriptor`, `DecodedAsset`, `TypeToken`, `void*` encode input and
`shared_ptr<const void>` payload output. `CookedAssetImageView` borrows a span and cannot itself retain a texture image.

The active cooked envelope already supports v1/v2 metadata and auxiliary payload layout validation. The migration preserves
that outer wire while replacing the public object model and making the parsed image own `SharedBytes`.
