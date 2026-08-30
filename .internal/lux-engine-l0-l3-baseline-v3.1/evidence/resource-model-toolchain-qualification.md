# Resource Identity / Model Toolchain Qualification

## Revision

- Starting baseline: `a35beab7d4ab2b495a17d4b8bdf7565f3d9b9003`
- Qualified implementation: `23b3195925b80e2df11c5c79076350ec1281256b`
- Configuration date: 2026-08-30

## Build and test matrix

All commands ran from Visual Studio 2022 Developer PowerShell 17.14.35 with `-j 4 -k 0`.

| Build tree | Profile/content | Result |
|---|---|---|
| `build/RelWithDebInfo/lux-engine` | default Developer Runtime closure | `all` passed; second build `ninja: no work to do`; CTest 116/116 |
| `build/Toolchain/lux-engine` | `LUX_BUILD_PROFILE=TOOLCHAIN` | `all` passed; second build `ninja: no work to do`; CTest 110/110 |
| `build/FullRender/lux-engine` | packed full Render content | `all` passed; second build `ninja: no work to do`; CTest 127/127 |

Independent installed consumers passed for `resource_identity`, `resource_descriptions`, and `typed_resource_assets`.
The installed Toolchain packer successfully cooked the static glTF fixture into 10 typed Assets, wrote a Pak, and
inspected all 10 entries after installing Assimp/meshoptimizer and their transitive runtime DLLs.

## Model and wire evidence

- Model v2 golden: size 564 bytes, SHA-256
  `47e4ab994c3d3666cd34e92f5fc4b2e499a04bdf5d94a0940f13582c8b5a6215`.
- Static PBR source covers three source meshes, two source materials plus Assimp's canonical material set, shared
  material, one source mesh referenced by two nodes, local node transforms, external PPM, embedded PPM, metallic /
  roughness, and alpha Mask.
- Skinned source covers two bones, normalized vertex weights, one AnimationClip, typed encode/decode, and Model-level
  Skeleton/AnimationClip AssetId relationships.
- FullRender qualification Pak SHA-256:
  `C12B264B6A0D457AAFCDAE0149348BEAD3002FBE79553EB5ED80A5BB158B5F74`.
- Provider/Pak/AssetVfs reload resolves every Model primitive directly to its Mesh and Material AssetIds, then resolves
  each Material texture slot directly to Texture AssetIds. No positional fallback is present.

## Real Vulkan qualification

- OS: Microsoft Windows 11 Pro 10.0.26200
- CPU: 13th Gen Intel Core i7-13700KF
- GPU: NVIDIA GeForce RTX 4070 Ti
- Driver: 591.86.0.0
- Vulkan instance: 1.4.321
- Vulkan device API: 1.4.325
- Validation: enabled
- Command: `model_asset_vulkan_qualification.exe --require-gpu`
- Result: `gpu=1 model_primitives=3 mesh_vertices=3 material_textures=2 lit_pixels=512 validation_errors=0`

The upload path used the cooked BC7 mip blocks directly, the typed Mesh payload, and Material-owned compiled shaders.
No runtime image decoding, Asset residency manager, or GPU Asset bridge participated.

Android engine configure/build/CTest was intentionally not run. Debug, RelWithDebInfo, and Android install include trees
were synchronized for changed public Resource headers as required by the meta-generation contract.
