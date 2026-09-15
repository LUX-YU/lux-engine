#pragma once
#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <map>

struct SceneSourceChecks final
{
    using Id = lux::world::WorldObjectId;
    using IdLess = lux::world::WorldObjectIdLess;
    struct Opaque final
    {
        std::uint32_t partition, version;
        lux::world::WorldPartitionId partition_id;
        std::vector<std::byte> bytes;
        bool operator==(const Opaque &) const = default;
    };
    std::map<Id, Opaque, IdLess> opaque;
    std::map<Id, std::uint32_t, IdLess> membership;
    std::vector<lux::world::WorldPartitionId> partitions;
    std::vector<std::byte> scene, simulation;
    lux::world::WorldBundleId bundle;

    class VolumeReader final : public lux::async::OperationPort<lux::process::world_loading::ReadWorldStorageRange>::Endpoint
    {
      public:
        std::vector<lux::cxx::SharedBytes<>> volumes;
        lux::async::SubmitResult submit(lux::process::world_loading::ReadWorldStorageRange request, void *state,
            void (*complete)(void *, lux::async::OperationOutcome<lux::process::world_loading::ReadWorldStorageRange> &&) noexcept,
            lux::async::SubmitOptions) noexcept override
        {
            assert(request.volume < volumes.size());
            const auto &bytes = volumes[request.volume];
            assert(request.offset <= bytes.size() && request.size <= bytes.size() - request.offset);
            complete(state, bytes.subspan(request.offset, request.size));
            return {};
        }
    };
    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;
        lux::world::WorldPartitionData *result;
        bool *complete;
        auto get_env() const noexcept { return stdexec::env<>{}; }
        void set_value(lux::world::WorldPartitionData value) && noexcept
        {
            *result = std::move(value);
            *complete = true;
        }
        void set_error(lux::process::world_loading::WorldStorageRuntimeFailure) && noexcept { assert(false); }
        void set_stopped() && noexcept { assert(false); }
    };

    static SceneSourceChecks read(const std::filesystem::path &path)
    {
        using namespace lux;
        using namespace world;

        constexpr std::size_t limit = 256U * 1024U * 1024U;
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        assert(file.good());
        const auto size = file.tellg();
        assert(size > 0 && size <= limit);
        auto bytes = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(size));
        file.seekg(0);
        assert(file.read(reinterpret_cast<char *>(bytes->data()), size));
        auto package = asset::decodePak(cxx::SharedBytes<>::fromOwner(bytes, *bytes), 4096);
        assert(package);
        const auto entry = [&](std::string_view name) -> const asset::PakDecodedEntry &
        {
            const auto found = std::ranges::find(package->entries, name,
                                                 [](const auto &row)
                                                 {
                                                     return row.metadata.vpath;
                                                 });
            assert(found != package->entries.end());
            return *found;
        };
        const auto &encoded = entry("World");
        auto world = asset::TAssetSerDeser<WorldAsset>::decode(encoded.metadata.id, encoded.bytes, {limit, limit, 128});
        assert(world);
        const auto &description = (*world)->data();
        SceneSourceChecks result;
        result.bundle = description.bundleId();
        for (auto pair : {std::pair{"Scene", &result.scene}, std::pair{"Simulation", &result.simulation}})
        {
            const auto data = entry(pair.first).bytes.view();
            pair.second->assign(data.begin(), data.end());
        }
        auto reader = std::make_shared<VolumeReader>();
        for (std::uint32_t index{}; index < description.storageVolumes().size(); ++index)
        {
            reader->volumes.push_back(entry("Storage/" + std::to_string(index)).bytes);
        }
        namespace loading = lux::process::world_loading;
        auto source = loading::WorldStorageSource::create(
            std::shared_ptr<const WorldDescription>(*world, &description),
            lux::async::OperationPort<loading::ReadWorldStorageRange>{reader});
        assert(source);
        for (std::uint32_t index{}; index < description.partitionCount(); ++index)
        {
            WorldPartitionData partition;
            bool complete{};
            auto operation = stdexec::connect(loading::loadWorldPartition(*source, lux::partition::PartitionOrdinal{index}, limit, {}),
                Receiver{&partition, &complete});
            stdexec::start(operation);
            assert(complete);
            const auto ordinal = partition.partition();
            result.partitions.push_back(partition.id());
            for (std::size_t item{}; item < partition.objectCount(); ++item)
            {
                const auto object = partition.objectAt(item);
                assert(result.membership.emplace(object.id(), ordinal.value).second);
                for (std::size_t field{}; field < object.dataCount(); ++field)
                {
                    const auto schema = description.schemas()[object.schemaOrdinalAt(field)];
                    assert(schema.name != "lux.ecs.WorldTransform3D");
                    if (schema.name == "test.UnknownAuthorPayload")
                    {
                        const auto value = object.payloadAt(field);
                        assert(result.opaque.emplace(object.id(), Opaque{ordinal.value, object.schemaVersionAt(field),
                            partition.id(), {value.begin(), value.end()}}).second);
                    }
                }
            }
        }
        return result;
    }
    static void verify(const std::filesystem::path &root)
    {
        const auto before = read(root / "Main.before.luxscene");
        const auto after = read(root / "Main.luxscene");
        assert(before.partitions.size() == 2 && before.opaque.size() == 4);
        assert(before.opaque == after.opaque && before.partitions == after.partitions && before.bundle == after.bundle);
        assert(before.scene == after.scene && before.simulation == after.simulation);
        assert(after.membership.size() == before.membership.size() + 1);
        for (const auto &[object, partition] : before.membership)
        {
            assert(after.membership.at(object) == partition);
        }
        for (const auto &[object, partition] : after.membership)
        {
            if (!before.membership.contains(object))
            {
                assert(partition == 1);
            }
        }
        std::puts("PASS independent source preservation: two partition IDs, four opaque payloads/version/owners, all "
                  "original objects, new object in partition 1, unchanged Scene/Simulation descriptions, no derived "
                  "Transform schema");
    }
};
