#include <lux/engine/simulation/ecs/core/detail/EcsChangeLog.hpp>

#include <lux/engine/simulation/ecs/EcsState.hpp>

#include <algorithm>
#include <utility>

namespace lux::simulation::ecs::detail
{
    namespace
    {
        [[nodiscard]] const JournalRecord& recordAt(
            const JournalBlock& block,
            std::size_t offset,
            std::uint64_t sequence
        ) noexcept
        {
            require(offset < block.count);
            const JournalRecord& result = block.records[offset];
            require(result.sequence == sequence);
            return result;
        }

        void advanceEpoch(std::uint64_t& epoch) noexcept
        {
            ++epoch;
            if (epoch == 0)
                ++epoch;
        }
    } // namespace

    EcsChangeLog::EcsChangeLog(EcsChangeLogConfigValue config)
        : config_(config),
          max_blocks_(config.max_bytes / kJournalBlockBytes)
    {
        require(config_.initial_bytes <= config_.max_bytes);
        require(max_blocks_ != 0U);
        component_streams_.reserve(
            std::max<std::size_t>(
                8,
                config_.initial_bytes / kJournalBlockBytes
            )
        );
        owned_blocks_.reserve(max_blocks_);
        const std::size_t initial_blocks = std::min(
            max_blocks_,
            config_.initial_bytes / kJournalBlockBytes
        );
        for (std::size_t index{}; index < initial_blocks; ++index)
        {
            auto block = std::make_unique<JournalBlock>();
            JournalBlock* value = block.get();
            owned_blocks_.push_back(std::move(block));
            releaseBlock(*value);
        }
    }

    EcsChangeLog::~EcsChangeLog() noexcept = default;

    ChangeRecorder EcsChangeLog::recorder() noexcept
    {
        return ChangeRecorder{
            this,
            [](void* context, std::uint64_t storage, Entity entity,
               EComponentChangeKind kind) noexcept
            {
                (void)static_cast<EcsChangeLog*>(context)->recordComponent(
                    storage, entity, kind
                );
            }};
    }

    BoundEcsChangeStream EcsChangeLog::bindComponent(
        std::uint64_t storage
    ) noexcept
    {
        ++stream_bind_count_;
        JournalStream* stream = ensureStream(storage);
        if (stream == nullptr)
            return {};
        return BoundEcsChangeStream{
            .owner = this,
            .stream = stream,
            .append = [](
                void* owner,
                void* target_stream,
                Entity entity,
                EComponentChangeKind kind
            ) noexcept
            {
                return static_cast<EcsChangeLog*>(owner)->append(
                    *static_cast<JournalStream*>(target_stream),
                    entity,
                    static_cast<std::uint8_t>(kind)
                );
            }
        };
    }

    JournalStream* EcsChangeLog::ensureStream(std::uint64_t storage) noexcept
    {
        if (const auto found = component_streams_.find(storage);
            found != component_streams_.end())
        {
            return found->second.get();
        }

        if (fail_next_stream_descriptor_for_test_)
        {
            fail_next_stream_descriptor_for_test_ = false;
            markHistoryLoss();
            return nullptr;
        }

        try
        {
            auto stream = std::make_unique<JournalStream>();
            JournalStream* result = stream.get();
            component_streams_.emplace(storage, std::move(stream));
            return result;
        }
        catch (...)
        {
            markHistoryLoss();
            return nullptr;
        }
    }

    JournalPosition EcsChangeLog::positionAt(
        const JournalStream& stream,
        std::uint64_t sequence
    ) noexcept
    {
        require(
            sequence >= stream.oldest_sequence &&
            sequence < stream.next_sequence &&
            stream.count != 0U
        );
        const std::size_t record_offset = static_cast<std::size_t>(
            sequence - stream.oldest_sequence
        );
        require(record_offset < stream.count);
        const std::size_t block_offset =
            record_offset / kJournalRecordsPerBlock;
        const std::size_t within_block =
            record_offset % kJournalRecordsPerBlock;

        JournalBlock* block{};
        if (block_offset <= stream.block_count / 2U)
        {
            block = stream.first;
            for (std::size_t index{}; index < block_offset; ++index)
                block = block->stream_next;
        }
        else
        {
            block = stream.last;
            for (std::size_t index = stream.block_count - 1U;
                 index > block_offset; --index)
            {
                block = block->stream_previous;
            }
        }
        require(block != nullptr && within_block < block->count);
        return JournalPosition{block, within_block};
    }

    void EcsChangeLog::attachBlock(
        JournalStream& stream,
        JournalBlock& block
    ) noexcept
    {
        require(
            block.stream_previous == nullptr &&
            block.stream_next == nullptr &&
            block.free_next == nullptr
        );
        block.count = 0U;
        block.first_write = 0U;
        block.stream_previous = stream.last;
        if (stream.last != nullptr)
            stream.last->stream_next = std::addressof(block);
        else
            stream.first = std::addressof(block);
        stream.last = std::addressof(block);
        ++stream.block_count;
    }

    JournalBlock* EcsChangeLog::detachFrontBlock(
        JournalStream& stream
    ) noexcept
    {
        require(stream.first != nullptr && stream.block_count != 0U);
        JournalBlock* block = stream.first;
        require(block->count != 0U);
        stream.first = block->stream_next;
        if (stream.first != nullptr)
            stream.first->stream_previous = nullptr;
        else
            stream.last = nullptr;
        block->stream_previous = nullptr;
        block->stream_next = nullptr;
        --stream.block_count;
        require(stream.count >= block->count);
        stream.count -= block->count;
        stream.oldest_sequence = stream.first == nullptr
            ? stream.next_sequence
            : stream.first->records[0].sequence;
        stream.minimum_available = std::max(
            stream.minimum_available,
            stream.oldest_sequence
        );
        return block;
    }

    void EcsChangeLog::releaseBlock(JournalBlock& block) noexcept
    {
        require(
            block.stream_previous == nullptr &&
            block.stream_next == nullptr
        );
        block.count = 0U;
        block.first_write = 0U;
        block.free_next = free_blocks_;
        free_blocks_ = std::addressof(block);
    }

    void EcsChangeLog::discardStream(JournalStream& stream) noexcept
    {
        require(stream.pins == 0U);
        while (stream.first != nullptr)
            releaseBlock(*detachFrontBlock(stream));
        stream.count = 0U;
        stream.oldest_sequence = stream.next_sequence;
        stream.minimum_available = stream.next_sequence;
    }

    JournalStream* EcsChangeLog::oldestEvictableStream() noexcept
    {
        JournalStream* result{};
        const auto consider = [&result](JournalStream& candidate) noexcept
        {
            if (candidate.pins != 0U || candidate.first == nullptr)
                return;
            if (result == nullptr ||
                candidate.first->first_write < result->first->first_write)
            {
                result = std::addressof(candidate);
            }
        };
        consider(entity_stream_);
        for (auto& [_, stream] : component_streams_)
            consider(*stream);
        return result;
    }

    JournalBlock* EcsChangeLog::acquireBlock() noexcept
    {
        if (fail_next_block_acquisition_for_test_)
        {
            fail_next_block_acquisition_for_test_ = false;
            return nullptr;
        }
        if (free_blocks_ != nullptr)
        {
            JournalBlock* result = free_blocks_;
            free_blocks_ = result->free_next;
            result->free_next = nullptr;
            return result;
        }
        if (owned_blocks_.size() < max_blocks_)
        {
            try
            {
                auto owned = std::make_unique<JournalBlock>();
                JournalBlock* result = owned.get();
                owned_blocks_.push_back(std::move(owned));
                ++dynamic_block_acquisition_count_;
                return result;
            }
            catch (...)
            {
                return nullptr;
            }
        }

        JournalStream* victim = oldestEvictableStream();
        return victim == nullptr ? nullptr : detachFrontBlock(*victim);
    }

    bool EcsChangeLog::append(
        JournalStream& stream,
        Entity entity,
        std::uint8_t kind
    ) noexcept
    {
        const std::uint64_t sequence = stream.next_sequence;
        JournalBlock* block = stream.last;
        if (block == nullptr || block->count == kJournalRecordsPerBlock)
        {
            block = acquireBlock();
            if (block == nullptr)
            {
                markHistoryLoss();
                return false;
            }
            if (fail_next_block_attach_for_test_)
            {
                fail_next_block_attach_for_test_ = false;
                releaseBlock(*block);
                markHistoryLoss();
                return false;
            }
            attachBlock(stream, *block);
        }

        ++stream.next_sequence;
        stream.last_write = ++write_sequence_;
        if (block->count == 0U)
            block->first_write = stream.last_write;
        block->records[block->count] = JournalRecord{sequence, entity, kind};
        ++block->count;
        ++stream.count;
        ++record_write_count_;
        stream.minimum_available = std::max(
            stream.minimum_available,
            stream.oldest_sequence
        );
        return true;
    }

    bool EcsChangeLog::recordComponent(
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        ++per_record_lookup_count_;
        if (JournalStream* stream = ensureStream(storage))
            return append(*stream, entity, static_cast<std::uint8_t>(kind));
        return false;
    }

    bool EcsChangeLog::recordEntity(
        Entity entity,
        EEntityChangeKind kind
    ) noexcept
    {
        return append(entity_stream_, entity, static_cast<std::uint8_t>(kind));
    }

    ChangeRangeData EcsChangeLog::readComponentRaw(
        std::uint64_t storage,
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) const noexcept
    {
        const auto found = component_streams_.find(storage);
        JournalStream* stream = found == component_streams_.end()
            ? nullptr
            : found->second.get();
        const std::uint64_t next = stream == nullptr ? 1 : stream->next_sequence;
        const std::uint64_t oldest = stream == nullptr
            ? next
            : std::max(stream->oldest_sequence, stream->minimum_available);
        if (cursor_epoch != epoch_ || cursor_sequence == 0 ||
            cursor_sequence < oldest || cursor_sequence > next)
        {
            cursor_epoch = epoch_;
            cursor_sequence = next;
            return ChangeRangeData{
                nullptr, nullptr, 0, next, next,
                EChangeReadStatus::RESYNC_REQUIRED};
        }

        const std::uint64_t begin = cursor_sequence;
        cursor_sequence = next;
        if (stream == nullptr || begin == next)
        {
            return ChangeRangeData{
                nullptr, nullptr, 0, next, next,
                EChangeReadStatus::CURRENT};
        }
        const JournalPosition position = positionAt(*stream, begin);
        ++stream->pins;
        return ChangeRangeData{
            stream, position.block, position.offset, begin, next,
            EChangeReadStatus::CURRENT};
    }

    EntityChanges EcsChangeLog::read(EntityChangeCursor& cursor) const noexcept
    {
        return EntityChanges::fromDetail(readEntityRaw(
            cursor.epoch_, cursor.sequence_
        ));
    }

    ChangeRangeData EcsChangeLog::readEntityRaw(
        std::uint64_t& cursor_epoch,
        std::uint64_t& cursor_sequence
    ) const noexcept
    {
        JournalStream* stream = const_cast<JournalStream*>(&entity_stream_);
        const std::uint64_t next = stream->next_sequence;
        const std::uint64_t oldest = std::max(
            stream->oldest_sequence,
            stream->minimum_available
        );
        if (cursor_epoch != epoch_ || cursor_sequence == 0 ||
            cursor_sequence < oldest || cursor_sequence > next)
        {
            cursor_epoch = epoch_;
            cursor_sequence = next;
            return ChangeRangeData{
                nullptr, nullptr, 0, next, next,
                EChangeReadStatus::RESYNC_REQUIRED};
        }

        const std::uint64_t begin = cursor_sequence;
        cursor_sequence = next;
        if (begin == next)
        {
            return ChangeRangeData{
                nullptr, nullptr, 0, next, next,
                EChangeReadStatus::CURRENT};
        }
        const JournalPosition position = positionAt(*stream, begin);
        ++stream->pins;
        return ChangeRangeData{
            stream, position.block, position.offset, begin, next,
            EChangeReadStatus::CURRENT};
    }

    void EcsChangeLog::establishBaseline() noexcept
    {
        advanceEpoch(epoch_);
        const auto reset = [this](JournalStream& stream) noexcept
        {
            require(stream.pins == 0U);
            discardStream(stream);
            stream.oldest_sequence = 1;
            stream.next_sequence = 1;
            stream.minimum_available = 1;
            stream.last_write = 0U;
        };
        reset(entity_stream_);
        for (auto& [_, stream] : component_streams_)
            reset(*stream);
    }

    void EcsChangeLog::markHistoryLoss() noexcept
    {
        advanceEpoch(epoch_);
    }

    ComponentChange componentChangeAt(
        const void* block,
        std::size_t block_offset,
        std::uint64_t sequence
    ) noexcept
    {
        require(block != nullptr);
        const JournalRecord& record = recordAt(
            *static_cast<const JournalBlock*>(block),
            block_offset,
            sequence
        );
        return ComponentChange{
            record.entity,
            static_cast<EComponentChangeKind>(record.kind)};
    }

    EntityChange entityChangeAt(
        const void* block,
        std::size_t block_offset,
        std::uint64_t sequence
    ) noexcept
    {
        require(block != nullptr);
        const JournalRecord& record = recordAt(
            *static_cast<const JournalBlock*>(block),
            block_offset,
            sequence
        );
        return EntityChange{
            record.entity,
            static_cast<EEntityChangeKind>(record.kind)};
    }

    void advanceChangePosition(
        const void*& block,
        std::size_t& block_offset,
        std::uint64_t& sequence
    ) noexcept
    {
        require(block != nullptr);
        const auto* current = static_cast<const JournalBlock*>(block);
        ++sequence;
        ++block_offset;
        if (block_offset == current->count)
        {
            block = current->stream_next;
            block_offset = 0U;
        }
    }

    void releaseChangeStream(const void* stream) noexcept
    {
        if (stream == nullptr)
            return;
        auto& value = *const_cast<JournalStream*>(
            static_cast<const JournalStream*>(stream)
        );
        require(value.pins != 0);
        --value.pins;
    }
} // namespace lux::simulation::ecs::detail
