#include <lux/engine/editor/scene/SceneSnapshot.hpp>
#include <algorithm>

namespace lux::editor::scene
{
    namespace
    {
        constexpr std::size_t limit = 256U * 1024U * 1024U;
        auto failed(lux::scene::EScenePackageError code, std::string message = {})
        {
            return lux::cxx::unexpected(lux::scene::ScenePackageFailure{.code = code, .stage = std::move(message)});
        }
        template <class Asset>
        lux::cxx::expected<void, lux::scene::ScenePackageFailure> preserveAuxiliary(
            std::shared_ptr<const Asset>& to,
            const Asset& from,
            lux::scene::ScenePackage& package
        )
        {
            const auto auxiliary = from.auxiliaryPayloads();
            auto next = Asset::create(to->info(), to->sharedData(), {auxiliary.begin(), auxiliary.end()});
            if (!next)
                return failed(lux::scene::EScenePackageError::ENCODE, "Asset auxiliary payload");
            to = std::move(*next);
            auto encoded = asset::TAssetSerDeser<Asset>::encode(*to, asset::AssetEncodeLimits{limit});
            if (!encoded)
                return failed(lux::scene::EScenePackageError::ENCODE, "Asset root encoding");
            auto found = std::ranges::find(package.package.entries, to->id(), [](const auto& entry) {
                return entry.metadata.id;
            });
            if (found == package.package.entries.end())
                return failed(lux::scene::EScenePackageError::ENCODE);
            found->bytes = lux::cxx::SharedBytes<>::copyOf(*encoded);
            found->metadata.size = found->bytes.size();
            lux::cxx::algorithm::Sha256 hash;
            hash.update(found->bytes.view());
            found->metadata.content_digest = hash.digest();
            return {};
        }
    }
    lux::cxx::expected<lux::scene::ScenePackage, lux::scene::ScenePackageFailure> buildSceneSnapshotPackage(
        const SceneSnapshot& snapshot,
        std::stop_token stop
    )
    {
        if (stop.stop_requested())
            return failed(lux::scene::EScenePackageError::CANCELLED);
        const auto& config = snapshot.configuration();
        const auto& world = config.world->data();
        if (!world.partitionIndexes().empty())
            return failed(
                lux::scene::EScenePackageError::INDEX_REBUILD_REQUIRED,
                "Indexed source requires its registered index builder"
            );
        if (snapshot.partitionIds().size() != world.partitionCount())
            return failed(lux::scene::EScenePackageError::ENCODE, "Partition identities");
        std::vector<std::shared_ptr<const lux::world::WorldPartitionData>> partitions;
        std::size_t bytes_used{};
        for (std::uint32_t ordinal{}; ordinal < world.partitionCount(); ++ordinal)
        {
            if (stop.stop_requested())
                return failed(lux::scene::EScenePackageError::CANCELLED);
            std::vector<std::vector<lux::world::WorldEncodedDataRecord>> data;
            std::vector<lux::world::WorldEncodedObjectRecord> objects;
            data.reserve(snapshot.objects().size());
            objects.reserve(snapshot.objects().size());
            for (const auto& object : snapshot.objects())
            {
                if (object.partition.value != ordinal)
                    continue;
                auto& components = data.emplace_back();
                for (const auto& component : object.components)
                {
                    auto schema =
                        std::ranges::find(world.schemas(), component.schema.name, &lux::world::WorldDataSchemaId::name);
                    if (schema == world.schemas().end())
                        return failed(lux::scene::EScenePackageError::ENCODE, component.schema.name);
                    components.push_back(
                        {std::uint32_t(schema - world.schemas().begin()), component.version, component.bytes}
                    );
                }
                std::ranges::sort(components, {}, &lux::world::WorldEncodedDataRecord::schema_ordinal);
                objects.push_back({object.id, components});
            }
            std::ranges::sort(objects, lux::world::WorldObjectIdLess{}, &lux::world::WorldEncodedObjectRecord::id);
            auto encoded = lux::world::encodeWorldPartitionData({ordinal}, objects);
            if (!encoded)
                return failed(
                    lux::scene::EScenePackageError::ENCODE,
                    "Partition encode " + std::to_string(unsigned(encoded.error().code))
                );
            if (encoded->size() > limit - bytes_used)
                return failed(lux::scene::EScenePackageError::LIMIT, "Partition bytes");
            bytes_used += encoded->size();
            auto decoded = lux::world::decodeWorldPartitionData(
                *encoded,
                world.bundleId(),
                world.generation(),
                {ordinal},
                snapshot.partitionIds()[ordinal],
                static_cast<std::uint32_t>(world.schemas().size()),
                limit,
                stop
            );
            if (!decoded)
                return failed(
                    lux::scene::EScenePackageError::ENCODE,
                    "Partition decode " + std::to_string(unsigned(decoded.error().code))
                );
            partitions.push_back(std::make_shared<const lux::world::WorldPartitionData>(std::move(*decoded)));
        }
        lux::scene::ScenePackage source{config.scene, config.world, config.simulation};
        source.package = snapshot.package();
        auto package = lux::scene::assembleScenePackage(source, partitions, stop);
        if (!package)
            return failed(lux::scene::EScenePackageError::ENCODE, package.error().stage);
        {
            auto preserved = preserveAuxiliary(package->scene, *config.scene, *package);
            if (preserved)
                preserved = preserveAuxiliary(package->world, *config.world, *package);
            if (preserved)
                preserved = preserveAuxiliary(package->simulation, *config.simulation, *package);
            if (!preserved)
                return lux::cxx::unexpected(preserved.error());
            for (const auto& entry : snapshot.package().entries)
            {
                const bool root = entry.metadata.id == config.scene->id() || entry.metadata.id == config.world->id() ||
                                  entry.metadata.id == config.simulation->id();
                bool storage{};
                for (std::size_t i{}; i < snapshot.volumes().size(); ++i)
                    storage = storage || entry.metadata.vpath == "Storage/" + std::to_string(i);
                if (!root && !storage)
                    package->package.entries.push_back(entry);
            }
            package->package.mount_hint = snapshot.package().mount_hint;
        }
        return std::move(*package);
    }
}
