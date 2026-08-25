#include <lux/engine/ecs/core/detail/CommandStorage.hpp>

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace lux::ecs
{
    WorldCommands::WorldCommands(
        detail::CommandShard& shard,
        std::uint32_t generation
    ) noexcept
        : shard_(&shard), generation_(generation)
    {
    }

    WorldCommands::operator bool() const noexcept
    {
        return shard_ != nullptr && shard_->accepts(generation_);
    }

    ECommandResult WorldCommands::pushRaw(
        const CommandVTable& table,
        void* source
    ) const noexcept
    {
        if (shard_ == nullptr)
            return ECommandResult::STALE_WRITER;
        return shard_->push(generation_, table, source);
    }
} // namespace lux::ecs

namespace lux::ecs::detail
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
    )
    {
        detail::require(size != 0 && alignment != 0);
        for (std::size_t index = cursor_; index < blocks_.size(); ++index)
        {
            Block& block = blocks_[index];
            const auto address = reinterpret_cast<std::uintptr_t>(
                block.data.get() + block.used
            );
            const std::size_t padding = static_cast<std::size_t>(
                (alignment - address % alignment) % alignment
            );
            if (padding <= block.size - block.used &&
                size <= block.size - block.used - padding)
            {
                block.used += padding;
                void* result = block.data.get() + block.used;
                block.used += size;
                cursor_ = index;
                return result;
            }
        }

        const std::size_t block_size = std::max<std::size_t>(
            4096,
            size + alignment
        );
        Block block;
        block.data = std::make_unique<std::byte[]>(block_size);
        block.size = block_size;
        blocks_.push_back(std::move(block));
        ++allocation_events_;
        cursor_ = blocks_.size() - 1;
        return allocate(size, alignment);
    }

    void CommandArena::reserve(std::size_t bytes)
    {
        if (bytes == 0)
            return;
        std::size_t capacity{};
        for (const Block& block : blocks_)
            capacity += block.size;
        if (capacity >= bytes)
            return;
        Block block;
        block.size = bytes - capacity;
        block.data = std::make_unique<std::byte[]>(block.size);
        blocks_.push_back(std::move(block));
        ++allocation_events_;
    }

    void CommandArena::reset() noexcept
    {
        for (Block& block : blocks_)
            block.used = 0;
        cursor_ = 0;
    }

    void CommandArena::swap(CommandArena& other) noexcept
    {
        blocks_.swap(other.blocks_);
        std::swap(cursor_, other.cursor_);
        std::swap(allocation_events_, other.allocation_events_);
    }

    std::size_t CommandArena::allocationEvents() const noexcept
    {
        return allocation_events_;
    }

    CommandShard::CommandShard(std::uint32_t generation) noexcept
        : generation_(generation == 0 ? 1 : generation)
    {
    }

    CommandShard::CommandShard(CommandShard&& other) noexcept
        : pending_(std::move(other.pending_)),
          pending_arena_(std::move(other.pending_arena_)),
          generation_(other.generation_),
          discarded_(other.discarded_),
          record_allocation_events_(other.record_allocation_events_),
          fail_next_push_for_test_(other.fail_next_push_for_test_)
    {
        detail::require(!other.active_ && !other.applying_);
        other.discarded_ = 0U;
        other.record_allocation_events_ = 0U;
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
        fail_next_push_for_test_ = other.fail_next_push_for_test_;
        other.discarded_ = 0U;
        other.record_allocation_events_ = 0U;
        other.fail_next_push_for_test_ = false;
        return *this;
    }

    void CommandShard::reserve(std::size_t count)
    {
        pending_.reserve(count);
        pending_arena_.reserve(count * 64U);
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
        const WorldCommands::CommandVTable& table,
        void* source
    ) noexcept
    {
        if (!accepts(writer_generation))
        {
            ++discarded_;
            return ECommandResult::STALE_WRITER;
        }

        if (fail_next_push_for_test_)
        {
            fail_next_push_for_test_ = false;
            return ECommandResult::ALLOCATION_FAILURE;
        }

        try
        {
            if (pending_.size() == pending_.capacity())
            {
                pending_.reserve(std::max<std::size_t>(
                    8,
                    pending_.capacity() * 2
                ));
                ++record_allocation_events_;
            }
            void* payload = pending_arena_.allocate(
                table.size,
                table.alignment
            );
            table.move_construct(payload, source);

            CommandRecord record;
            record.payload = payload;
            record.apply = table.apply;
            record.destroy = table.destroy;
            pending_.push_back(std::move(record));
        }
        catch (...)
        {
            return ECommandResult::ALLOCATION_FAILURE;
        }

        return ECommandResult::ACCEPTED;
    }

    WorldCommands CommandShard::beginExecution() noexcept
    {
        detail::require(!active_ && !applying_);
        ++generation_;
        if (generation_ == 0)
            ++generation_;
        active_ = true;
        return WorldCommands(*this, generation_);
    }

    void CommandShard::endExecution() noexcept
    {
        detail::require(active_ && !applying_);
        active_ = false;
        ++generation_;
        if (generation_ == 0)
            ++generation_;
    }

    void CommandShard::applyPending(WorldMutation& edit) noexcept
    {
        detail::require(!active_ && !applying_);
        applying_ = true;
        for (CommandRecord& record : pending_)
        {
            record.apply(record.payload, edit);
            record.reset();
        }
        pending_.clear();
        pending_arena_.reset();
        applying_ = false;
    }
} // namespace lux::ecs::detail
