#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>

#include <lux/engine/ecs/World.hpp>

#include <algorithm>
#include <utility>

namespace lux::ecs::detail
{
    namespace
    {
        [[nodiscard]] const JournalRecord& recordAt(
            const JournalStream& stream,
            std::uint64_t sequence
        ) noexcept
        {
            require(
                sequence >= stream.oldest_sequence &&
                sequence < stream.next_sequence &&
                stream.count != 0
            );
            const std::size_t offset = static_cast<std::size_t>(
                sequence - stream.oldest_sequence
            );
            require(offset < stream.count);
            const std::size_t block_offset =
                offset / kJournalRecordsPerBlock;
            const std::size_t record_offset =
                offset % kJournalRecordsPerBlock;
            require(block_offset < stream.block_count);
            const std::size_t block_index =
                (stream.block_start + block_offset) % stream.blocks.size();
            const JournalBlock* block = stream.blocks[block_index];
            require(block != nullptr && record_offset < block->count);
            const JournalRecord& result = block->records[record_offset];
            require(result.sequence == sequence);
            return result;
        }
    } // namespace

    ChangeJournal::ChangeJournal(ChangeJournalConfigValue config)
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
        const std::size_t initial_blocks = std::min(
            max_blocks_,
            config_.initial_bytes / kJournalBlockBytes
        );
        owned_blocks_.reserve(initial_blocks);
        free_blocks_.reserve(initial_blocks);
        for (std::size_t index{}; index < initial_blocks; ++index)
        {
            auto block = std::make_unique<JournalBlock>();
            free_blocks_.push_back(block.get());
            owned_blocks_.push_back(std::move(block));
        }
    }

    ChangeJournal::~ChangeJournal() noexcept = default;

    ChangeRecorder ChangeJournal::recorder() noexcept
    {
        return ChangeRecorder{
            this,
            [](void* context, std::uint64_t storage, Entity entity,
               EComponentChangeKind kind) noexcept
            {
                static_cast<ChangeJournal*>(context)->recordComponent(
                    storage, entity, kind
                );
            }};
    }

    JournalStream* ChangeJournal::ensureStream(std::uint64_t storage) noexcept
    {
        if (const auto found = component_streams_.find(storage);
            found != component_streams_.end())
        {
            return found->second.get();
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
            return nullptr;
        }
    }

    JournalBlock* ChangeJournal::blockAt(
        const JournalStream& stream,
        std::size_t offset
    ) noexcept
    {
        require(offset < stream.block_count && !stream.blocks.empty());
        return stream.blocks[
            (stream.block_start + offset) % stream.blocks.size()
        ];
    }

    bool ChangeJournal::attachBlock(
        JournalStream& stream,
        JournalBlock& block
    ) noexcept
    {
        try
        {
            if (stream.block_count == stream.blocks.size())
            {
                const std::size_t old_capacity = stream.blocks.size();
                const std::size_t new_capacity = std::min(
                    max_blocks_,
                    std::max<std::size_t>(1U, old_capacity * 2U)
                );
                if (new_capacity <= old_capacity)
                    return false;
                std::vector<JournalBlock*> replacement(new_capacity);
                for (std::size_t index{}; index < stream.block_count; ++index)
                    replacement[index] = blockAt(stream, index);
                stream.blocks.swap(replacement);
                stream.block_start = 0U;
            }
            const std::size_t index =
                (stream.block_start + stream.block_count) %
                stream.blocks.size();
            block.count = 0U;
            block.first_write = 0U;
            stream.blocks[index] = std::addressof(block);
            ++stream.block_count;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    JournalBlock* ChangeJournal::detachFrontBlock(
        JournalStream& stream
    ) noexcept
    {
        require(stream.block_count != 0U);
        JournalBlock* block = stream.blocks[stream.block_start];
        require(block != nullptr && block->count != 0U);
        stream.block_start =
            (stream.block_start + 1U) % stream.blocks.size();
        --stream.block_count;
        require(stream.count >= block->count);
        stream.count -= block->count;
        stream.oldest_sequence = stream.block_count == 0U
            ? stream.next_sequence
            : blockAt(stream, 0U)->records[0].sequence;
        stream.minimum_available = std::max(
            stream.minimum_available,
            stream.oldest_sequence
        );
        block->count = 0U;
        block->first_write = 0U;
        if (stream.block_count == 0U)
            stream.block_start = 0U;
        return block;
    }

    void ChangeJournal::discardStream(JournalStream& stream) noexcept
    {
        require(stream.pins == 0U);
        while (stream.block_count != 0U)
            free_blocks_.push_back(detachFrontBlock(stream));
        stream.count = 0U;
        stream.oldest_sequence = stream.next_sequence;
        stream.minimum_available = stream.next_sequence;
        stream.reset_pending = false;
    }

    JournalStream* ChangeJournal::oldestEvictableStream() noexcept
    {
        JournalStream* result{};
        const auto consider = [&result](JournalStream& candidate) noexcept
        {
            if (candidate.pins != 0U || candidate.block_count == 0U)
                return;
            if (result == nullptr ||
                blockAt(candidate, 0U)->first_write <
                    blockAt(*result, 0U)->first_write)
            {
                result = std::addressof(candidate);
            }
        };
        consider(entity_stream_);
        for (auto& [_, stream] : component_streams_)
            consider(*stream);
        return result;
    }

    JournalBlock* ChangeJournal::acquireBlock(
        JournalStream& stream
    ) noexcept
    {
        JournalBlock* block{};
        if (!free_blocks_.empty())
        {
            block = free_blocks_.back();
            free_blocks_.pop_back();
        }
        else if (owned_blocks_.size() < max_blocks_)
        {
            try
            {
                auto owned = std::make_unique<JournalBlock>();
                block = owned.get();
                owned_blocks_.push_back(std::move(owned));
            }
            catch (...)
            {
                return nullptr;
            }
        }
        else
        {
            JournalStream* victim = oldestEvictableStream();
            if (victim == nullptr)
                return nullptr;
            block = detachFrontBlock(*victim);
        }

        if (!attachBlock(stream, *block))
        {
            free_blocks_.push_back(block);
            return nullptr;
        }
        return block;
    }

    void ChangeJournal::append(
        JournalStream& stream,
        Entity entity,
        std::uint8_t kind
    ) noexcept
    {
        if (stream.reset_pending)
        {
            if (stream.pins != 0U)
            {
                ++stream.next_sequence;
                stream.minimum_available = stream.next_sequence;
                return;
            }
            discardStream(stream);
        }

        const std::uint64_t sequence = stream.next_sequence;
        JournalBlock* block = stream.block_count == 0U
            ? nullptr
            : blockAt(stream, stream.block_count - 1U);
        if (block == nullptr || block->count == kJournalRecordsPerBlock)
        {
            block = acquireBlock(stream);
            if (block == nullptr)
            {
                ++stream.next_sequence;
                stream.minimum_available = stream.next_sequence;
                stream.reset_pending = true;
                return;
            }
        }

        ++stream.next_sequence;
        stream.last_write = ++write_sequence_;
        if (block->count == 0U)
            block->first_write = stream.last_write;
        block->records[block->count] = JournalRecord{sequence, entity, kind};
        ++block->count;
        ++stream.count;
        stream.minimum_available = std::max(
            stream.minimum_available,
            stream.oldest_sequence
        );
    }

    void ChangeJournal::recordComponent(
        std::uint64_t storage,
        Entity entity,
        EComponentChangeKind kind
    ) noexcept
    {
        if (JournalStream* stream = ensureStream(storage))
            append(*stream, entity, static_cast<std::uint8_t>(kind));
    }

    void ChangeJournal::recordEntity(
        Entity entity,
        EEntityChangeKind kind
    ) noexcept
    {
        append(entity_stream_, entity, static_cast<std::uint8_t>(kind));
    }

    EntityChanges ChangeJournal::read(EntityChangeCursor& cursor) const noexcept
    {
        JournalStream* stream = const_cast<JournalStream*>(&entity_stream_);
        const std::uint64_t next = stream->next_sequence;
        const std::uint64_t oldest = std::max(
            stream->oldest_sequence,
            stream->minimum_available
        );
        if (cursor.epoch_ != epoch_ || cursor.sequence_ == 0 ||
            cursor.sequence_ < oldest || cursor.sequence_ > next)
        {
            cursor.epoch_ = epoch_;
            cursor.sequence_ = next;
            return EntityChanges(
                nullptr, next, next, EChangeReadStatus::RESYNC_REQUIRED
            );
        }

        const std::uint64_t begin = cursor.sequence_;
        cursor.sequence_ = next;
        if (begin == next)
        {
            return EntityChanges(
                nullptr, next, next, EChangeReadStatus::CURRENT
            );
        }
        ++stream->pins;
        return EntityChanges(
            stream, begin, next, EChangeReadStatus::CURRENT
        );
    }

    ChangeRangeData ChangeJournal::readComponentRaw(
        std::uint64_t storage,
        std::uint32_t& cursor_epoch,
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
                nullptr, next, next, EChangeReadStatus::RESYNC_REQUIRED};
        }

        const std::uint64_t begin = cursor_sequence;
        cursor_sequence = next;
        if (stream == nullptr || begin == next)
            return ChangeRangeData{nullptr, next, next, EChangeReadStatus::CURRENT};
        ++stream->pins;
        return ChangeRangeData{stream, begin, next, EChangeReadStatus::CURRENT};
    }

    ChangeRangeData ChangeJournal::readEntityRaw(
        std::uint32_t& cursor_epoch,
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
                nullptr, next, next, EChangeReadStatus::RESYNC_REQUIRED};
        }

        const std::uint64_t begin = cursor_sequence;
        cursor_sequence = next;
        if (begin == next)
            return ChangeRangeData{nullptr, next, next, EChangeReadStatus::CURRENT};
        ++stream->pins;
        return ChangeRangeData{stream, begin, next, EChangeReadStatus::CURRENT};
    }

    void ChangeJournal::establishBaseline() noexcept
    {
        ++epoch_;
        if (epoch_ == 0)
            ++epoch_;
        const auto reset = [this](JournalStream& stream) noexcept
        {
            require(stream.pins == 0U);
            discardStream(stream);
            stream.oldest_sequence = 1;
            stream.next_sequence = 1;
            stream.minimum_available = 1;
            stream.last_write = 0U;
            stream.reset_pending = false;
        };
        reset(entity_stream_);
        for (auto& [_, stream] : component_streams_)
            reset(*stream);
    }

    ComponentChange componentChangeAt(
        const void* stream,
        std::uint64_t sequence
    ) noexcept
    {
        const JournalRecord& record = recordAt(
            *static_cast<const JournalStream*>(stream), sequence
        );
        return ComponentChange{
            record.entity,
            static_cast<EComponentChangeKind>(record.kind)};
    }

    EntityChange entityChangeAt(
        const void* stream,
        std::uint64_t sequence
    ) noexcept
    {
        const JournalRecord& record = recordAt(
            *static_cast<const JournalStream*>(stream), sequence
        );
        return EntityChange{
            record.entity,
            static_cast<EEntityChangeKind>(record.kind)};
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
} // namespace lux::ecs::detail
