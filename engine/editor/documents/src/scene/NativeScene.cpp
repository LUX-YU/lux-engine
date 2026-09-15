#include <lux/engine/editor/scene/NativeScene.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <algorithm>
#include <unordered_map>

namespace lux::editor::scene
{
    namespace
    {
        namespace loading = lux::process::world_loading;

        auto failed(ENativeSceneError code, lux::asset::AssetId id = {}, std::size_t ordinal = 0)
        {
            return lux::cxx::unexpected(NativeSceneFailure{code, id, ordinal});
        }

        class CapturedVolumes final : public lux::async::OperationPort<loading::ReadWorldStorageRange>::Endpoint
        {
          public:
            explicit CapturedVolumes(std::span<const lux::cxx::SharedBytes<>> volumes) : volumes_(volumes) {}

            lux::async::SubmitResult submit(
                loading::ReadWorldStorageRange request, void *state,
                void (*complete)(void *, lux::async::OperationOutcome<loading::ReadWorldStorageRange> &&) noexcept,
                lux::async::SubmitOptions) noexcept override
            {
                using Failure = lux::async::OperationFailure<loading::WorldStorageRuntimeFailure>;
                if (request.volume >= volumes_.size())
                {
                    complete(state,
                             lux::cxx::unexpected(Failure::domain({loading::EWorldStorageRuntimeError::INVALID_VOLUME,
                                                                   request.volume, request.offset})));
                    return {};
                }
                const auto &bytes = volumes_[request.volume];
                if (request.offset > bytes.size() || request.size > bytes.size() - request.offset)
                {
                    complete(state,
                             lux::cxx::unexpected(Failure::domain({loading::EWorldStorageRuntimeError::RANGE_OVERFLOW,
                                                                   request.volume, request.offset})));
                    return {};
                }
                complete(state, bytes.subspan(static_cast<std::size_t>(request.offset),
                                              static_cast<std::size_t>(request.size)));
                return {};
            }

          private:
            std::span<const lux::cxx::SharedBytes<>> volumes_;
        };

        struct Cancelled final
        {
        };

        using PartitionResult =
            std::variant<lux::world::WorldPartitionData, loading::WorldStorageRuntimeFailure, Cancelled>;

        struct PartitionReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            PartitionResult &result;

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
        lux::cxx::expected<std::shared_ptr<const T>, NativeSceneFailure> decodeAsset(
            std::span<const lux::asset::PakDecodedEntry> entries, lux::asset::AssetId id,
            const lux::asset::AssetDecodeLimits &limits)
        {
            const auto found = std::lower_bound(entries.begin(), entries.end(), id, [](const auto &entry, auto identity)
                                                { return entry.metadata.id < identity; });
            if (found == entries.end() || found->metadata.id != id || found->metadata.tombstone)
            {
                return failed(ENativeSceneError::MISSING_ASSET, id);
            }
            if (found->metadata.magic_number != T::primary_magic)
            {
                return failed(ENativeSceneError::TYPE_MISMATCH, id);
            }
            auto result = lux::asset::TAssetSerDeser<T>::decode(id, found->bytes, limits);
            if (!result)
            {
                return lux::cxx::unexpected(NativeSceneFailure{ENativeSceneError::DECODE, id, 0, result.error()});
            }
            return std::move(*result);
        }
    } // namespace

    lux::cxx::expected<NativeScene, NativeSceneFailure> decodeNativeScene(const lux::cxx::SharedBytes<> &image,
                                                                          std::stop_token stop)
    {
        constexpr std::size_t byte_limit = 256U * 1024U * 1024U;
        const lux::asset::AssetDecodeLimits limits{byte_limit, byte_limit, 128};
        if (image.size() > byte_limit)
        {
            return failed(ENativeSceneError::LIMIT);
        }
        auto package = lux::asset::decodePak(image, 4096);
        if (!package)
        {
            return lux::cxx::unexpected(NativeSceneFailure{ENativeSceneError::PACKAGE, {}, 0, package.error()});
        }
        const auto root =
            std::ranges::find_if(package->entries, [](const auto &entry)
                                 { return entry.metadata.vpath == "Scene" && !entry.metadata.tombstone; });
        if (root == package->entries.end())
        {
            return failed(ENativeSceneError::MISSING_ASSET);
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

        NativeScene result{std::move(*scene), std::move(*world), std::move(*simulation), {}, {}};
        std::unordered_map<std::string_view, const lux::asset::PakDecodedEntry *> paths;
        for (const auto &entry : package->entries)
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
                return failed(ENativeSceneError::MISSING_VOLUME, result.world->id(), index);
            }
            if (found->second->bytes.size() != volumes[index].file_size)
            {
                return failed(ENativeSceneError::VOLUME_SIZE, result.world->id(), index);
            }
            result.volumes.push_back(found->second->bytes);
        }

        auto source = loading::WorldStorageSource::create(
            std::shared_ptr<const lux::world::WorldDescription>(result.world, &result.world->data()),
            lux::async::OperationPort<loading::ReadWorldStorageRange>{
                std::make_shared<CapturedVolumes>(result.volumes)});
        if (!source)
        {
            return lux::cxx::unexpected(NativeSceneFailure{ENativeSceneError::STORAGE, {}, 0, source.error()});
        }
        const auto count = result.world->data().partitionCount();
        if (count > byte_limit / sizeof(lux::world::WorldPartitionData))
        {
            return failed(ENativeSceneError::LIMIT);
        }
        result.partitions.reserve(count);
        auto remaining = byte_limit - result.partitions.capacity() * sizeof(lux::world::WorldPartitionData);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (stop.stop_requested())
            {
                return failed(ENativeSceneError::CANCELLED);
            }
            PartitionResult loaded{Cancelled{}};
            auto operation = stdexec::connect(
                loading::loadWorldPartition(*source, lux::partition::PartitionOrdinal{index}, remaining, stop),
                PartitionReceiver{loaded});
            // CapturedVolumes completes synchronously over owned bytes; no filesystem or wait occurs here.
            stdexec::start(operation);
            if (auto *value = std::get_if<lux::world::WorldPartitionData>(&loaded))
            {
                if (value->retainedBytes() > remaining)
                {
                    return failed(ENativeSceneError::LIMIT);
                }
                remaining -= value->retainedBytes();
                result.partitions.push_back(std::move(*value));
            }
            else if (const auto *error = std::get_if<loading::WorldStorageRuntimeFailure>(&loaded))
            {
                return lux::cxx::unexpected(NativeSceneFailure{ENativeSceneError::STORAGE, {}, index, *error});
            }
            else
            {
                return failed(ENativeSceneError::CANCELLED);
            }
        }
        return result;
    }
} // namespace lux::editor::scene
