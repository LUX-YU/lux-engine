#include <lux/engine/ecs/core/detail/ChangeJournal.hpp>

#include <lux/engine/ecs/World.hpp>

#include <algorithm>
#include <utility>

namespace lux::ecs::detail
{
    namespace
    {
        constexpr std::size_t kBlockBytes = 4096U;

        [[nodiscard]] const JournalRecord& recordAt(
            const JournalStream& stream,
            std::uint64_t sequence
        ) noexcept
        {
            require(
                sequence >= stream.oldest_sequence &&
                sequence < stream.next_sequence &&
                sequence >= stream.minimum_available &&
                stream.count != 0
            );
            const std::size_t offset = static_cast<std::size_t>(
                sequence - stream.oldest_sequence
            );
            require(offset < stream.count);
            const std::size_t index = (stream.start + offset) % stream.records.size();
            const JournalRecord& result = stream.records[index];
            require(result.sequence == sequence);
            return result;
        }
    } // namespace

    ChangeJournal::ChangeJournal(ChangeJournalConfigValue config)
        : config_(config)
    {
        require(config_.initial_bytes <= config_.max_bytes);
        require(config_.max_bytes >= kBlockBytes);
        component_streams_.reserve(
            std::max<std::size_t>(8, config_.initial_bytes / kBlockBytes)
        );
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

    bool ChangeJournal::grow(JournalStream& stream) noexcept
    {
        const std::size_t records_per_block = std::max<std::size_t>(
            1,
            kBlockBytes / sizeof(JournalRecord)
        );
        const std::size_t old_capacity = stream.records.size();
        const std::size_t requested = old_capacity + records_per_block;
        const std::size_t added_bytes = records_per_block * sizeof(JournalRecord);
        if (allocated_bytes_ > config_.max_bytes ||
            added_bytes > config_.max_bytes - allocated_bytes_)
        {
            return false;
        }

        try
        {
            std::vector<JournalRecord> replacement(requested);
            for (std::size_t index{}; index < stream.count; ++index)
            {
                replacement[index] = stream.records.empty()
                    ? JournalRecord{}
                    : stream.records[(stream.start + index) % stream.records.size()];
            }
            stream.records.swap(replacement);
            stream.start = 0;
            allocated_bytes_ += added_bytes;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void ChangeJournal::append(
        JournalStream& stream,
        Entity entity,
        std::uint8_t kind
    ) noexcept
    {
        const std::uint64_t sequence = stream.next_sequence++;
        stream.last_write = ++write_sequence_;

        if (stream.records.empty() && !grow(stream))
        {
            stream.minimum_available = stream.next_sequence;
            return;
        }
        if (stream.count == stream.records.size())
            (void)grow(stream);
        if (stream.count == stream.records.size())
        {
            if (stream.pins != 0)
            {
                stream.minimum_available = stream.next_sequence;
                return;
            }
            stream.start = (stream.start + 1) % stream.records.size();
            ++stream.oldest_sequence;
            --stream.count;
        }

        const std::size_t index = (stream.start + stream.count) %
            stream.records.size();
        stream.records[index] = JournalRecord{sequence, entity, kind};
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
        const auto reset = [](JournalStream& stream) noexcept
        {
            require(stream.pins == 0);
            stream.start = 0;
            stream.count = 0;
            stream.oldest_sequence = 1;
            stream.next_sequence = 1;
            stream.minimum_available = 1;
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
