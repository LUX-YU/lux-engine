#include <algorithm>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <unordered_map>

namespace lux::scene
{
    namespace
    {
        namespace loading = lux::process::world_loading;

        auto failed(EScenePackageError code, lux::asset::AssetId id = {}, std::size_t ordinal = 0)
        {
            return lux::cxx::unexpected(ScenePackageFailure{code, id, ordinal});
        }

        struct Cancelled final
        {};

        using VPartitionResult =
            std::variant<lux::world::WorldPartitionData, loading::WorldStorageRuntimeFailure, Cancelled>;

        struct PartitionReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            VPartitionResult& result;

            stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

            void set_value(lux::world::WorldPartitionData value) && noexcept
            {
                result = std::move(value);
            }

            void set_error(loading::WorldStorageRuntimeFailure error) && noexcept
            {
                result = error;
            }

            void set_stopped() && noexcept
            {
                result = Cancelled{};
            }
        };

        template <class T>
        lux::cxx::expected<std::shared_ptr<const T>, ScenePackageFailure> decodeAsset(
            std::span<const lux::asset::PakDecodedEntry> entries,
            lux::asset::AssetId id,
            const lux::asset::AssetDecodeLimits& limits
        )
        {
            const auto found =
                std::lower_bound(entries.begin(), entries.end(), id, [](const auto& entry, auto identity) {
                    return entry.metadata.id < identity;
                });
            if (found == entries.end() || found->metadata.id != id || found->metadata.tombstone)
            {
                return failed(EScenePackageError::MISSING_ASSET, id);
            }
            if (found->metadata.magic_number != T::primary_magic)
            {
                return failed(EScenePackageError::TYPE_MISMATCH, id);
            }
            auto result = lux::asset::TAssetSerDeser<T>::decode(id, found->bytes, limits);
            if (!result)
            {
                return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::DECODE, id, 0, result.error()});
            }
            return std::move(*result);
        }
    } // namespace

    lux::cxx::expected<ScenePackage, ScenePackageFailure> decodeScenePackage(
        const lux::cxx::SharedBytes<>& image,
        std::stop_token stop,
        bool retain_partition_sources
    )
    {
        constexpr std::size_t byte_limit = 256U * 1024U * 1024U;
        const lux::asset::AssetDecodeLimits limits{byte_limit, byte_limit, 128};
        if (image.size() > byte_limit)
        {
            return failed(EScenePackageError::LIMIT);
        }
        auto package = lux::asset::decodePak(image, 4096);
        if (!package)
        {
            return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::PACKAGE, {}, 0, package.error()});
        }
        const auto root = std::ranges::find_if(package->entries, [](const auto& entry) {
            return entry.metadata.vpath == "Scene" && !entry.metadata.tombstone;
        });
        if (root == package->entries.end())
        {
            return failed(EScenePackageError::MISSING_ASSET);
        }
        auto scene = decodeAsset<lux::scene::SceneAsset>(package->entries, root->metadata.id, limits);
        if (!scene)
        {
            return lux::cxx::unexpected(scene.error());
        }
        auto world = decodeAsset<lux::world::WorldAsset>(package->entries, (*scene)->data().world(), limits);
        if (!world)
        {
            return lux::cxx::unexpected(world.error());
        }
        auto simulation =
            decodeAsset<lux::simulation::SimulationAsset>(package->entries, (*scene)->data().simulation(), limits);
        if (!simulation)
        {
            return lux::cxx::unexpected(simulation.error());
        }

        ScenePackage result{std::move(*scene), std::move(*world), std::move(*simulation), {}, {}};
        std::unordered_map<std::string_view, const lux::asset::PakDecodedEntry*> paths;
        for (const auto& entry : package->entries)
        {
            if (!entry.metadata.tombstone)
            {
                paths.emplace(entry.metadata.vpath, &entry);
            }
        }
        const auto volumes = result.world->data().storageVolumes();
        result.volumes.reserve(volumes.size());
        for (std::size_t index = 0; index < volumes.size(); ++index)
        {
            const auto found = paths.find("Storage/" + std::to_string(index));
            if (found == paths.end())
            {
                return failed(EScenePackageError::MISSING_VOLUME, result.world->id(), index);
            }
            if (found->second->bytes.size() != volumes[index].file_size)
            {
                return failed(EScenePackageError::VOLUME_SIZE, result.world->id(), index);
            }
            result.volumes.push_back(found->second->bytes);
        }

        auto source = loading::makeWorldMemoryStorageSource(
            std::shared_ptr<const lux::world::WorldDescription>(result.world, &result.world->data()),
            result.volumes
        );
        if (!source)
        {
            return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::STORAGE, {}, 0, source.error()});
        }
        const auto count = result.world->data().partitionCount();
        if (count > byte_limit / sizeof(lux::world::WorldPartitionData))
        {
            return failed(EScenePackageError::LIMIT);
        }
        if (retain_partition_sources)
        {
            result.partitions.reserve(count);
        }
        auto remaining = byte_limit - result.partitions.capacity() * sizeof(result.partitions.front());
        for (std::uint32_t index = 0; retain_partition_sources && index < count; ++index)
        {
            if (stop.stop_requested())
            {
                return failed(EScenePackageError::CANCELLED);
            }
            VPartitionResult loaded{Cancelled{}};
            auto operation = stdexec::connect(
                loading::loadWorldPartition(*source, lux::partition::PartitionOrdinal{index}, remaining, stop),
                PartitionReceiver{loaded}
            );
            // The memory source completes synchronously; decoding performs no IO or wait.
            stdexec::start(operation);
            if (auto* value = std::get_if<lux::world::WorldPartitionData>(&loaded))
            {
                if (value->retainedBytes() > remaining)
                {
                    return failed(EScenePackageError::LIMIT);
                }
                remaining -= value->retainedBytes();
                result.partitions.push_back(std::make_shared<const lux::world::WorldPartitionData>(std::move(*value)));
            }
            else if (const auto* error = std::get_if<loading::WorldStorageRuntimeFailure>(&loaded))
            {
                return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::STORAGE, {}, index, *error});
            }
            else
            {
                return failed(EScenePackageError::CANCELLED);
            }
        }
        result.package = std::move(*package);
        return result;
    }

    lux::cxx::expected<std::vector<std::byte>, ScenePackageFailure> encodeScenePackage(
        const ScenePackage& source,
        std::size_t max_bytes,
        std::stop_token stop
    )
    {
        if (stop.stop_requested())
            return failed(EScenePackageError::CANCELLED);
        std::vector<lux::asset::PakWriteEntry> entries;
        entries.reserve(source.package.entries.size());
        for (const auto& entry : source.package.entries)
        {
            entries.push_back(
                {entry.metadata.id,
                 entry.metadata.magic_number,
                 entry.metadata.vpath,
                 {},
                 entry.bytes,
                 entry.metadata.tombstone}
            );
        }
        auto encoded = lux::asset::encodePak(entries, max_bytes, source.package.mount_hint);
        if (!encoded)
            return lux::cxx::unexpected(ScenePackageFailure{EScenePackageError::PACKAGE, {}, 0, encoded.error()});
        return std::move(*encoded);
    }
} // namespace lux::scene
