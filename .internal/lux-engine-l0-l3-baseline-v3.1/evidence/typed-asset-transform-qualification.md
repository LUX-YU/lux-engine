# Typed Asset / Toolchain / Transform Qualification — 2026-08-30

## Implementation commits

- Typed Asset SSOT and model: `517f45f3`, `1668de1c`.
- Texture zero-copy typed codec: `ee9f62e8`.
- Shader typed codec and active cooker/packer: `e14d8472`, `55c60a58`, `50b3e5df`.
- World / Simulation / Scene typed migration: `9691e841`, `ec0e0260`, `0b134e7d`.
- Mature asset groups: `ebc2e003`, `a6814915`, `41097f36`, `3699262d`.
- Erased codec retirement: `18439ef0`.
- Active private cooker relocation: `3f539634`.
- Transform single-owner registration: `d72e6bbc`.
- Typed BC Vulkan qualification: `78bcf027`.
- Typed Lua Toolchain envelope: `fae5b613`.
- Final typed-asset architecture gates: `d37b0955`.

## Frozen wire evidence

The following legacy golden images remain byte-identical after typed migration:

| Asset | Bytes | SHA-256 |
|---|---:|---|
| Texture | 820 | `e01de6ccfb600f997b0ad08035acbda1c404647faa86284e4dcd28a03efed3cc` |
| Shader | 423 | `ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d` |
| Mesh | 708 | `55d3667e298f4b5a358cdd9979b348323d5c911ff5f5971f55beeaf181b5f765` |
| Skeleton | 576 | `0f9757141b0f49ac269a74901050c96d378d71858227e31b58e6aeca0ece0248` |
| AnimationClip | 496 | `c88929b5122c40953854b8828d48b87022a7711b8705e249d2df7145ef0baf50` |
| Material | 495 | `34ddba8c3a78463d048553fa3d44481a737646895b6797e8fb62e19e9bd1fd8f` |
| MaterialInstance | 472 | `a35ae4037601ccabd656b458ce979beec7947ccb3081345ddb3955fabbb6d495` |
| Model | 485 | `715aa44f5c17fb9fcf23fb2f91bc35b2c4b9db084ba6325ce5dcfc92822558be` |
| TextureAtlas | 477 | `2c0a7f6353760c6994065c143b169707c16191899076b53c0814604e5a86d2e1` |
| FlipbookClip | 469 | `38e7fa62a043f95947ba06b0a756118ec86ea33250195791038e541747a15533` |

World (`228` bytes, `dea3eca1af27b347bc525dbdb328437dad8c22cbb599bace44c7b10eb8064993`), Simulation (`865`
bytes, `8151e12e91b262bea50ac877fa893591dfa1a24162db55d295405dda8a65a492`) and Scene (`40` bytes,
`ff5add560504767ee622029d7c794ff8635749525df53d03be7d7dbcb97af6cd`) inner payload hashes also remain frozen; only
the generic v2 outer envelope was added.

Texture decode retains the owning cooked image through `SharedBytes::subspan()`: the typed texture pixel pointer equals
the inspected outer data pointer, remains valid after the temporary `CookedAssetImage` and original owner are released,
and decode/re-encode is byte-identical.

## Build and test matrix

- Default RelWithDebInfo: full `all -j 4 -k 0`; second run `ninja: no work to do`; CTest `115/115`.
- Toolchain RelWithDebInfo: full `all -j 4 -k 0`; second run `ninja: no work to do`; CTest `103/103`.
- Toolchain install prefix: `E:/SyncForder/CodeRepos/install/Toolchain`; installed `lux_asset_packer` cooked and
  inspected a BC1 `NO_MIPS` TextureAsset successfully.
- Full-render RelWithDebInfo: active in-tree `lux_asset_packer` generated all shader/texture content; full `all`; second
  run `ninja: no work to do`.
- Installed consumers passed for typed Asset, Texture, mature typed assets, WorldAsset, SimulationAsset, SceneAsset,
  Transform registration and System/Script binding.

## Real Vulkan BC qualification

- OS: Microsoft Windows 11 Pro `10.0.26200`.
- CPU: Intel Core i7-13700KF.
- GPU: NVIDIA GeForce RTX 4070 Ti, driver `591.86.0.0`.
- Vulkan instance/device API: `1.4.321` / `1.4.325`.
- Validation: enabled; error count `0`.
- Pipeline: authoring PNG -> active Texture cooker -> BC3_SRGB 14-mip TextureAsset -> active Pak ->
  `PakAssetProvider`/`AssetVfs` requested-ID validation -> typed zero-copy decode -> direct compressed mip upload ->
  Canvas2D sample -> offscreen readback.
- Result (two consecutive `--require-gpu` runs): cooked bytes `44,740,096`, upload ready, lit pixels `109`, non-SKIP.

## Transform ownership

The registration span contains one concrete Transform type. The registered object owns one hierarchy index, one delta
batch and one maintenance mechanism, followed by the existing 2D/3D propagation mechanisms. Tests prove Parent-only
mutation updates both dimensions in the same `Simulation::execute()`, a 3D failure discards already-recorded 2D commands,
zero-change execution is valid, and a deliberately duplicated maintenance call would clear the delta and leave 3D stale.
No SimulationBuilder, TaskGroup, phase, Manager, Context or Services API was added.

## Explicit exclusions

No GPU CI runner, Android engine build/CTest, Asset residency/demand API, ScriptSystem registration, Full 3D Streaming,
Jolt broadphase sweep or Linux TSAN qualification was added in this wave.
