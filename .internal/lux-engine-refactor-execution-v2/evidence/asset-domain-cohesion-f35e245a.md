# Asset 领域内聚基线与 wire 验收证据

**基线：** `f35e245a1e493c388722a41711f1a3ecd1df2acb`

**实施：** `1364810c`、`e7348155`

## 基线公共面

- `modules/resource/asset/include` 共 32 个安装头。
- 唯一库/target/component 已是 `lux_engine_asset` / `lux::engine::resource::asset` / `asset`。
- 基线仍包含根部领域头、`codecs/`、`pak/`、`BuiltinAssetIds.hpp` 和 Toolchain 对 Pak `pinclude` 的越界访问；实施后均归零。
- 安装导出仍只有 Resource `asset` component；新增 Engine Content 作为独立 `lux-engine-content/content` component，不改变 Resource component 身份。

## 冻结 wire 指纹

下列 fixture 由确定性输入生成；迁移后验证当前格式 decode/re-encode 逐字节一致，并验证历史 AssetFileHeader v1 可读。任何有意变更必须先升级格式版本。

| Fixture | 字节数 | SHA-256 |
| --- | ---: | --- |
| texture | 820 | `e01de6ccfb600f997b0ad08035acbda1c404647faa86284e4dcd28a03efed3cc` |
| texture_atlas | 477 | `2c0a7f6353760c6994065c143b169707c16191899076b53c0814604e5a86d2e1` |
| flipbook_clip | 469 | `38e7fa62a043f95947ba06b0a756118ec86ea33250195791038e541747a15533` |
| material | 495 | `34ddba8c3a78463d048553fa3d44481a737646895b6797e8fb62e19e9bd1fd8f` |
| material_instance | 472 | `a35ae4037601ccabd656b458ce979beec7947ccb3081345ddb3955fabbb6d495` |
| mesh | 708 | `55d3667e298f4b5a358cdd9979b348323d5c911ff5f5971f55beeaf181b5f765` |
| model | 485 | `715aa44f5c17fb9fcf23fb2f91bc35b2c4b9db084ba6325ce5dcfc92822558be` |
| script | 531 | `0c9673e7e98a1aa11c027658ab12d4e7f42d75c8a5885ef89453d9836cc886ed` |
| shader | 423 | `ba6d84448d95af9a83ba28d16cf8899f5c788595c2c94078b86ae6ea6ed57a9d` |
| skeleton | 576 | `0f9757141b0f49ac269a74901050c96d378d71858227e31b58e6aeca0ece0248` |
| animation_clip | 496 | `c88929b5122c40953854b8828d48b87022a7711b8705e249d2df7145ef0baf50` |
| LUXPAK v2 / 1000 entries | 163840 | `18b617f54954c5ec5548c8f38b460603459c03bdf50ea598815c3791edf5e715` |

## 验收结论

- Asset/Pak 格式、magic、UUID 排序、16 字节 payload 对齐、4 KiB B+tree page 与 SHA-256 行为未改变。
- 版本化 header 校验按 v1/v2 实际 header 大小执行；修复前被误拒绝的 v1 image 现可读取，v2 fixture 指纹不变。
- Public Pak writer/inspector/provider、opaque VFS、AssetRef 账本、Catalog 冲突、Engine Content UUID 与 ECS fallback 注入契约均通过。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 的 Windows RelWithDebInfo `target all -j 4 -k 0` 通过，第二轮均为 `ninja: no work to do`。
