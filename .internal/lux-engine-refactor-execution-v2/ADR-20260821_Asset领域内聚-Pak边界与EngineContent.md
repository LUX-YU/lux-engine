# ADR：Asset 领域内聚、Pak 公共边界与 Engine Content

**状态：** Implemented

**日期：** 2026-08-21

**代码施工基线：** `f35e245a1e493c388722a41711f1a3ecd1df2acb`

**实施提交：** `1364810c`（owner/目录/API 迁移）、`e7348155`（wire/storage 契约与 v1 兼容修复）

**关联裁决：** `ADR-20260820_SceneAsset与Resource边界.md`

## 1. 背景

`modules/resource/asset` 已经收敛为单一公共组件，并保留了通用的
`LuxAsset`、`AssetManager`、`TAssetSerDeser`、Catalog、Provider、VFS 与 Pak。
当前源码仍按 `core/codecs/pak` 横向分区，导致一个资产族的值、SerDeser、私有
Description Codec、实现和测试被拆散；Engine Toolchain 还直接包含 Resource
`pinclude` 中的 Pak wire 实现。

Resource 同时公开 `BuiltinAssetIds.hpp`。这些冻结 UUID、`M_Missing` 与演示色板是
Lux Engine 默认内容契约，不是外部 Asset SDK 的通用语义。ECS Render 直接引用该头，
也使默认产品策略反向渗入 ECS。

## 2. 裁决

### 2.1 Asset 按领域族内聚

公共与私有源码按以下领域镜像组织：

```text
asset/
├── texture/   # Texture、TextureAtlas、FlipbookClip
├── material/  # Material、MaterialInstance
├── mesh/
├── model/
├── animation/ # Skeleton、AnimationClip
├── shader/
├── script/
└── storage/   # Provider、VFS、VirtualPath、Pak
```

`AssetCodecCatalog.hpp` 等跨领域资产核心合同保留在 Asset 根部。完整 `.luxasset`
转换统一使用 `*SerDeser`；被动值段转换继续使用私有 `*DescriptionCodec`。
`TextureCodec` 与 `ModelCodec` 分别改名为 `TextureSerDeser` 与 `ModelSerDeser`。
旧头、类型 alias、namespace alias 和 forwarding header 一律不保留。

### 2.2 Provider、VFS 与引用账本

Provider 是 `asset_id_t`/`VirtualPath` 到 opaque bytes 的存储接口，只负责
`resolve/contains/open/enumerate/pathOf`。它不创建 `LuxAsset`，不解释 Engine Scene
语义，也不修改资产引用计数。

```text
IAssetProvider -> AssetVfs -> AssetBlob
                              |-> Catalog/SerDeser -> AssetManager
                              `-> Scene Section Codec（不进入 AssetManager）
```

`AssetManager + AssetRef` 是唯一资产引用账本。Scene Section、Spatial 分区等记录可以
复用 Provider/VFS，但直接读取不产生 `AssetRef`，不参与驻留、revision 或驱逐。

### 2.3 Pak 是公共 Asset SDK 的存储格式

LUXPAK v2 的 Provider、writer 与 inspector 都属于 `modules/resource/asset/storage/pak`。
公共 API 提供 `PakWriteEntry`、`writePakFile()`、`PakInspectEntry`、`PakInspectInfo` 和
`inspectPak()`。Writer 只验证结构、路径、ID、magic、payload、排序、对齐、索引和
digest，不按 `EAssetType` 或 Catalog 解释条目语义，因此可以承载 Engine Scene 与
Scene Section 的原始 magic。

Engine Toolchain 只拥有 source 扫描、authoring auxiliary 剥离、Catalog/magic 策略、
冲突诊断、Scene/Game cook 组合与发布策略。Engine 不得包含 Resource `pinclude`，也不
得访问 `lux::asset::detail::Pak*`。

### 2.4 Engine 默认内容归 `engine/content`

新建轻量 `lux::engine::content` 组件，仅拥有冻结的内置资产 UUID、`M_Missing` 身份和
内置色板。它复用公共 `asset_id_t`，但不拥有 AssetManager、Codec、Provider、IO、异步
加载、几何生成或注册逻辑。

ECS 不依赖 Engine Content。`ResidencySubsystem` 接收可选的 fallback material ID：
Engine Runtime 在装配点注入 `builtinMissingMaterialId()`；通用 ECS/headless consumer
可以传 nil，明确禁用默认材质请求。Engine Toolchain builtin baker 继续拥有几何生成，
但其冻结身份来自 `engine/content`。

## 3. 公共结构

```text
modules/resource/asset/include/lux/engine/resource/asset/
├── Asset*.hpp
├── texture/
├── material/
├── mesh/
├── model/
├── animation/
├── shader/
├── script/
└── storage/
    ├── AssetProvider.hpp
    ├── AssetVfs.hpp
    ├── VirtualPath.hpp
    └── pak/
        ├── PakArchive.hpp
        └── PakAssetProvider.hpp

engine/content/include/lux/engine/content/
└── BuiltinAssetIds.hpp
```

单一 Resource target `lux::engine::resource::asset`、`lux::asset` namespace 与安装
component `asset` 保持不变。Engine Content target 为 `lux::engine::content`，安装到
`lux-engine-content` 的 `content` component。

## 4. 兼容性

以下字节合同不得改变：

- AssetFileHeader v1/v2；
- 所有标准资产内部 wire、magic 与 `EAssetType` 数值；
- 所有冻结内置 UUID 与颜色；
- LUXPAK v2 header、4 KiB B+tree page、16-byte payload alignment、SHA-256 与排序。

本 ADR 允许源码、target 依赖和安装头路径发生破坏性迁移，不允许格式升级或兼容 shim。

## 5. 验收

- 每个标准资产具备 deterministic length/SHA、decode/re-encode、legacy v1 与坏输入契约；
- Catalog 覆盖 type、main/legacy magic、C++ type hash/name 冲突；
- AssetRef 生命周期和 1->0 广播语义不变；
- Provider/VFS 的 opaque Section 读取不改变 AssetManager 账本；
- Resource 公共 Pak writer/inspector/provider 覆盖正常、冲突、截断与 digest 损坏；
- ECS fallback 的 nil/非 nil 两条路径均有 owner tests，ECS link closure 不含 Engine Content；
- installed consumer 可独立写、检查并读取 Pak；
- 安装树与全仓不存在旧 `codecs/`、旧 `pak/`、旧根部领域头、旧类名和 Resource
  `BuiltinAssetIds.hpp`。

## 6. 非目标

本轮不创建第二套 AssetStore、Provider、AssetId 或 AssetTypeId；不推进 M0、全局 M7
命名迁移、Scene/Registry/Editor 架构重写或 Extension ABI；不改变任何 wire/schema。
