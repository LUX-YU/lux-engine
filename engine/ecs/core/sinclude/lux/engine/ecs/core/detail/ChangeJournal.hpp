#pragma once

#include <lux/engine/ecs/ComponentChanges.hpp>
#include <lux/engine/ecs/EntityChanges.hpp>
#include <lux/engine/ecs/Query.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::ecs::detail
{
    struct ChangeJournalConfigValue final
    {
        std::size_t initial_bytes{};
        std::size_t max_bytes{};
    };

    struct JournalRecord final
    {
        std::uint64_t sequence{};
        Entity entity{NullEntity};
        std::uint8_t kind{};
    };

    struct JournalStream final
    {
        std::vector<JournalRecord> records;
        std::size_t start{};
        std::size_t count{};
        std::uint64_t oldest_sequence{1};
        std::uint64_t next_sequence{1};
        std::uint64_t minimum_available{1};
        std::uint64_t last_write{};
        mutable std::size_t pins{};
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC ChangeJournal final
    {
      public:
        explicit ChangeJournal(ChangeJournalConfigValue config);
        ~ChangeJournal() noexcept;

        ChangeJournal(const ChangeJournal&) = delete;
        ChangeJournal& operator=(const ChangeJournal&) = delete;

        [[nodiscard]] ChangeRecorder recorder() noexcept;

        void recordComponent(
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;
        void recordEntity(Entity entity, EEntityChangeKind kind) noexcept;

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> read(
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            const auto found = component_streams_.find(
                entt::type_hash<Component>::value()
            );
            return readComponent(
                found == component_streams_.end() ? nullptr : found->second.get(),
                cursor
            );
        }

        [[nodiscard]] ChangeRangeData readComponentRaw(
            std::uint64_t storage,
            std::uint32_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) const noexcept;

        [[nodiscard]] EntityChanges read(EntityChangeCursor& cursor) const noexcept;

        [[nodiscard]] ChangeRangeData readEntityRaw(
            std::uint32_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) const noexcept;

        void establishBaseline() noexcept;

        [[nodiscard]] std::uint32_t epoch() const noexcept
        {
            return epoch_;
        }

      private:
        [[nodiscard]] JournalStream* ensureStream(std::uint64_t storage) noexcept;
        void append(JournalStream& stream, Entity entity, std::uint8_t kind) noexcept;
        [[nodiscard]] bool grow(JournalStream& stream) noexcept;

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> readComponent(
            JournalStream* stream,
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            const std::uint64_t next = stream == nullptr ? 1 : stream->next_sequence;
            const std::uint64_t oldest = stream == nullptr
                ? next
                : std::max(stream->oldest_sequence, stream->minimum_available);

            if (cursor.epoch_ != epoch_ || cursor.sequence_ == 0 ||
                cursor.sequence_ < oldest || cursor.sequence_ > next)
            {
                cursor.epoch_ = epoch_;
                cursor.sequence_ = next;
                return ComponentChanges<Component>(
                    nullptr,
                    next,
                    next,
                    EChangeReadStatus::RESYNC_REQUIRED
                );
            }

            const std::uint64_t begin = cursor.sequence_;
            cursor.sequence_ = next;
            if (stream == nullptr || begin == next)
            {
                return ComponentChanges<Component>(
                    nullptr,
                    next,
                    next,
                    EChangeReadStatus::CURRENT
                );
            }

            ++stream->pins;
            return ComponentChanges<Component>(
                stream,
                begin,
                next,
                EChangeReadStatus::CURRENT
            );
        }

        std::unordered_map<std::uint64_t, std::unique_ptr<JournalStream>>
            component_streams_;
        JournalStream entity_stream_;
        ChangeJournalConfigValue config_;
        std::size_t allocated_bytes_{};
        std::uint64_t write_sequence_{};
        std::uint32_t epoch_{1};
    };
} // namespace lux::ecs::detail
