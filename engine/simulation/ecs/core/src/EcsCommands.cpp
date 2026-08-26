#include <lux/engine/simulation/ecs/core/detail/CommandStorage.hpp>

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace lux::simulation::ecs
{
    EcsCommands::EcsCommands(
        detail::CommandShard& shard,
        std::uint32_t generation
    ) noexcept
        : shard_(&shard), generation_(generation)
    {
    }

    EcsCommands::operator bool() const noexcept
    {
        return shard_ != nullptr && shard_->accepts(generation_);
    }

    ECommandResult EcsCommands::pushRaw(
        const CommandVTable& table,
        void* source
    ) const noexcept
    {
        if (shard_ == nullptr)
            return ECommandResult::STALE_WRITER;
        return shard_->push(generation_, table, source);
    }
} // namespace lux::simulation::ecs

namespace lux::simulation::ecs::detail
{
    CommandRecord::CommandRecord(CommandRecord&& other) noexcept
        : payload(std::exchange(other.payload, nullptr)),
          apply(std::exchange(other.apply, nullptr)),
          destroy(std::exchange(other.destroy, nullptr))
    {
    }

    CommandRecord& CommandRecord::operator=(CommandRecord&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            payload = std::exchange(other.payload, nullptr);
            apply = std::exchange(other.apply, nullptr);
            destroy = std::exchange(other.destroy, nullptr);
        }
        return *this;
    }

    CommandRecord::~CommandRecord() noexcept
    {
        reset();
    }

    void CommandRecord::reset() noexcept
    {
        if (payload == nullptr)
            return;
        destroy(payload);
        payload = nullptr;
    }

    void* CommandArena::allocate(
        std::size_t size,
        std::size_t alignment
    ) noexcept
    {
        detail::require(size != 0 && alignment != 0);
        if (used_ > storage_.size())
            return nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(
            storage_.data() + used_
        );
        const std::size_t padding = static_cast<std::size_t>(
            (alignment - address % alignment) % alignment
        );
        if (padding > storage_.size() - used_ ||
            size > storage_.size() - used_ - padding)
        {
            return nullptr;
        }
        used_ += padding;
        void* result = storage_.data() + used_;
        used_ += size;
        return result;
    }

    void CommandArena::prepare(std::size_t bytes)
    {
        if (storage_.capacity() < bytes)
            ++allocation_events_;
        storage_.resize(bytes);
        used_ = 0U;
    }

    void CommandArena::reset() noexcept
    {
        used_ = 0U;
    }

    void CommandArena::swap(CommandArena& other) noexcept
    {
        storage_.swap(other.storage_);
        std::swap(used_, other.used_);
        std::swap(allocation_events_, other.allocation_events_);
    }

    std::size_t CommandArena::allocationEvents() const noexcept
    {
        return allocation_events_;
    }

    CommandShard::CommandShard(
        std::uint32_t generation,
        bool* batch_failed
    ) noexcept
        : generation_(generation == 0 ? 1 : generation),
          batch_failed_(batch_failed)
    {
    }

    CommandShard::CommandShard(CommandShard&& other) noexcept
        : pending_(std::move(other.pending_)),
          pending_arena_(std::move(other.pending_arena_)),
          generation_(other.generation_),
          discarded_(other.discarded_),
          record_allocation_events_(other.record_allocation_events_),
          max_commands_(other.max_commands_),
          batch_failed_(other.batch_failed_),
          fail_next_push_for_test_(other.fail_next_push_for_test_)
    {
        detail::require(!other.active_ && !other.applying_);
        other.discarded_ = 0U;
        other.record_allocation_events_ = 0U;
        other.max_commands_ = 0U;
        other.batch_failed_ = nullptr;
        other.fail_next_push_for_test_ = false;
    }

    CommandShard& CommandShard::operator=(CommandShard&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        detail::require(
            !active_ && !applying_ && !other.active_ && !other.applying_
        );
        pending_ = std::move(other.pending_);
        pending_arena_ = std::move(other.pending_arena_);
        generation_ = other.generation_;
        discarded_ = other.discarded_;
        record_allocation_events_ = other.record_allocation_events_;
        max_commands_ = other.max_commands_;
        batch_failed_ = other.batch_failed_;
        fail_next_push_for_test_ = other.fail_next_push_for_test_;
        other.discarded_ = 0U;
        other.record_allocation_events_ = 0U;
        other.max_commands_ = 0U;
        other.batch_failed_ = nullptr;
        other.fail_next_push_for_test_ = false;
        return *this;
    }

    void CommandShard::prepare(
        std::size_t command_capacity,
        std::size_t payload_capacity,
        bool& batch_failed
    )
    {
        detail::require(!active_ && !applying_);
        pending_.clear();
        if (pending_.capacity() < command_capacity)
            ++record_allocation_events_;
        pending_.reserve(command_capacity);
        pending_arena_.prepare(payload_capacity);
        max_commands_ = command_capacity;
        batch_failed_ = std::addressof(batch_failed);
    }

    void CommandShard::invalidate() noexcept
    {
        discarded_ += pending_.size();
        pending_.clear();
        pending_arena_.reset();
        active_ = false;
        ++generation_;
        if (generation_ == 0)
            ++generation_;
    }

    bool CommandShard::accepts(std::uint32_t generation) const noexcept
    {
        return active_ && generation == generation_;
    }

    std::uint32_t CommandShard::generation() const noexcept
    {
        return generation_;
    }

    std::size_t CommandShard::discarded() const noexcept
    {
        return discarded_;
    }

    std::size_t CommandShard::allocationEvents() const noexcept
    {
        return record_allocation_events_ +
            pending_arena_.allocationEvents();
    }

    ECommandResult CommandShard::push(
        std::uint32_t writer_generation,
        const EcsCommands::CommandVTable& table,
        void* source
    ) noexcept
    {
        if (!accepts(writer_generation))
        {
            ++discarded_;
            return ECommandResult::STALE_WRITER;
        }

        if (batch_failed_ != nullptr && *batch_failed_)
            return ECommandResult::BATCH_FAILED;

        if (fail_next_push_for_test_)
        {
            fail_next_push_for_test_ = false;
            if (batch_failed_ != nullptr)
                *batch_failed_ = true;
            return ECommandResult::CAPACITY_EXCEEDED;
        }

        if (pending_.size() >= max_commands_)
        {
            if (batch_failed_ != nullptr)
                *batch_failed_ = true;
            return ECommandResult::CAPACITY_EXCEEDED;
        }
        void* payload = pending_arena_.allocate(
            table.size,
            table.alignment
        );
        if (payload == nullptr)
        {
            if (batch_failed_ != nullptr)
                *batch_failed_ = true;
            return ECommandResult::CAPACITY_EXCEEDED;
        }
        table.move_construct(payload, source);

        CommandRecord record;
        record.payload = payload;
        record.apply = table.apply;
        record.destroy = table.destroy;
        pending_.push_back(std::move(record));

        return ECommandResult::ACCEPTED;
    }

    EcsCommands CommandShard::beginExecution() noexcept
    {
        detail::require(!active_ && !applying_);
        ++generation_;
        if (generation_ == 0)
            ++generation_;
        active_ = true;
        return EcsCommands(*this, generation_);
    }

    void CommandShard::endExecution() noexcept
    {
        detail::require(active_ && !applying_);
        active_ = false;
        ++generation_;
        if (generation_ == 0)
            ++generation_;
    }

    void CommandShard::applyPending(SimulationEcsMutation& mutation) noexcept
    {
        detail::require(!active_ && !applying_);
        applying_ = true;
        for (CommandRecord& record : pending_)
        {
            record.apply(record.payload, mutation);
            record.reset();
        }
        pending_.clear();
        pending_arena_.reset();
        applying_ = false;
    }
} // namespace lux::simulation::ecs::detail
