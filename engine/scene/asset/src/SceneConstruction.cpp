#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <algorithm>

namespace lux::scene
{
    lux::cxx::expected<ScenePackage, ScenePackageFailure> copyScenePackage(
        const ScenePackage& source,
        asset::AssetId id,
        std::stop_token stop
    )
    {
        const bool unknown_roots = !source.scene->auxiliaryPayloads().empty() ||
                                   !source.world->auxiliaryPayloads().empty() ||
                                   !source.simulation->auxiliaryPayloads().empty();
        const bool unsupported_structure = !source.world->data().partitionIndexes().empty() ||
                                           source.package.entries.size() != source.volumes.size() + 3;
        if (unknown_roots || unsupported_structure)
            return lux::cxx::unexpected(ScenePackageFailure{
                EScenePackageError::INVALID_ARGUMENT,
                id,
                0,
                std::string("Package extensions or indexes contain identity references that cannot be rewritten safely"
                ),
                "scene.save-as.extensions"
            });
        return assembleScenePackage(
            id,
            source.world->data().name(),
            source.world->data().schemas(),
            source.world->data().partitioner(),
            source.partitions,
            source.simulation->sharedData(),
            source.scene->data(),
            stop
        );
    }
    namespace
    {
        constexpr std::size_t kSourceLimit = 256U * 1024U * 1024U;
        template <class Error> auto failed(std::string domain, Error error)
        {
            return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::ENCODE, {}, 0, error, std::move(domain)}
            );
        }
        auto bytes(std::vector<std::byte> value)
        {
            auto owner = std::make_shared<const std::vector<std::byte>>(std::move(value));
            return lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
        }
        void append(
            lux::scene::ScenePackage& output,
            asset::AssetId id,
            std::uint32_t magic,
            std::string path,
            lux::cxx::SharedBytes<> content
        )
        {
            lux::cxx::algorithm::Sha256 digest;
            digest.update(content.view());
            output.package.entries.push_back(
                {{id, magic, std::move(path), 0, content.size(), 0, false, digest.digest()}, content}
            );
        }
        template <class Asset>
        lux::cxx::expected<void, ScenePackageFailure> append(
            lux::scene::ScenePackage& output,
            const std::shared_ptr<const Asset>& value,
            std::string path
        )
        {
            auto encoded = asset::TAssetSerDeser<Asset>::encode(*value, asset::AssetEncodeLimits{kSourceLimit});
            if (!encoded)
                return failed("scene.source.encode", encoded.error());
            append(output, value->id(), Asset::primary_magic, std::move(path), bytes(std::move(*encoded)));
            return {};
        }
    }

    static lux::cxx::expected<ScenePackage, ScenePackageFailure> assemble(
        asset::AssetId id,
        std::string_view name,
        std::span<const lux::world::WorldDataSchemaId> schemas,
        lux::world::WorldPartitionerDescriptor partitioner,
        std::span<const std::shared_ptr<const lux::world::WorldPartitionData>> partitions,
        std::shared_ptr<const lux::simulation::SimulationDescription> simulation,
        const lux::scene::SceneDescription& scene,
        std::stop_token stop,
        const ScenePackage* source
    ) noexcept
    try
    {
        const bool invalid_input = id.isNull() || name.empty() || !simulation || partitions.empty() ||
                                   std::ranges::any_of(partitions, [](const auto& value) { return !value; });
        if (invalid_input)
            return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::INVALID_ARGUMENT});
        namespace world = lux::world;
        uuids::uuid_name_generator identity{id.uuid()};
        const auto bundle = source ? source->world->data().bundleId() : world::WorldBundleId{identity("world-bundle")};
        const auto world_id = source ? source->world->id() : asset::AssetId{identity("world")};
        const auto simulation_id = source ? source->simulation->id() : asset::AssetId{identity("simulation")};
        if (partitions.size() > UINT32_MAX || partitions.empty())
            return lux::cxx::unexpected(
                ScenePackageFailure{EScenePackageError::INVALID_ARGUMENT, {}, 0, {}, "scene.source.partitions"}
            );
        std::vector<std::vector<std::byte>> encoded_partitions;
        std::vector<world::WorldPartitionRecord> records;
        std::vector<world::WorldPartitionExtent> extents;
        std::size_t retained{};
        lux::cxx::algorithm::Sha256 content_digest;
        for (std::size_t ordinal{}; ordinal < partitions.size(); ++ordinal)
        {
            if (stop.stop_requested())
                return lux::cxx::unexpected(
                    ScenePackageFailure{EScenePackageError::CANCELLED, {}, 0, {}, "scene.source.copy"}
                );
            auto encoded = world::encodeWorldPartitionData(*partitions[ordinal]);
            if (!encoded)
                return failed("scene.source.partition", encoded.error());
            if (encoded->size() > kSourceLimit - retained)
                return lux::cxx::unexpected(
                    ScenePackageFailure{EScenePackageError::LIMIT, {}, 0, {}, "scene.source.partition"}
                );
            retained += encoded->size();
            content_digest.update(*encoded);
            encoded_partitions.push_back(std::move(*encoded));
            const auto index = static_cast<std::uint32_t>(ordinal);
            records.push_back({partitions[ordinal]->id(), index, 1});
            extents.push_back({0, index + 1, 1});
        }
        const auto digest = content_digest.digest();
        const std::string generation_key(reinterpret_cast<const char*>(digest.data()), digest.size());
        const auto seed = source ? source->world->data().generation().value : identity("world-generation");
        const world::WorldBundleGeneration generation{uuids::uuid_name_generator(seed)(generation_key)};
        auto table = world::encodeWorldPartitionTablePage({0}, records, extents);
        if (!table)
            return failed("scene.source.table", table.error());
        std::vector<world::WorldStorageChunkInput> chunks;
        chunks.push_back({world::EWorldStorageChunkKind::PARTITION_TABLE_PAGE, world::EWorldStorageCodec::NONE, *table}
        );
        for (const auto& part : encoded_partitions)
            chunks.push_back(
                {world::EWorldStorageChunkKind::WORLD_PARTITION_DATA, world::EWorldStorageCodec::NONE, part}
            );
        auto volume = world::encodeWorldStorageVolume(bundle, generation, 0, chunks, kSourceLimit);
        if (!volume)
            return failed("scene.source.volume", volume.error());
        world::WorldDescriptionBuilder world_builder;
        auto valid = world_builder.setIdentity(bundle, generation, name);
        for (const auto& schema : schemas)
            if (valid)
                valid = world_builder.addSchema(schema);
        if (valid)
            valid = world_builder.setPartitioner(std::move(partitioner), static_cast<std::uint32_t>(partitions.size()));
        if (valid)
            valid = world_builder.addStorageVolume(
                {"World.wvol", 1, static_cast<std::uint32_t>(chunks.size()), volume->size()}
            );
        if (valid)
            valid = world_builder.addPartitionTablePage({{0}, static_cast<std::uint32_t>(partitions.size()), {0, 0}});
        if (!valid)
            return failed("scene.source.world", valid.error());
        auto world_description = std::move(world_builder).build();
        if (!world_description)
            return failed("scene.source.world", world_description.error());
        lux::scene::SceneDescriptionBuilder description;
        description.setWorld(world_id);
        description.setSimulation(simulation_id);
        for (std::size_t i{}; i < scene.systemCount(); ++i)
        {
            const auto system = scene.systemAt(i);
            auto added = description.addSystem(
                system.instanceId(),
                system.instanceName(),
                system.type(),
                system.version(),
                system.configurationSchemaName(),
                system.configurationSchemaVersion(),
                system.configurationPayload()
            );
            if (!added)
                return failed("scene.source.system", added.error());
            for (std::size_t j{}; j < system.requirementBindingCount(); ++j)
            {
                const auto binding = system.requirementBindingAt(j);
                auto bound =
                    description.bindRequirement(system.instanceId(), binding.requirement(), binding.provider());
                if (!bound)
                    return failed("scene.source.binding", bound.error());
            }
        }
        for (std::size_t i{}; i < scene.dependencyCount(); ++i)
        {
            const auto edge = scene.dependencyAt(i);
            auto added = description.addDependency(edge.before(), edge.after());
            if (!added)
                return failed("scene.source.dependency", added.error());
        }
        auto scene_description = std::move(description).build();
        if (!scene_description)
            return failed("scene.source.description", scene_description.error());
        auto world_asset = world::WorldAsset::create(
            source ? source->world->info() : asset::AssetInfo{world_id},
            std::make_shared<const world::WorldDescription>(std::move(*world_description))
        );
        if (!world_asset)
            return failed("scene.source.world-asset", world_asset.error());
        auto simulation_asset = lux::simulation::SimulationAsset::create(
            source ? source->simulation->info() : asset::AssetInfo{simulation_id},
            std::move(simulation)
        );
        if (!simulation_asset)
            return failed("scene.source.simulation-asset", simulation_asset.error());
        auto scene_asset = lux::scene::SceneAsset::create(
            source ? source->scene->info() : asset::AssetInfo{id},
            std::make_shared<const lux::scene::SceneDescription>(std::move(*scene_description))
        );
        if (!scene_asset)
            return failed("scene.source.scene-asset", scene_asset.error());
        lux::scene::ScenePackage result{*scene_asset, *world_asset, *simulation_asset};
        result.package.mount_hint = "/Scene";
        auto encoded = append(result, result.world, "World");
        if (encoded)
            encoded = append(result, result.simulation, "Simulation");
        if (encoded)
            encoded = append(result, result.scene, "Scene");
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        result.volumes.push_back(bytes(std::move(*volume)));
        append(result, asset::AssetId{identity("storage/0")}, 1, "Storage/0", result.volumes.front());
        if (source)
        {
            result.package.mount_hint = source->package.mount_hint;
            for (auto& entry : result.package.entries)
            {
                const auto original = std::ranges::find_if(source->package.entries, [&](const auto& candidate) {
                    return candidate.metadata.id == entry.metadata.id ||
                           (entry.metadata.vpath == "Storage/0" && candidate.metadata.vpath == "Storage/0");
                });
                if (original == source->package.entries.end())
                    continue;
                const auto size = entry.metadata.size;
                const auto digest = entry.metadata.content_digest;
                entry.metadata = original->metadata;
                entry.metadata.size = size;
                entry.metadata.content_digest = digest;
            }
        }
        std::ranges::sort(result.package.entries, std::less<asset::AssetId>{}, [](const auto& entry) {
            return entry.metadata.id;
        });
        for (std::size_t i{}; i < partitions.size(); ++i)
        {
            auto decoded = world::decodeWorldPartitionData(
                encoded_partitions[i],
                bundle,
                generation,
                {static_cast<std::uint32_t>(i)},
                records[i].id,
                static_cast<std::uint32_t>(schemas.size()),
                kSourceLimit,
                stop
            );
            if (!decoded)
                return failed("scene.source.partition", decoded.error());
            result.partitions.push_back(std::make_shared<const world::WorldPartitionData>(std::move(*decoded)));
        }
        return result;
    }
    catch (...)
    {
        return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::LIMIT});
    }

    lux::cxx::expected<ScenePackage, ScenePackageFailure> assembleScenePackage(
        asset::AssetId id,
        std::string_view name,
        std::span<const world::WorldDataSchemaId> schemas,
        world::WorldPartitionerDescriptor partitioner,
        std::span<const std::shared_ptr<const world::WorldPartitionData>> partitions,
        std::shared_ptr<const simulation::SimulationDescription> simulation,
        const SceneDescription& scene,
        std::stop_token stop
    ) noexcept
    {
        return assemble(
            id,
            name,
            schemas,
            std::move(partitioner),
            partitions,
            std::move(simulation),
            scene,
            stop,
            nullptr
        );
    }

    lux::cxx::expected<ScenePackage, ScenePackageFailure> assembleScenePackage(
        const ScenePackage& source,
        std::span<const std::shared_ptr<const world::WorldPartitionData>> partitions,
        std::stop_token stop
    ) noexcept
    {
        const auto& world = source.world->data();
        return assemble(
            source.scene->id(),
            world.name(),
            world.schemas(),
            world.partitioner(),
            partitions,
            source.simulation->sharedData(),
            source.scene->data(),
            stop,
            &source
        );
    }

    lux::cxx::expected<ScenePackage, ScenePackageFailure> createScenePackage(
        asset::AssetId id,
        std::string_view name,
        std::span<const lux::world::WorldDataSchemaId> schemas,
        std::shared_ptr<const lux::simulation::SimulationDescription> simulation,
        const lux::scene::SceneDescription& scene
    ) noexcept
    try
    {
        namespace world = lux::world;
        uuids::uuid_name_generator identity{id.uuid()};
        auto data = world::encodeWorldPartitionData({0}, {});
        if (!data)
            return failed("scene.new.partition", data.error());
        auto partition = world::decodeWorldPartitionData(
            *data,
            {identity("world-bundle")},
            {identity("world-generation")},
            {0},
            world::WorldPartitionId{identity("partition/0")},
            static_cast<std::uint32_t>(schemas.size()),
            kSourceLimit
        );
        if (!partition)
            return failed("scene.new.partition", partition.error());
        const std::array parts{std::make_shared<const world::WorldPartitionData>(std::move(*partition))};
        return assembleScenePackage(
            id,
            name,
            schemas,
            {world::worldPartitionerId("lux.spatial.builtin.single"), 1},
            parts,
            std::move(simulation),
            scene,
            {}
        );
    }
    catch (...)
    {
        return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::LIMIT, {}, 0, {}, "scene.source.create"});
    }

}
