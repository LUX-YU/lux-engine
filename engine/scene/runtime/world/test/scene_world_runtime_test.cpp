#include <lux/engine/process/world_loading/WorldPartitionLoadSender.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/serialization/external_support/Eigen.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexec/execution.hpp>
#include <utility>
#include <vector>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    class Endpoint final
        : public lux::async::OperationPort<lux::process::world_loading::ReadWorldStorageRange>::Endpoint
    {
    public:
        [[nodiscard]] lux::async::SubmitResult submit(
            lux::process::world_loading::ReadWorldStorageRange,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions
        ) noexcept override
        {
            complete(
                state,
                lux::cxx::unexpected(
                    lux::async::OperationFailure<lux::process::world_loading::WorldStorageRuntimeFailure>::domain(
                        {lux::process::world_loading::EWorldStorageRuntimeError::IO_FAILURE}
                    )
                )
            );
            return {};
        }
    };

    class MemoryEndpoint final
        : public lux::async::OperationPort<lux::process::world_loading::ReadWorldStorageRange>::Endpoint
    {
    public:
        explicit MemoryEndpoint(std::vector<std::byte> bytes) : bytes_(std::move(bytes))
        {
        }

        [[nodiscard]] lux::async::SubmitResult submit(
            lux::process::world_loading::ReadWorldStorageRange operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override
        {
            accounting_ok = accounting_ok && operation.size == options.accounted_bytes;
            ++submits;
            if (operation.volume != 0U || operation.offset > bytes_.size() ||
                operation.size > bytes_.size() - operation.offset)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::process::world_loading::WorldStorageRuntimeFailure>::domain(
                            {lux::process::world_loading::EWorldStorageRuntimeError::RANGE_OVERFLOW}
                        )
                    )
                );
                return {};
            }
            try
            {
                complete(
                    state,
                    Outcome{lux::cxx::SharedBytes<>::copyOf(
                        std::span<const std::byte>(bytes_).subspan(
                            static_cast<std::size_t>(operation.offset),
                            static_cast<std::size_t>(operation.size)
                        )
                    )}
                );
            }
            catch (...)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::process::world_loading::WorldStorageRuntimeFailure>::domain(
                            {lux::process::world_loading::EWorldStorageRuntimeError::ALLOCATION_FAILURE}
                        )
                    )
                );
            }
            return {};
        }

    private:
        std::vector<std::byte> bytes_;

    public:
        std::size_t submits{};
        bool accounting_ok{true};
    };

    struct LoadReceiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(lux::world::WorldPartitionData value) && noexcept
        {
            result->emplace(std::move(value));
        }

        void set_error(lux::process::world_loading::WorldStorageRuntimeFailure value) && noexcept
        {
            error->emplace(value);
        }

        void set_stopped() && noexcept
        {
            *stopped = true;
        }

        [[nodiscard]] stdexec::empty_env get_env() const noexcept
        {
            return {};
        }

        std::optional<lux::world::WorldPartitionData>* result{};
        std::optional<lux::process::world_loading::WorldStorageRuntimeFailure>* error{};
        bool* stopped{};
    };
}

int main()
{
    using namespace lux;
    using namespace lux::simulation::ecs;
    using namespace lux::world;
    using namespace lux::world::detail;

    WorldDescriptionBuilder world_builder;
    assert(world_builder.setIdentity(id<WorldBundleId>(1U), id<WorldBundleGeneration>(2U), "materialize"));
    assert(world_builder.addSchema(worldDataSchemaId("lux.ecs.Transform3D")));
    assert(world_builder.addSchema(worldDataSchemaId("test.unknown")));
    assert(world_builder.setPartitioner({worldPartitionerId("test.none"), 1U}, 1U));
    assert(world_builder.addStorageVolume({"materialize.wvol0", 1U, 1U, 1024U}));
    assert(world_builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 1U, {0U, 0U}}));
    auto world_value = std::move(world_builder).build();
    assert(world_value);
    auto world = std::make_shared<WorldDescription>(std::move(*world_value));

    assert(!process::world_loading::WorldStorageSource::create({}, {}));
    auto source = process::world_loading::WorldStorageSource::create(
        world,
        lux::async::OperationPort<process::world_loading::ReadWorldStorageRange>{std::make_shared<Endpoint>()}
    );
    assert(source);
    assert(source->world().bundleId() == world->bundleId());

    auto components = ComponentSchemaSet::build(
        std::vector<ComponentSchema>(
            transformComponentSchemas().begin(),
            transformComponentSchemas().end()
        )
    );
    assert(components);
    auto materializer = scene::WorldMaterializer::create(world, *components);
    assert(materializer);

    std::uint32_t transform_ordinal{};
    std::uint32_t unknown_ordinal{};
    for (std::uint32_t ordinal{}; ordinal < world->schemas().size(); ++ordinal)
    {
        if (world->schemas()[ordinal].name == "lux.ecs.Transform3D")
            transform_ordinal = ordinal;
        if (world->schemas()[ordinal].name == "test.unknown")
            unknown_ordinal = ordinal;
    }

    Transform3D transform{
        Eigen::Vector3d{1000.25, 2.0, 3.0},
        Eigen::Quaterniond::Identity(),
        Eigen::Vector3d::Ones()
    };
    std::vector<std::byte> payload;
    lux::serialization::BinaryWriter writer(payload);
    const lux::serialization::SerializationBudget budget{payload.max_size(), payload.max_size(), 64U};
    assert(lux::serialization::write(writer, transform, budget));

    const std::array first_data{WorldEncodedDataRecord{transform_ordinal, 1U, payload}};
    const std::array unknown_data{
        WorldEncodedDataRecord{unknown_ordinal, 1U, std::span<const std::byte>{}}
    };
    const std::array objects{
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(1U), first_data},
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(2U), unknown_data}
    };
    auto wire = encodeWorldPartitionData(lux::partition::PartitionOrdinal{0U}, objects);
    assert(wire);
    auto partition = decodeWorldPartitionData(
        *wire,
        world->bundleId(),
        world->generation(),
        lux::partition::PartitionOrdinal{0U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        {}
    );
    assert(partition);

    Registry registry;
    std::vector<Entity> created;
    assert(materializer->partition(registry, *partition, &created));
    assert(created.size() == 2U);
    assert(registry.get<const Transform3D>(created[0]).translation.isApprox(transform.translation));
    assert(!registry.all_of<Transform3D>(created[1]));

    WorldDescriptionBuilder other_world_builder;
    assert(other_world_builder.setIdentity(
        world->bundleId(),
        id<WorldBundleGeneration>(99U),
        "other-generation"
    ));
    assert(other_world_builder.addSchema(worldDataSchemaId("lux.ecs.Transform3D")));
    assert(other_world_builder.addSchema(worldDataSchemaId("test.unknown")));
    assert(other_world_builder.setPartitioner({worldPartitionerId("test.none"), 1U}, 1U));
    assert(other_world_builder.addStorageVolume({"other.wvol0", 1U, 1U, 1024U}));
    assert(other_world_builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 1U, {0U, 0U}}));
    auto other_world_value = std::move(other_world_builder).build();
    assert(other_world_value);
    auto other_materializer = scene::WorldMaterializer::create(
        std::make_shared<WorldDescription>(std::move(*other_world_value)),
        *components
    );
    assert(other_materializer);
    const std::size_t identity_mismatch_before = registry.view<const Transform3D>().size();
    auto identity_mismatch = other_materializer->partition(registry, *partition);
    assert(!identity_mismatch);
    assert(identity_mismatch.error().code == scene::EWorldMaterializeError::INVALID_WORLD_SCHEMA);
    assert(registry.view<const Transform3D>().size() == identity_mismatch_before);

    auto malformed_payload = payload;
    malformed_payload.pop_back();
    const std::array malformed_data{
        WorldEncodedDataRecord{transform_ordinal, 1U, malformed_payload}
    };
    const std::array rollback_objects{
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(3U), first_data},
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(4U), malformed_data}
    };
    auto rollback_wire = encodeWorldPartitionData(lux::partition::PartitionOrdinal{0U}, rollback_objects);
    assert(rollback_wire);
    auto rollback_partition = decodeWorldPartitionData(
        *rollback_wire,
        world->bundleId(),
        world->generation(),
        lux::partition::PartitionOrdinal{0U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        {}
    );
    assert(rollback_partition);

    const std::size_t before = registry.view<const Transform3D>().size();
    created = {registry.create()};
    auto rolled_back = materializer->partition(registry, *rollback_partition, &created);
    assert(!rolled_back);
    assert(created.empty());
    assert(registry.view<const Transform3D>().size() == before);

    const std::array table_records{
        WorldPartitionRecord{id<WorldPartitionId>(8U), 0U, 1U}
    };
    const std::array table_extents{WorldPartitionExtent{0U, 1U, 1U}};
    auto table_wire = encodeWorldPartitionTablePage(
        lux::partition::PartitionOrdinal{0U},
        table_records,
        table_extents
    );
    assert(table_wire);
    const std::array chunks{
        WorldStorageChunkInput{
            EWorldStorageChunkKind::PARTITION_TABLE_PAGE,
            EWorldStorageCodec::NONE,
            *table_wire
        },
        WorldStorageChunkInput{
            EWorldStorageChunkKind::WORLD_PARTITION_DATA,
            EWorldStorageCodec::NONE,
            *wire
        }
    };
    auto volume = encodeWorldStorageVolume(
        id<WorldBundleId>(20U),
        id<WorldBundleGeneration>(21U),
        0U,
        chunks
    );
    assert(volume);

    WorldDescriptionBuilder load_world_builder;
    assert(load_world_builder.setIdentity(
        id<WorldBundleId>(20U),
        id<WorldBundleGeneration>(21U),
        "load-world"
    ));
    assert(load_world_builder.addSchema(worldDataSchemaId("lux.ecs.Transform3D")));
    assert(load_world_builder.addSchema(worldDataSchemaId("test.unknown")));
    assert(load_world_builder.setPartitioner({worldPartitionerId("test.load"), 1U}, 1U));
    assert(load_world_builder.addStorageVolume({"load.wvol0", 1U, 2U, volume->size()}));
    assert(load_world_builder.addPartitionTablePage({lux::partition::PartitionOrdinal{0U}, 1U, {0U, 0U}}));
    auto load_world_value = std::move(load_world_builder).build();
    assert(load_world_value);
    auto load_world = std::make_shared<WorldDescription>(std::move(*load_world_value));
    auto memory_endpoint = std::make_shared<MemoryEndpoint>(*volume);
    auto load_source = process::world_loading::WorldStorageSource::create(
        load_world,
        lux::async::OperationPort<process::world_loading::ReadWorldStorageRange>{memory_endpoint}
    );
    assert(load_source);

    std::optional<WorldPartitionData> limited_value;
    std::optional<process::world_loading::WorldStorageRuntimeFailure> limited_error;
    bool limited_stopped{};
    auto limited_state = stdexec::connect(
        process::world_loading::loadWorldPartition(*load_source, lux::partition::PartitionOrdinal{0U}, 1U, {}),
        LoadReceiver{&limited_value, &limited_error, &limited_stopped}
    );
    stdexec::start(limited_state);
    assert(!limited_value);
    assert(limited_error);
    assert(limited_error->code == process::world_loading::EWorldStorageRuntimeError::LIMIT_EXCEEDED);

    std::optional<WorldPartitionData> loaded;
    std::optional<process::world_loading::WorldStorageRuntimeFailure> load_error;
    bool stopped{};
    auto load_state = stdexec::connect(
        process::world_loading::loadWorldPartition(
            *load_source,
            lux::partition::PartitionOrdinal{0U},
            volume->size(),
            {}
        ),
        LoadReceiver{&loaded, &load_error, &stopped}
    );
    stdexec::start(load_state);
    assert(loaded);
    assert(!load_error);
    assert(!stopped);
    assert(loaded->objectCount() == 2U);
    assert(memory_endpoint->submits > 0U);
    assert(memory_endpoint->accounting_ok);

    std::stop_source cancelled;
    cancelled.request_stop();
    loaded.reset();
    stopped = false;
    auto stopped_state = stdexec::connect(
        process::world_loading::loadWorldPartition(
            *load_source,
            lux::partition::PartitionOrdinal{0U},
            volume->size(),
            cancelled.get_token()
        ),
        LoadReceiver{&loaded, &load_error, &stopped}
    );
    stdexec::start(stopped_state);
    assert(!loaded);
    assert(stopped);

    return 0;
}
