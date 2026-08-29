# WorldDescription v2、多卷 World Storage 与 Bundle Identity 实施规范（v3.1）

> 状态：Normative。
>
> 施工 Phase：**2–3**。
>
> 影响：`engine/domain/world/core`、`engine/domain/world/asset`、`engine/domain/world/storage`。

---

## 1. 施工前置与类型预算

### Phase 2 allowed public types

仅允许新增：

```text
WorldBundleId
WorldBundleGeneration
WorldChunkReference
WorldStorageVolumeDescription
WorldPartitionTablePageDescription
WorldPartitionTable
WorldPartitionIndexDescription
```

优先修改/复用：

```text
WorldDescription
WorldDescriptionBuilder
WorldPartitionLayout
WorldPartitionLayoutBuilder
WorldPartitionBuildProduct
WorldPartitionerDescriptor
WorldPartitionIndexTypeId
WorldPartitionOrdinal
WorldPartitionId
WorldDataSchemaId
```

### Phase 3 allowed public types

仅允许新增：

```text
WorldPartitionData
WorldPartitionObjectView
```

Wire header/descriptor 默认 private。

明确禁止新增：

```text
WorldManifest
WorldRootDescription
WorldPartitionWorkspace  # FORBIDDEN
WorldStorageManager
WorldPartitionCatalog
WorldBundleManager
```

---

## 2. WorldDescription v2

`WorldDescription` 只保存 whole-world metadata：

```text
name
World bundle identity/generation
WorldData schema dictionary
partitioner descriptor
partition count
sidecar volume metadata
paged partition table directory
partition-index descriptors
```

不得包含：

```text
WorldObject[]
WorldData payload bytes
ECS Entity
runtime Component values
Process handles
Scene streaming state
```

canonical Asset type：

```text
lux.world.description
```

保留 `WorldDescription` 名称，不创建同义 root/manifest 类型。

---

## 3. 现有 partition build API 的强制迁移

当前 `WorldPartitionLayoutBuilder(const WorldDescription&)` 依赖旧 WorldDescription 中的 object universe。

v2 metadata-only 后该依赖必须删除。

精确迁移：

```cpp
class WorldPartitionLayoutBuilder final
{
public:
    explicit WorldPartitionLayoutBuilder(
        std::span<const WorldObjectId> objects
    );

    // existing addPartition/build semantics remain
};
```

Toolchain/Authoring 后续直接提供 object-id span：

```text
Authoring object IDs
    -> existing WorldPartitionLayoutBuilder
    -> existing WorldPartitionLayout
    -> existing WorldPartitionBuildProduct
    -> physicalizer
    -> WorldDescription v2 + sidecars
```

禁止为了替代旧构造器新增：

```text
WorldPartitionWorkspace  # FORBIDDEN
PartitionBuildContext
WorldCookContext
```

`WorldPartitionLayout` 继续是 exact-cover build product；runtime `WorldDescription` 不再作为 monolithic partition-build input。

---

## 4. Bundle physical model

```text
WorldName.luxasset       # root Asset: WorldDescription
WorldName.wvol0          # non-Asset sidecar
WorldName.wvol1
...
```

sidecar：

```text
no AssetId
not an AssetVfs asset
not individually imported/resolved as an Asset
```

Root + sidecars = one multipart bundle。

---

## 5. Strong bundle identity

新增：

```cpp
struct WorldBundleId final
{
    uuids::uuid value;
};

struct WorldBundleGeneration final
{
    uuids::uuid value;
};
```

语义：

```text
WorldBundleId
    = logical World bundle durable identity

WorldBundleGeneration
    = one physically consistent published cook generation
```

每次完整 publish/cook 生成新的 Generation。

Runtime 不接受 mixed generation。

不要用：

```text
World name
root AssetId
file timestamp
member filename
```

代替 generation consistency validation。

### Repository erratum — WorldChunkReference

v3.1 原始清单遗漏了后文 page/index descriptor 已经依赖的最小 chunk reference。
仓库 canonical baseline 将它明确补入 Phase 2 public type budget：

```cpp
struct WorldChunkReference final
{
    std::uint32_t volume{};
    std::uint32_t chunk{};
};
```

它只表示 volume ordinal + chunk ordinal，不携带 path、handle、offset、size 或 provider state。

---

## 6. WorldStorageVolumeDescription

精确语义：root 对每个 volume 记录 expected member metadata。

```cpp
struct WorldStorageVolumeDescription final
{
    std::string member_name;
    std::uint32_t format_version{};
    std::uint32_t chunk_count{};
    std::uint64_t file_size{};
};
```

`member_name` 必须是 bundle-relative logical member name。

禁止：

```text
absolute path
..
path traversal
OS file handle
provider-native path object
```

---

## 7. WorldDescription public shape

允许现有 naming style 调整 getter 拼写，但 semantic field 不得扩张：

```cpp
class WorldDescription final
{
public:
    [[nodiscard]] const WorldBundleId& bundleId() const noexcept;
    [[nodiscard]] const WorldBundleGeneration& generation() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;

    [[nodiscard]] std::span<const WorldDataSchemaId>
    schemas() const noexcept;

    [[nodiscard]] const WorldPartitionerDescriptor&
    partitioner() const noexcept;

    [[nodiscard]] std::uint32_t partitionCount() const noexcept;

    [[nodiscard]] std::span<const WorldStorageVolumeDescription>
    storageVolumes() const noexcept;

    [[nodiscard]] const WorldPartitionTable&
    partitionTable() const noexcept;

    [[nodiscard]] std::span<const WorldPartitionIndexDescription>
    partitionIndexes() const noexcept;
};
```

不得追加 runtime fields。

---

## 8. Sidecar header

Private wire 至少：

```cpp
struct WorldStorageVolumeHeaderWire final
{
    std::uint32_t magic;
    std::uint32_t version;

    std::array<std::byte, 16> bundle_id;
    std::array<std::byte, 16> generation;

    std::uint32_t volume_ordinal;
    std::uint32_t chunk_count;

    std::uint32_t descriptor_stride;
    std::uint32_t reserved;

    std::uint64_t descriptor_offset;
    std::uint64_t payload_offset;
    std::uint64_t file_size;
};
```

读取任何 payload 前必须验证：

```text
magic/version
BundleId
Generation
expected VolumeOrdinal
root format_version == header/supported version
root chunk_count == header chunk_count
root file_size == header file_size
file size bounds
descriptor bounds
```

Mismatch = structured failure；不得“尝试继续”。

---

## 9. Partition table

唯一职责：

```text
WorldPartitionOrdinal -> physical extents
```

不回答：

```text
position -> partition
tile -> partition
room -> partition
streaming desirability
```

Paged root directory：

```cpp
struct WorldPartitionTablePageDescription final
{
    WorldPartitionOrdinal first;
    std::uint32_t count{};
    WorldChunkReference chunk;
};
```

Hard requirements：

```text
pages sorted by first ordinal
exact-cover [0, partition_count)
no overlap/gap
root memory O(page_count), not O(partition_count/object_count)
page lookup binary-search or arithmetic O(logN)/O(1)
```

禁止线性扫描所有 pages 每次 query。

`WorldPartitionTable` 同样是原始 v3.1 清单遗漏但本节 public shape 已经引用的类型。
它只拥有 page descriptions，并提供：

```text
pages() -> span<const WorldPartitionTablePageDescription>
findPage(WorldPartitionOrdinal) -> const WorldPartitionTablePageDescription*
```

构造只由 `WorldDescriptionBuilder` 完成；lookup 使用 binary search 或等价 O(logN)/O(1) 方式。

---

## 10. Partition page wire

Private/World-storage record：

```cpp
struct WorldPartitionRecord final
{
    WorldPartitionId id;
    std::uint32_t first_extent{};
    std::uint32_t extent_count{};
};

struct WorldPartitionExtent final
{
    std::uint32_t volume{};
    std::uint32_t first_chunk{};
    std::uint32_t chunk_count{1};
};
```

允许：

```text
one partition -> multiple chunks
one partition -> multiple extents
one partition -> multiple volumes
```

Partition table page **不得保存 WorldObject list**。

---

## 11. Partition index descriptors

World core 只认识 descriptor：

```cpp
struct WorldPartitionIndexDescription final
{
    WorldPartitionIndexTypeId type;
    std::uint32_t version{};
    WorldChunkReference root;
};
```

World core 不拥有 runtime query interface。

Concrete domain 例如：

```text
scene/spatial3d
scene/spatial2d
a room/portal product
robot map product
```

自行解释 index root/pages/query。

增加新 index type 不得修改 World core enum。

---

## 12. Sidecar chunk format

```text
[fixed header]
[fixed-stride chunk descriptor array]
[alignment padding]
[payload 0]
[payload 1]
...
```

Private descriptor至少：

```cpp
struct WorldStorageChunkDescriptorWire final
{
    std::uint32_t kind;
    std::uint32_t codec;

    std::uint64_t offset;
    std::uint64_t stored_size;
    std::uint64_t decoded_size;

    std::array<std::byte, 32> digest;
};
```

固定 stride 支持：

```text
descriptor_offset + ordinal * descriptor_stride
```

单 descriptor range-read。

全部 offset/size = 64-bit。

---

## 13. v1 chunk kinds

至少：

```text
PARTITION_TABLE_PAGE
PARTITION_INDEX_PAGE
WORLD_PARTITION_DATA
```

禁止把普通 Assets塞进 World sidecar：

```text
TEXTURE
MESH
MATERIAL
SCRIPT
SHADER
```

这些仍属于 Asset domain。

---

## 14. Compression / digest

冻结：

```text
each chunk independently compressed
digest per chunk
no compression stream spans chunks
decoded_size validated before allocation
overflow checked before offset arithmetic
```

v1 codec 可只实现 `NONE`，但 wire codec field 保留。

不要因为只实现 NONE 而删除 codec field。

---

## 15. WorldPartitionData

Phase 3 public runtime value/view：

```cpp
class WorldPartitionData final
{
public:
    [[nodiscard]] WorldBundleId bundle() const noexcept;
    [[nodiscard]] WorldBundleGeneration generation() const noexcept;
    [[nodiscard]] WorldPartitionOrdinal partition() const noexcept;
    [[nodiscard]] std::size_t objectCount() const noexcept;
    [[nodiscard]] WorldPartitionObjectView objectAt(std::size_t index) const noexcept;
};
```

`WorldPartitionObjectView` 应是 non-owning view into the decoded partition value，不为每 object 单独 heap allocate。
它也暴露同一 bundle/generation identity，使 object-level materialization 能在创建 Entity 前验证 World。

Object view 至少能迭代：

```text
object-local data records
    schema ordinal
    encoded schema version
    payload span
```

具体 wire/private index 可自由优化，但不得引入 public object manager/catalog。

---

## 16. Minimum generic IO unit

冻结：

```text
minimum generic World IO unit = partition
```

因此：

```text
load P17
  -> all chunks/extents required for P17
  -> WorldPartitionData
```

之后可以：

```text
materialize all objects
materialize one object
materialize selected objects
inspect only
discard
```

这些 **不再次 IO**。

如果 P17 太大：

```text
fix Partitioner
```

不要添加：

```text
per-object storage index
loadWorldObject()
object-level disk random reads
```

到 generic runtime。

---

## 17. World data schema wire

Partition payload 使用 `schema_ordinal` 指向 `WorldDescription::schemas()`。

Stable schema 不等于 C++ layout。

禁止：

```text
memcpy raw C++ struct as durable wire
RefField::offset as persistent ABI
Eigen object binary image as stable schema
```

Decoder 入口必须知道：

```text
expected partition ordinal
schema count/version
byte limits
```

并进行 bounds/overflow validation。

---

## 18. Spatial precision

World core 不认识 Vec2/Vec3。

但 concrete spatial facts/index若表达 canonical coordinates：

```text
Transform placement
Spatial3D AABB
partition spatial bounds
robotics world/map pose
```

wire = double。

禁止：

```text
Authoring double -> cook float -> runtime double
```

Dense local geometry仍可 float。

---

## 19. Root wire v2

不做 v1 compatibility shim。

v2 logical section order 固定为：

```text
Header
BundleId
Generation
World name
Schema table
Partitioner descriptor
Storage volume table
Partition table page directory
Partition index descriptor table
```

Canonical requirements：

```text
schemas sorted/unique
volume ordinals dense [0,N)
member names unique
partition pages exact-cover
index type unique
explicit string lengths
all counts validated before allocation
```

---

## 20. Phase 2 tests

```text
WorldDescription contains no object payload
WorldPartitionLayoutBuilder(object-id span) exact cover
invalid/duplicate object assignment
BundleId/Generation validity
root retained-memory independent from object count
no WorldPartitionWorkspace type
```

## 21. Phase 3 tests/perf gates

```text
root/volume BundleId mismatch
root/volume Generation mismatch
wrong VolumeOrdinal
mixed-cook bundle rejection
path traversal member name rejection
multi-volume partition
multi-extent partition
truncated descriptor
64-bit overflow
invalid decoded_size
digest mismatch
1M partition root retained-memory benchmark
page lookup benchmark
exact range-read proof
WorldPartitionData no per-object heap baseline
```

---

## 22. Completion condition

只有当 Phase 2+3 全通过后，Phase 4/8 才能依赖 World v2。

禁止让 L3 runtime 临时兼容旧 WorldDescription payload model。
