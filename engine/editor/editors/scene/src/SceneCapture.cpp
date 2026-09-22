#include <lux/engine/editor/scene/NativeScene.hpp>

#include <algorithm>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::scene
{
namespace
{
template <class Value> auto failed(ENativeSceneError code, lux::asset::AssetId asset, std::size_t ordinal, Value cause)
{
    return lux::cxx::unexpected(NativeSceneFailure{code, asset, ordinal, std::move(cause)});
}

auto own(std::vector<std::byte> bytes)
{
    auto storage = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    return lux::cxx::SharedBytes<>::fromOwner(storage, *storage);
}
} // namespace

lux::cxx::expected<std::vector<std::byte>, NativeSceneFailure> encodeNativeScene(const SceneCapture &capture,
                                                                                 std::size_t max_bytes,
                                                                                 std::stop_token stop)
{
    namespace world = lux::world;
    const auto &source = *capture.source;
    const auto &before = source.world->data();
    const std::span<const world::WorldDataSchemaId> schemas =
        capture.schemas.empty() ? before.schemas() : std::span<const world::WorldDataSchemaId>(capture.schemas);
    std::vector<std::uint32_t> remapped_schemas;
    remapped_schemas.reserve(before.schemas().size());
    for (const auto &schema : before.schemas())
    {
        const auto found = std::ranges::lower_bound(schemas, schema, world::WorldDataSchemaIdLess{});
        if (found == schemas.end() || *found != schema)
        {
            return failed(ENativeSceneError::ENCODE, source.world->id(), 0,
                          std::string("Capture cannot discard an existing schema"));
        }
        remapped_schemas.push_back(static_cast<std::uint32_t>(found - schemas.begin()));
    }
    const auto rejected = [&](ENativeSceneError code, std::string message) {
        return failed(code, source.world->id(), 0, std::move(message));
    };
    if (stop.stop_requested())
    {
        return rejected(ENativeSceneError::CANCELLED, "Capture encoding was cancelled");
    }
    if (capture.components.empty() && !capture.structure_changed)
    {
        std::vector<lux::asset::PakWriteEntry> entries;
        for (const auto &entry : source.package.entries)
        {
            entries.push_back({entry.metadata.id,
                               entry.metadata.magic_number,
                               entry.metadata.vpath,
                               {},
                               entry.bytes,
                               entry.metadata.tombstone});
        }
        auto encoded = lux::asset::encodePak(entries, max_bytes, source.package.mount_hint);
        if (!encoded)
        {
            return rejected(ENativeSceneError::PACKAGE, std::move(encoded.error()));
        }
        return std::move(*encoded);
    }
    if (!before.partitionIndexes().empty())
    {
        return rejected(ENativeSceneError::INDEX_REBUILD_REQUIRED,
                        "Changed indexed content requires its registered partition index builder");
    }

    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(capture.components.size());
    for (const auto &component : capture.components)
    {
        if (stop.stop_requested())
        {
            return rejected(ENativeSceneError::CANCELLED, "Component encoding was cancelled");
        }
        auto encoded = component.value.encode(capture.identities, max_bytes);
        if (!encoded)
        {
            return failed(ENativeSceneError::ENCODE, source.world->id(), component.schema, encoded.error());
        }
        payloads.push_back(std::move(*encoded));
    }

    std::vector<std::vector<std::byte>> partitions;
    std::vector<world::WorldPartitionRecord> records;
    std::vector<world::WorldPartitionExtent> extents;
    std::size_t retained{};
    lux::cxx::algorithm::Sha256 digest;
    partitions.reserve(source.partitions.size());
    records.reserve(source.partitions.size());
    extents.reserve(source.partitions.size());
    std::unordered_map<world::WorldObjectId, world::WorldPartitionObjectView, world::WorldObjectIdHash> original;
    for (const auto &partition : source.partitions)
    {
        for (std::size_t index{}; index < partition->objectCount(); ++index)
        {
            const auto object = partition->objectAt(index);
            original.emplace(object.id(), object);
        }
    }
    std::vector<const SceneCapture::Object *> ordered;
    ordered.reserve(capture.objects.size());
    for (const auto &object : capture.objects)
    {
        if (object.partition.value >= source.partitions.size())
        {
            return rejected(ENativeSceneError::ENCODE, "Captured object has no World partition");
        }
        ordered.push_back(&object);
    }
    std::ranges::sort(ordered, [](const auto *first, const auto *second) {
        return first->partition == second->partition ? world::WorldObjectIdLess{}(first->id, second->id)
                                                     : first->partition.value < second->partition.value;
    });
    auto next_object = ordered.begin();
    for (const auto &partition : source.partitions)
    {
        const auto first_object = next_object;
        while (next_object != ordered.end() && (*next_object)->partition == partition->partition())
        {
            ++next_object;
        }
        std::size_t count{};
        for (auto item = first_object; item != next_object; ++item)
        {
            const auto found = original.find((*item)->id);
            if (found != original.end())
            {
                count += found->second.dataCount();
            }
            const auto changed = std::ranges::equal_range(capture.components, (*item)->id, world::WorldObjectIdLess{},
                                                          &CapturedComponent::object);
            count += static_cast<std::size_t>(changed.size());
        }
        std::vector<world::WorldEncodedDataRecord> data;
        std::vector<world::WorldEncodedObjectRecord> objects;
        data.reserve(count);
        objects.reserve(static_cast<std::size_t>(next_object - first_object));
        for (auto item = first_object; item != next_object; ++item)
        {
            const auto id = (*item)->id;
            const auto found = original.find(id);
            const auto object = found == original.end() ? world::WorldPartitionObjectView{} : found->second;
            const auto changed = std::ranges::equal_range(capture.components, id, world::WorldObjectIdLess{},
                                                          &CapturedComponent::object);
            auto replacement = changed.begin();
            const auto first = data.size();
            std::size_t index{};
            const auto old_count = object ? object.dataCount() : 0;
            while (index < old_count || replacement != changed.end())
            {
                const auto ordinal = index < old_count ? remapped_schemas[object.schemaOrdinalAt(index)] : UINT32_MAX;
                if (replacement != changed.end() && replacement->schema <= ordinal)
                {
                    const auto offset = static_cast<std::size_t>(replacement - capture.components.begin());
                    data.push_back({replacement->schema, replacement->version, payloads[offset]});
                    if (replacement->schema == ordinal)
                    {
                        ++index;
                    }
                    ++replacement;
                }
                else
                {
                    // Unknown and unchanged author payloads keep their exact bytes.
                    data.push_back({ordinal, object.schemaVersionAt(index), object.payloadAt(index)});
                    ++index;
                }
            }
            objects.push_back({id, std::span<const world::WorldEncodedDataRecord>(data).subspan(first)});
        }
        auto encoded = world::encodeWorldPartitionData(partition->partition(), objects);
        if (!encoded)
        {
            return failed(ENativeSceneError::ENCODE, source.world->id(), partition->partition().value, encoded.error());
        }
        if (encoded->size() > max_bytes - retained)
        {
            return rejected(ENativeSceneError::LIMIT, "Encoded partitions exceed the capture limit");
        }
        retained += encoded->size();
        digest.update(*encoded);
        partitions.push_back(std::move(*encoded));
        records.push_back({partition->id(), partition->partition().value, 1});
        extents.push_back({0, partition->partition().value + 1, 1});
    }
    if (records.empty())
    {
        // An empty World has no storage table to replace.
        std::vector<lux::asset::PakWriteEntry> entries;
        for (const auto &entry : source.package.entries)
        {
            entries.push_back({entry.metadata.id,
                               entry.metadata.magic_number,
                               entry.metadata.vpath,
                               {},
                               entry.bytes,
                               entry.metadata.tombstone});
        }
        auto encoded = lux::asset::encodePak(entries, max_bytes, source.package.mount_hint);
        if (!encoded)
        {
            return rejected(ENativeSceneError::PACKAGE, std::move(encoded.error()));
        }
        return std::move(*encoded);
    }

    const auto hash = digest.digest();
    const std::string generation_key(reinterpret_cast<const char *>(hash.data()), hash.size());
    const world::WorldBundleGeneration generation{
        uuids::uuid_name_generator(before.generation().value)(generation_key)};
    auto table = world::encodeWorldPartitionTablePage(lux::partition::PartitionOrdinal{0}, records, extents);
    if (!table)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, table.error());
    }
    std::vector<world::WorldStorageChunkInput> chunks;
    chunks.push_back({world::EWorldStorageChunkKind::PARTITION_TABLE_PAGE, world::EWorldStorageCodec::NONE, *table});
    for (const auto &partition : partitions)
    {
        chunks.push_back(
            {world::EWorldStorageChunkKind::WORLD_PARTITION_DATA, world::EWorldStorageCodec::NONE, partition});
    }
    auto volume = world::encodeWorldStorageVolume(before.bundleId(), generation, 0, chunks, max_bytes);
    if (!volume)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, volume.error());
    }
    world::WorldDescriptionBuilder builder;
    auto prepared = builder.setIdentity(before.bundleId(), generation, before.name());
    for (const auto &schema : schemas)
    {
        if (prepared)
        {
            prepared = builder.addSchema(schema);
        }
    }
    if (prepared)
    {
        prepared = builder.setPartitioner(before.partitioner(), before.partitionCount());
    }
    if (prepared)
    {
        prepared =
            builder.addStorageVolume({"World.wvol", 1, static_cast<std::uint32_t>(chunks.size()), volume->size()});
    }
    if (prepared)
    {
        prepared =
            builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0}, before.partitionCount(), {0, 0}});
    }
    if (!prepared)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, prepared.error());
    }
    auto description = std::move(builder).build();
    if (!description)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, description.error());
    }
    const auto auxiliary = source.world->auxiliaryPayloads();
    auto asset = world::WorldAsset::create(
        source.world->info(), std::make_shared<const world::WorldDescription>(std::move(*description)),
        std::vector<lux::asset::AssetAuxiliaryPayload>(auxiliary.begin(), auxiliary.end()));
    if (!asset)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, asset.error());
    }
    auto encoded_world =
        lux::asset::TAssetSerDeser<world::WorldAsset>::encode(**asset, lux::asset::AssetEncodeLimits{max_bytes});
    if (!encoded_world)
    {
        return failed(ENativeSceneError::ENCODE, source.world->id(), 0, encoded_world.error());
    }
    std::unordered_set<std::string> volume_paths;
    for (std::size_t index{}; index < source.volumes.size(); ++index)
    {
        volume_paths.insert("Storage/" + std::to_string(index));
    }
    std::vector<lux::asset::PakWriteEntry> entries;
    for (const auto &entry : source.package.entries)
    {
        if (entry.metadata.id != source.world->id() && !volume_paths.contains(entry.metadata.vpath))
        {
            entries.push_back({entry.metadata.id,
                               entry.metadata.magic_number,
                               entry.metadata.vpath,
                               {},
                               entry.bytes,
                               entry.metadata.tombstone});
        }
    }
    const auto original_world = std::ranges::find(source.package.entries, source.world->id(),
                                                  [](const auto &entry) { return entry.metadata.id; });
    entries.push_back({source.world->id(),
                       world::WorldAssetPrimaryMagic,
                       original_world->metadata.vpath,
                       {},
                       own(std::move(*encoded_world))});
    const auto storage = std::ranges::find(source.package.entries, std::string{"Storage/0"},
                                           [](const auto &entry) { return entry.metadata.vpath; });
    entries.push_back({storage->metadata.id, storage->metadata.magic_number, "Storage/0", {}, own(std::move(*volume))});
    if (stop.stop_requested())
    {
        return rejected(ENativeSceneError::CANCELLED, "Package encoding was cancelled");
    }
    auto encoded = lux::asset::encodePak(entries, max_bytes, source.package.mount_hint);
    if (!encoded)
    {
        return rejected(ENativeSceneError::PACKAGE, std::move(encoded.error()));
    }
    return std::move(*encoded);
}
} // namespace lux::editor::scene
