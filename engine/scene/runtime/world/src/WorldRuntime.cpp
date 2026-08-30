#include <lux/engine/scene/WorldRuntime.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] WorldStorageRuntimeFailure sourceFailure() noexcept
        {
            return WorldStorageRuntimeFailure{EWorldStorageRuntimeError::INVALID_SOURCE};
        }

        [[nodiscard]] WorldMaterializeFailure materializeFailure(
            EWorldMaterializeError code,
            std::size_t object = 0U,
            std::size_t data = 0U
        ) noexcept
        {
            return WorldMaterializeFailure{code, {}, object, data};
        }
    } // namespace

    WorldStorageSource::WorldStorageSource(
        std::shared_ptr<const world::WorldDescription> world,
        lux::async::OperationPort<ReadWorldStorageRange> read_port
    ) noexcept
        : world_(std::move(world)), read_port_(std::move(read_port))
    {
    }

    lux::cxx::expected<WorldStorageSource, WorldStorageRuntimeFailure> WorldStorageSource::create(
        std::shared_ptr<const world::WorldDescription> world,
        lux::async::OperationPort<ReadWorldStorageRange> read_port
    ) noexcept
    {
        if (!world || !read_port)
            return lux::cxx::unexpected(sourceFailure());
        return WorldStorageSource(std::move(world), std::move(read_port));
    }

    WorldStorageSource::operator bool() const noexcept
    {
        return world_ != nullptr && static_cast<bool>(read_port_);
    }

    const world::WorldDescription& WorldStorageSource::world() const noexcept
    {
        return *world_;
    }

    const lux::async::OperationPort<ReadWorldStorageRange>& WorldStorageSource::readPort() const noexcept
    {
        return read_port_;
    }

    WorldMaterializer::WorldMaterializer(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components,
        std::vector<const simulation::ecs::ComponentSchema*> mappings
    ) noexcept
        : world_(std::move(world)), components_(std::move(components)), mappings_(std::move(mappings))
    {
    }

    lux::cxx::expected<WorldMaterializer, WorldMaterializeFailure> WorldMaterializer::create(
        std::shared_ptr<const world::WorldDescription> world,
        simulation::ecs::ComponentSchemaSet components
    ) noexcept
    {
        if (!world)
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }

        try
        {
            std::vector<const simulation::ecs::ComponentSchema*> mappings;
            mappings.reserve(world->schemas().size());
            for (const auto& schema : world->schemas())
            {
                mappings.push_back(
                    components.find(simulation::ecs::componentSchemaId(schema.name))
                );
            }
            return WorldMaterializer(std::move(world), std::move(components), std::move(mappings));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<simulation::ecs::Entity, WorldMaterializeFailure> WorldMaterializer::object(
        simulation::ecs::Registry& registry,
        world::WorldPartitionObjectView object
    ) const noexcept
    {
        if (!object)
            return lux::cxx::unexpected(materializeFailure(EWorldMaterializeError::INVALID_OBJECT));
        if (object.bundle() != world_->bundleId() || object.generation() != world_->generation())
        {
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }

        simulation::ecs::Entity entity{simulation::ecs::NullEntity};
        try
        {
            entity = registry.create();
            for (std::size_t data{}; data < object.dataCount(); ++data)
            {
                const std::uint32_t ordinal = object.schemaOrdinalAt(data);
                if (ordinal >= mappings_.size())
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA, 0U, data)
                    );
                }
                const auto* schema = mappings_[ordinal];
                if (schema == nullptr)
                    continue;
                if (schema->decode_emplace == nullptr)
                {
                    registry.destroy(entity);
                    return lux::cxx::unexpected(
                        materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0U, data)
                    );
                }
                auto decoded = schema->decode_emplace(
                    registry,
                    entity,
                    object.schemaVersionAt(data),
                    object.payloadAt(data)
                );
                if (!decoded)
                {
                    registry.destroy(entity);
                    WorldMaterializeFailure failure =
                        materializeFailure(EWorldMaterializeError::COMPONENT_DECODE_FAILURE, 0U, data);
                    failure.component = decoded.error();
                    return lux::cxx::unexpected(std::move(failure));
                }
            }
            return entity;
        }
        catch (const std::bad_alloc&)
        {
            if (registry.valid(entity))
                registry.destroy(entity);
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }

    lux::cxx::expected<void, WorldMaterializeFailure> WorldMaterializer::partition(
        simulation::ecs::Registry& registry,
        const world::WorldPartitionData& data,
        std::vector<simulation::ecs::Entity>* created
    ) const noexcept
    {
        if (data.bundle() != world_->bundleId() || data.generation() != world_->generation())
        {
            if (created != nullptr)
                created->clear();
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::INVALID_WORLD_SCHEMA)
            );
        }
        std::vector<simulation::ecs::Entity> local;
        try
        {
            local.reserve(data.objectCount());
            for (std::size_t object_index{}; object_index < data.objectCount(); ++object_index)
            {
                auto entity = object(registry, data.objectAt(object_index));
                if (!entity)
                {
                    for (const auto value : local)
                    {
                        if (registry.valid(value))
                            registry.destroy(value);
                    }
                    if (created != nullptr)
                        created->clear();
                    auto failure = entity.error();
                    failure.object = object_index;
                    return lux::cxx::unexpected(std::move(failure));
                }
                local.push_back(*entity);
            }
            if (created != nullptr)
                *created = std::move(local);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            for (const auto value : local)
            {
                if (registry.valid(value))
                    registry.destroy(value);
            }
            if (created != nullptr)
                created->clear();
            return lux::cxx::unexpected(
                materializeFailure(EWorldMaterializeError::ALLOCATION_FAILURE)
            );
        }
    }

    struct detail::WorldPartitionLoadMachine::Impl final
    {
        using Outcome = lux::async::OperationOutcome<ReadWorldStorageRange>;

        enum class EStage : std::uint8_t
        {
            HEADER,
            DESCRIPTOR,
            PAYLOAD,
        };

        Impl(
            WorldStorageSource source_value,
            partition::PartitionOrdinal partition_value,
            std::size_t max_bytes_value,
            std::stop_token stop_value,
            void* receiver_value,
            void (*value_fn)(void*, world::WorldPartitionData&&) noexcept,
            void (*error_fn)(void*, WorldStorageRuntimeFailure) noexcept,
            void (*stopped_fn)(void*) noexcept
        ) noexcept
            : source(std::move(source_value)), partition(partition_value), max_bytes(max_bytes_value), stop(stop_value),
              receiver(receiver_value), set_value(value_fn), set_error(error_fn), set_stopped(stopped_fn)
        {
        }

        [[nodiscard]] WorldStorageRuntimeFailure mapFailure(
            world::detail::WorldStorageCodecFailure failure
        ) const noexcept
        {
            using Input = world::detail::EWorldStorageCodecError;
            EWorldStorageRuntimeError code{EWorldStorageRuntimeError::DECODE_FAILURE};
            switch (failure.code)
            {
            case Input::BUNDLE_MISMATCH:
            case Input::GENERATION_MISMATCH:
            case Input::VOLUME_MISMATCH:
                code = EWorldStorageRuntimeError::BUNDLE_MISMATCH;
                break;
            case Input::RANGE_OVERFLOW:
                code = EWorldStorageRuntimeError::RANGE_OVERFLOW;
                break;
            case Input::SIZE_LIMIT:
                code = EWorldStorageRuntimeError::LIMIT_EXCEEDED;
                break;
            case Input::DIGEST_MISMATCH:
                code = EWorldStorageRuntimeError::DIGEST_MISMATCH;
                break;
            case Input::UNSUPPORTED_CODEC:
                code = EWorldStorageRuntimeError::DECOMPRESSION_FAILURE;
                break;
            case Input::ALLOCATION_FAILURE:
                code = EWorldStorageRuntimeError::ALLOCATION_FAILURE;
                break;
            case Input::CORRUPT_DESCRIPTOR:
            case Input::INVALID_MAGIC:
            case Input::UNSUPPORTED_VERSION:
                code = EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR;
                break;
            default:
                break;
            }
            return WorldStorageRuntimeFailure{code, failure.volume, failure.offset};
        }

        void finishError(WorldStorageRuntimeFailure failure) noexcept
        {
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_error(receiver, failure);
        }

        void finishStopped() noexcept
        {
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_stopped(receiver);
        }

        void finishCodecFailure(world::detail::WorldStorageCodecFailure failure) noexcept
        {
            if (failure.code == world::detail::EWorldStorageCodecError::CANCELLED)
                finishStopped();
            else
                finishError(mapFailure(failure));
        }

        void submit(std::uint32_t volume, std::uint64_t offset, std::uint64_t size) noexcept
        {
            if (stop.stop_requested())
            {
                finishStopped();
                return;
            }
            if (size > std::numeric_limits<std::size_t>::max() ||
                static_cast<std::size_t>(size) > max_bytes)
            {
                finishError({EWorldStorageRuntimeError::LIMIT_EXCEEDED, volume, offset});
                return;
            }
            const std::size_t accounted_bytes = static_cast<std::size_t>(size);
            callback_seen.store(false, std::memory_order_release);
            const auto submitted = source.readPort().submit(
                ReadWorldStorageRange{volume, offset, size},
                this,
                [](void* state, Outcome&& outcome) noexcept {
                    auto& self = *static_cast<Impl*>(state);
                    self.callback_seen.store(true, std::memory_order_release);
                    self.complete(std::move(outcome));
                },
                lux::async::SubmitOptions{.accounted_bytes = accounted_bytes}
            );
            if (!submitted && !callback_seen.load(std::memory_order_acquire))
            {
                finishError({EWorldStorageRuntimeError::IO_FAILURE, volume, offset});
            }
        }

        void beginChunk(world::WorldChunkReference reference, bool table) noexcept
        {
            if (reference.volume >= source.world().storageVolumes().size())
            {
                finishError({EWorldStorageRuntimeError::INVALID_VOLUME, reference.volume});
                return;
            }
            current = reference;
            reading_table = table;
            stage = EStage::HEADER;
            submit(reference.volume, 0U, world::detail::kWorldStorageVolumeHeaderWireSize);
        }

        void complete(Outcome&& outcome) noexcept
        {
            if (finished.load(std::memory_order_acquire))
                return;
            if (stop.stop_requested())
            {
                finishStopped();
                return;
            }
            if (!outcome)
            {
                if (outcome.error().isRuntime())
                    finishError({EWorldStorageRuntimeError::IO_FAILURE, current.volume});
                else
                    finishError(outcome.error().domainError());
                return;
            }

            const auto bytes = outcome->view();
            const auto& volume_description = source.world().storageVolumes()[current.volume];
            if (stage == EStage::HEADER)
            {
                auto decoded = world::detail::decodeWorldStorageVolumeHeader(
                    bytes,
                    source.world().bundleId(),
                    source.world().generation(),
                    current.volume,
                    volume_description
                );
                if (!decoded)
                {
                    finishCodecFailure(decoded.error());
                    return;
                }
                header = *decoded;
                if (current.chunk >= header.chunk_count)
                {
                    finishError({EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR, current.volume});
                    return;
                }
                stage = EStage::DESCRIPTOR;
                submit(
                    current.volume,
                    header.descriptor_offset +
                        static_cast<std::uint64_t>(current.chunk) * header.descriptor_stride,
                    header.descriptor_stride
                );
                return;
            }
            if (stage == EStage::DESCRIPTOR)
            {
                auto decoded = world::detail::decodeWorldStorageChunkDescriptor(bytes, header, current.chunk);
                if (!decoded)
                {
                    finishError(mapFailure(decoded.error()));
                    return;
                }
                descriptor = *decoded;
                if (descriptor.stored_size > max_bytes || descriptor.decoded_size > max_bytes)
                {
                    finishError({EWorldStorageRuntimeError::LIMIT_EXCEEDED, current.volume, descriptor.offset});
                    return;
                }
                stage = EStage::PAYLOAD;
                submit(current.volume, descriptor.offset, descriptor.stored_size);
                return;
            }

            auto decoded_payload = world::detail::decodeWorldStorageChunkPayload(
                bytes,
                descriptor,
                max_bytes,
                stop
            );
            if (!decoded_payload)
            {
                finishCodecFailure(decoded_payload.error());
                return;
            }
            if (reading_table)
            {
                if (descriptor.kind != world::detail::EWorldStorageChunkKind::PARTITION_TABLE_PAGE)
                {
                    finishError({EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR, current.volume});
                    return;
                }
                auto page = world::detail::decodeWorldPartitionTablePage(
                    *decoded_payload,
                    table_page->first,
                    table_page->count,
                    max_bytes,
                    stop
                );
                if (!page)
                {
                    finishCodecFailure(page.error());
                    return;
                }
                const auto* record = page->find(partition);
                if (record == nullptr)
                {
                    finishError({EWorldStorageRuntimeError::INVALID_PARTITION});
                    return;
                }
                partition_page = std::move(*page);
                record = partition_page.find(partition);
                if (record == nullptr || record->extent_count == 0U)
                {
                    finishError({EWorldStorageRuntimeError::DECODE_FAILURE});
                    return;
                }
                first_partition_extent = record->first_extent;
                partition_extent_count = record->extent_count;
                partition_extent_index = 0U;
                chunk_in_extent = 0U;
                if (!beginNextPartitionChunk())
                    finishError({EWorldStorageRuntimeError::DECODE_FAILURE});
                return;
            }

            if (descriptor.kind != world::detail::EWorldStorageChunkKind::WORLD_PARTITION_DATA)
            {
                finishError({EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR, current.volume});
                return;
            }
            try
            {
                if (partition_bytes.size() > max_bytes ||
                    decoded_payload->size() > max_bytes - partition_bytes.size())
                {
                    finishError({EWorldStorageRuntimeError::LIMIT_EXCEEDED, current.volume, descriptor.offset});
                    return;
                }
                partition_bytes.insert(
                    partition_bytes.end(),
                    decoded_payload->begin(),
                    decoded_payload->end()
                );
            }
            catch (const std::bad_alloc&)
            {
                finishError({EWorldStorageRuntimeError::ALLOCATION_FAILURE});
                return;
            }
            if (beginNextPartitionChunk())
            {
                return;
            }

            finishPartition();
        }

        [[nodiscard]] bool beginNextPartitionChunk() noexcept
        {
            while (partition_extent_index < partition_extent_count)
            {
                const std::size_t extent_ordinal = static_cast<std::size_t>(first_partition_extent) +
                    partition_extent_index;
                if (extent_ordinal >= partition_page.extents.size())
                {
                    finishError({EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR});
                    return true;
                }
                const auto& extent = partition_page.extents[extent_ordinal];
                if (chunk_in_extent == 0U)
                {
                    if (extent.volume >= source.world().storageVolumes().size())
                    {
                        finishError({EWorldStorageRuntimeError::INVALID_VOLUME, extent.volume});
                        return true;
                    }
                    const auto volume_chunks = source.world().storageVolumes()[extent.volume].chunk_count;
                    const bool invalid_first = extent.first_chunk > volume_chunks;
                    const bool invalid_count = extent.chunk_count == 0U ||
                        (!invalid_first && extent.chunk_count > volume_chunks - extent.first_chunk);
                    if (invalid_first || invalid_count)
                    {
                        finishError({EWorldStorageRuntimeError::CORRUPT_DESCRIPTOR, extent.volume});
                        return true;
                    }
                }
                if (chunk_in_extent < extent.chunk_count)
                {
                    const world::WorldChunkReference reference{
                        extent.volume,
                        extent.first_chunk + chunk_in_extent
                    };
                    ++chunk_in_extent;
                    beginChunk(reference, false);
                    return true;
                }
                ++partition_extent_index;
                chunk_in_extent = 0U;
            }
            return false;
        }

        void finishPartition() noexcept
        {
            auto data = world::detail::decodeWorldPartitionData(
                partition_bytes,
                source.world().bundleId(),
                source.world().generation(),
                partition,
                static_cast<std::uint32_t>(source.world().schemas().size()),
                max_bytes,
                stop
            );
            if (!data)
            {
                finishCodecFailure(data.error());
                return;
            }
            if (!finished.exchange(true, std::memory_order_acq_rel))
                set_value(receiver, std::move(*data));
        }

        void start() noexcept
        {
            if (!source || max_bytes == 0U || partition.value >= source.world().partitionCount())
            {
                finishError({max_bytes == 0U ? EWorldStorageRuntimeError::LIMIT_EXCEEDED
                                             : EWorldStorageRuntimeError::INVALID_PARTITION});
                return;
            }
            table_page = source.world().partitionTable().findPage(partition);
            if (table_page == nullptr)
            {
                finishError({EWorldStorageRuntimeError::INVALID_PARTITION});
                return;
            }
            beginChunk(table_page->chunk, true);
        }

        WorldStorageSource source;
        partition::PartitionOrdinal partition;
        std::size_t max_bytes{};
        std::stop_token stop;
        void* receiver{};
        void (*set_value)(void*, world::WorldPartitionData&&) noexcept{};
        void (*set_error)(void*, WorldStorageRuntimeFailure) noexcept{};
        void (*set_stopped)(void*) noexcept{};
        std::atomic_bool finished{};
        std::atomic_bool callback_seen{};
        EStage stage{EStage::HEADER};
        bool reading_table{};
        world::WorldChunkReference current;
        const world::WorldPartitionTablePageDescription* table_page{};
        world::detail::WorldStorageVolumeHeader header;
        world::detail::WorldStorageChunkDescriptor descriptor;
        world::detail::WorldPartitionTablePage partition_page;
        std::uint32_t first_partition_extent{};
        std::uint32_t partition_extent_count{};
        std::uint32_t partition_extent_index{};
        std::uint32_t chunk_in_extent{};
        std::vector<std::byte> partition_bytes;
    };

    detail::WorldPartitionLoadMachine::WorldPartitionLoadMachine(
        WorldStorageSource source,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop,
        void* receiver,
        void (*set_value)(void*, world::WorldPartitionData&&) noexcept,
        void (*set_error)(void*, WorldStorageRuntimeFailure) noexcept,
        void (*set_stopped)(void*) noexcept
    )
        : impl_(std::make_unique<Impl>(
              std::move(source),
              partition,
              max_bytes,
              stop,
              receiver,
              set_value,
              set_error,
              set_stopped
          ))
    {
    }

    detail::WorldPartitionLoadMachine::~WorldPartitionLoadMachine() = default;

    void detail::WorldPartitionLoadMachine::start() noexcept
    {
        impl_->start();
    }
} // namespace lux::scene
