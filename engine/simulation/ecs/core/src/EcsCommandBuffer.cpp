#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

#include <algorithm>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lux::simulation::ecs
{
    struct EcsCommandBuffer::Impl final
    {
        enum class EKind : std::uint8_t
        {
            CREATE,
            DESTROY,
            EMPLACE,
            REMOVE,
        };

        struct Record final
        {
            EKind kind{EKind::CREATE};
            Entity entity{NullEntity};
            DeferredEntity deferred;
            bool uses_deferred{};
            void* payload{};
            EcsCommandWriter::RawCommandVTable table;
            EcsCommandWriter::RemoveFn remove{};

            Record() noexcept = default;
            Record(const Record&) = delete;
            Record& operator=(const Record&) = delete;
            Record(Record&& other) noexcept
                : kind(other.kind),
                  entity(other.entity),
                  deferred(other.deferred),
                  uses_deferred(other.uses_deferred),
                  payload(std::exchange(other.payload, nullptr)),
                  table(other.table),
                  remove(other.remove)
            {
            }
            Record& operator=(Record&& other) noexcept
            {
                if (this == std::addressof(other))
                    return *this;
                clear();
                kind = other.kind;
                entity = other.entity;
                deferred = other.deferred;
                uses_deferred = other.uses_deferred;
                payload = std::exchange(other.payload, nullptr);
                table = other.table;
                remove = other.remove;
                return *this;
            }
            ~Record() noexcept { clear(); }

            void clear() noexcept
            {
                if (payload != nullptr)
                    table.destroy(payload);
                payload = nullptr;
            }
        };

        struct Arena final
        {
            std::vector<std::byte> storage;
            std::size_t used{};
            std::size_t allocation_events{};

            void prepare(std::size_t capacity)
            {
                if (storage.capacity() < capacity)
                    ++allocation_events;
                storage.resize(capacity);
                used = 0U;
            }

            [[nodiscard]] void* allocate(
                std::size_t size,
                std::size_t alignment
            ) noexcept
            {
                if (size == 0U)
                    return storage.data();
                const auto address = reinterpret_cast<std::uintptr_t>(
                    storage.data() + used
                );
                const auto padding = static_cast<std::size_t>(
                    (alignment - address % alignment) % alignment
                );
                if (padding > storage.size() - used ||
                    size > storage.size() - used - padding)
                {
                    return nullptr;
                }
                used += padding;
                auto* result = storage.data() + used;
                used += size;
                return result;
            }

            void reset() noexcept { used = 0U; }
        };

        struct Producer final
        {
            Producer() = default;
            ~Producer() noexcept { records.clear(); }
            Producer(const Producer&) = delete;
            Producer& operator=(const Producer&) = delete;
            Producer(Producer&&) noexcept = default;
            Producer& operator=(Producer&&) noexcept = default;

            std::vector<Record> records;
            Arena arena;
            std::vector<Entity> resolved;
            std::size_t max_commands{};
            std::uint32_t create_count{};
            bool active{};
            std::size_t record_allocation_events{};

            void prepare(EcsCommandProducerCapacity capacity)
            {
                records.clear();
                resolved.clear();
                if (records.capacity() < capacity.max_commands)
                    ++record_allocation_events;
                records.reserve(capacity.max_commands);
                resolved.reserve(capacity.max_commands);
                arena.prepare(capacity.max_payload_bytes);
                max_commands = capacity.max_commands;
                create_count = 0U;
                active = false;
            }

            void clearPending() noexcept
            {
                records.clear();
                arena.reset();
                create_count = 0U;
            }
        };

        std::vector<Producer> producers;
        std::uint32_t generation{1U};
        bool failed{};
        EcsCommandFailure failure;
        std::size_t discarded{};

        void nextGeneration() noexcept
        {
            ++generation;
            if (generation == 0U)
                ++generation;
        }
    };

    EcsCommandWriter::EcsCommandWriter(
        EcsCommandBuffer& owner,
        std::uint32_t producer,
        std::uint32_t generation
    ) noexcept
        : owner_(&owner), producer_(producer), generation_(generation)
    {
    }

    EcsCommandWriter::~EcsCommandWriter() noexcept { release(); }

    EcsCommandWriter::EcsCommandWriter(EcsCommandWriter&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)),
          producer_(other.producer_),
          generation_(other.generation_)
    {
    }

    EcsCommandWriter::operator bool() const noexcept
    {
        return owner_ != nullptr && !owner_->failed();
    }

    DeferredEntity EcsCommandWriter::create() noexcept
    {
        return owner_ != nullptr
            ? owner_->recordCreate(producer_, generation_)
            : DeferredEntity{};
    }

    void EcsCommandWriter::destroy(Entity entity) noexcept
    {
        if (owner_ != nullptr)
            owner_->recordDestroy(producer_, generation_, entity);
    }

    bool EcsCommandWriter::recordPayload(
        Entity entity,
        DeferredEntity deferred,
        bool uses_deferred,
        const RawCommandVTable& table,
        void* source
    ) noexcept
    {
        return owner_ != nullptr && owner_->recordPayload(
            producer_,
            generation_,
            entity,
            deferred,
            uses_deferred,
            table,
            source
        );
    }

    bool EcsCommandWriter::recordRemove(
        Entity entity,
        RemoveFn remove
    ) noexcept
    {
        return owner_ != nullptr && owner_->recordRemove(
            producer_,
            generation_,
            entity,
            remove
        );
    }

    void EcsCommandWriter::fail(EEcsCommandError error) noexcept
    {
        if (owner_ != nullptr)
            owner_->fail(producer_, generation_, error);
    }

    void EcsCommandWriter::release() noexcept
    {
        if (owner_ != nullptr)
            owner_->end(producer_, generation_);
        owner_ = nullptr;
    }

    EcsCommandBuffer::EcsCommandBuffer()
        : impl_(std::make_unique<Impl>())
    {
    }
    EcsCommandBuffer::~EcsCommandBuffer() = default;
    EcsCommandBuffer::EcsCommandBuffer(EcsCommandBuffer&&) noexcept = default;
    EcsCommandBuffer& EcsCommandBuffer::operator=(EcsCommandBuffer&&) noexcept =
        default;

    lux::cxx::expected<void, EcsCommandFailure> EcsCommandBuffer::prepare(
        std::span<const EcsCommandProducerCapacity> capacities
    ) noexcept
    {
        for (std::size_t index{}; index < impl_->producers.size(); ++index)
        {
            if (impl_->producers[index].active)
            {
                return lux::cxx::unexpected(EcsCommandFailure{
                    EEcsCommandError::ACTIVE_WRITER,
                    index});
            }
        }
        try
        {
            impl_->nextGeneration();
            impl_->producers.resize(capacities.size());
            for (std::size_t index{}; index < capacities.size(); ++index)
                impl_->producers[index].prepare(capacities[index]);
            impl_->failed = false;
            impl_->failure = {};
            return {};
        }
        catch (const std::bad_alloc&)
        {
            impl_->failed = true;
            impl_->failure = {EEcsCommandError::ALLOCATION_FAILURE};
            return lux::cxx::unexpected(impl_->failure);
        }
        catch (const std::length_error&)
        {
            impl_->failed = true;
            impl_->failure = {EEcsCommandError::ALLOCATION_FAILURE};
            return lux::cxx::unexpected(impl_->failure);
        }
    }

    void EcsCommandBuffer::reset() noexcept { discardPending(); }

    lux::cxx::expected<EcsCommandWriter, EcsCommandFailure>
    EcsCommandBuffer::begin(std::size_t producer) noexcept
    {
        if (producer >= impl_->producers.size())
        {
            return lux::cxx::unexpected(EcsCommandFailure{
                EEcsCommandError::INVALID_PRODUCER,
                producer});
        }
        if (impl_->failed)
            return lux::cxx::unexpected(impl_->failure);
        auto& target = impl_->producers[producer];
        if (target.active)
        {
            return lux::cxx::unexpected(EcsCommandFailure{
                EEcsCommandError::ACTIVE_WRITER,
                producer});
        }
        target.active = true;
        return EcsCommandWriter(
            *this,
            static_cast<std::uint32_t>(producer),
            impl_->generation
        );
    }

    std::optional<Entity> EcsCommandBuffer::resolve(
        DeferredEntity entity
    ) const noexcept
    {
        if (!entity.valid() || entity.generation != impl_->generation ||
            entity.producer >= impl_->producers.size())
        {
            return std::nullopt;
        }
        const auto& resolved = impl_->producers[entity.producer].resolved;
        return entity.ordinal < resolved.size()
            ? std::optional<Entity>{resolved[entity.ordinal]}
            : std::nullopt;
    }

    bool EcsCommandBuffer::failed() const noexcept { return impl_->failed; }

    std::size_t EcsCommandBuffer::allocationEvents() const noexcept
    {
        std::size_t result{};
        for (const auto& producer : impl_->producers)
        {
            result += producer.record_allocation_events;
            result += producer.arena.allocation_events;
        }
        return result;
    }

    std::size_t EcsCommandBuffer::discarded() const noexcept
    {
        return impl_->discarded;
    }

    void EcsCommandBuffer::discardPending() noexcept
    {
        impl_->nextGeneration();
        for (auto& producer : impl_->producers)
        {
            impl_->discarded += producer.records.size();
            producer.clearPending();
            producer.resolved.clear();
            producer.active = false;
        }
        impl_->failed = false;
        impl_->failure = {};
    }

    DeferredEntity EcsCommandBuffer::recordCreate(
        std::uint32_t producer,
        std::uint32_t generation
    ) noexcept
    {
        if (producer >= impl_->producers.size() ||
            generation != impl_->generation ||
            !impl_->producers[producer].active)
        {
            fail(producer, generation, EEcsCommandError::STALE_WRITER);
            return {};
        }
        auto& target = impl_->producers[producer];
        if (impl_->failed || target.records.size() >= target.max_commands)
        {
            fail(producer, generation, EEcsCommandError::CAPACITY_EXCEEDED);
            return {};
        }
        const DeferredEntity deferred{
            producer,
            target.create_count,
            generation};
        Impl::Record record;
        record.kind = Impl::EKind::CREATE;
        record.deferred = deferred;
        target.records.push_back(std::move(record));
        ++target.create_count;
        return deferred;
    }

    void EcsCommandBuffer::recordDestroy(
        std::uint32_t producer,
        std::uint32_t generation,
        Entity entity
    ) noexcept
    {
        if (producer >= impl_->producers.size() ||
            generation != impl_->generation ||
            !impl_->producers[producer].active)
        {
            fail(producer, generation, EEcsCommandError::STALE_WRITER);
            return;
        }
        auto& target = impl_->producers[producer];
        if (impl_->failed || target.records.size() >= target.max_commands)
        {
            fail(producer, generation, EEcsCommandError::CAPACITY_EXCEEDED);
            return;
        }
        Impl::Record record;
        record.kind = Impl::EKind::DESTROY;
        record.entity = entity;
        target.records.push_back(std::move(record));
    }

    bool EcsCommandBuffer::recordPayload(
        std::uint32_t producer,
        std::uint32_t generation,
        Entity entity,
        DeferredEntity deferred,
        bool uses_deferred,
        const EcsCommandWriter::RawCommandVTable& table,
        void* source
    ) noexcept
    {
        if (producer >= impl_->producers.size() ||
            generation != impl_->generation ||
            !impl_->producers[producer].active)
        {
            fail(producer, generation, EEcsCommandError::STALE_WRITER);
            return false;
        }
        auto& target = impl_->producers[producer];
        if (uses_deferred &&
            (deferred.producer != producer || deferred.generation != generation ||
             deferred.ordinal >= target.create_count))
        {
            fail(producer, generation, EEcsCommandError::INVALID_DEFERRED_ENTITY);
            return false;
        }
        if (impl_->failed || target.records.size() >= target.max_commands)
        {
            fail(producer, generation, EEcsCommandError::CAPACITY_EXCEEDED);
            return false;
        }
        void* payload = target.arena.allocate(table.size, table.alignment);
        if (payload == nullptr)
        {
            fail(producer, generation, EEcsCommandError::CAPACITY_EXCEEDED);
            return false;
        }
        try
        {
            table.move_construct(payload, source);
        }
        catch (const std::bad_alloc&)
        {
            fail(producer, generation, EEcsCommandError::ALLOCATION_FAILURE);
            return false;
        }
        catch (...)
        {
            fail(
                producer,
                generation,
                EEcsCommandError::COMPONENT_CONSTRUCTION_FAILURE
            );
            return false;
        }
        Impl::Record record;
        record.kind = Impl::EKind::EMPLACE;
        record.entity = entity;
        record.deferred = deferred;
        record.uses_deferred = uses_deferred;
        record.payload = payload;
        record.table = table;
        target.records.push_back(std::move(record));
        return true;
    }

    bool EcsCommandBuffer::recordRemove(
        std::uint32_t producer,
        std::uint32_t generation,
        Entity entity,
        EcsCommandWriter::RemoveFn remove
    ) noexcept
    {
        if (producer >= impl_->producers.size() ||
            generation != impl_->generation ||
            !impl_->producers[producer].active)
        {
            fail(producer, generation, EEcsCommandError::STALE_WRITER);
            return false;
        }
        auto& target = impl_->producers[producer];
        if (impl_->failed || target.records.size() >= target.max_commands)
        {
            fail(producer, generation, EEcsCommandError::CAPACITY_EXCEEDED);
            return false;
        }
        Impl::Record record;
        record.kind = Impl::EKind::REMOVE;
        record.entity = entity;
        record.remove = remove;
        target.records.push_back(std::move(record));
        return true;
    }

    void EcsCommandBuffer::fail(
        std::uint32_t producer,
        std::uint32_t generation,
        EEcsCommandError error
    ) noexcept
    {
        if (impl_->failed)
            return;
        impl_->failed = true;
        impl_->failure = {error, producer};
        if (generation != impl_->generation)
            impl_->failure.code = EEcsCommandError::STALE_WRITER;
    }

    void EcsCommandBuffer::end(
        std::uint32_t producer,
        std::uint32_t generation
    ) noexcept
    {
        if (generation == impl_->generation && producer < impl_->producers.size())
            impl_->producers[producer].active = false;
    }

    lux::cxx::expected<void, EcsCommandFailure> applyEcsCommands(
        Registry& registry,
        EcsCommandBuffer& commands
    ) noexcept
    {
        for (std::size_t producer_index{};
             producer_index < commands.impl_->producers.size();
             ++producer_index)
        {
            if (commands.impl_->producers[producer_index].active)
            {
                return lux::cxx::unexpected(EcsCommandFailure{
                    EEcsCommandError::ACTIVE_WRITER,
                    producer_index});
            }
        }
        if (commands.impl_->failed)
        {
            const auto failure = commands.impl_->failure;
            commands.discardPending();
            return lux::cxx::unexpected(failure);
        }

        for (std::size_t producer_index{};
             producer_index < commands.impl_->producers.size();
             ++producer_index)
        {
            auto& producer = commands.impl_->producers[producer_index];
            producer.resolved.clear();
            for (std::size_t command_index{};
                 command_index < producer.records.size();
                 ++command_index)
            {
                auto& record = producer.records[command_index];
                try
                {
                    Entity target = record.entity;
                    if (record.kind == EcsCommandBuffer::Impl::EKind::CREATE)
                    {
                        producer.resolved.push_back(registry.create());
                        continue;
                    }
                    if (record.uses_deferred)
                    {
                        if (record.deferred.producer != producer_index ||
                            record.deferred.generation != commands.impl_->generation ||
                            record.deferred.ordinal >= producer.resolved.size())
                        {
                            const EcsCommandFailure failure{
                                EEcsCommandError::INVALID_DEFERRED_ENTITY,
                                producer_index,
                                command_index};
                            commands.discardPending();
                            return lux::cxx::unexpected(failure);
                        }
                        target = producer.resolved[record.deferred.ordinal];
                    }
                    if (!registry.valid(target))
                    {
                        const EcsCommandFailure failure{
                            EEcsCommandError::INVALID_ENTITY,
                            producer_index,
                            command_index};
                        commands.discardPending();
                        return lux::cxx::unexpected(failure);
                    }
                    switch (record.kind)
                    {
                    case EcsCommandBuffer::Impl::EKind::DESTROY:
                        registry.destroy(target);
                        break;
                    case EcsCommandBuffer::Impl::EKind::EMPLACE:
                        record.table.apply(record.payload, registry, target);
                        break;
                    case EcsCommandBuffer::Impl::EKind::REMOVE:
                        record.remove(registry, target);
                        break;
                    case EcsCommandBuffer::Impl::EKind::CREATE:
                        break;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    const EcsCommandFailure failure{
                        EEcsCommandError::ALLOCATION_FAILURE,
                        producer_index,
                        command_index};
                    commands.discardPending();
                    return lux::cxx::unexpected(failure);
                }
                catch (...)
                {
                    const EcsCommandFailure failure{
                        EEcsCommandError::COMPONENT_CONSTRUCTION_FAILURE,
                        producer_index,
                        command_index};
                    commands.discardPending();
                    return lux::cxx::unexpected(failure);
                }
            }
            producer.clearPending();
        }
        return {};
    }
}
