#pragma once

#include <lux/engine/ecs/ComponentChanges.hpp>
#include <lux/engine/ecs/EntityChanges.hpp>
#include <lux/engine/ecs/Query.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lux::ecs::detail
{
    struct EcsChangeLogConfigValue final
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

    inline constexpr std::size_t kJournalBlockBytes = 4096U;
    inline constexpr std::size_t kJournalBlockHeaderBytes =
        sizeof(std::size_t) + sizeof(std::uint64_t) + 3U * sizeof(void*);
    inline constexpr std::size_t kJournalRecordsPerBlock =
        (kJournalBlockBytes - kJournalBlockHeaderBytes) /
        sizeof(JournalRecord);
    static_assert(kJournalRecordsPerBlock != 0U);

    struct JournalBlock final
    {
        std::size_t count{};
        std::uint64_t first_write{};
        JournalBlock* stream_previous{};
        JournalBlock* stream_next{};
        JournalBlock* free_next{};
        std::array<JournalRecord, kJournalRecordsPerBlock> records{};
    };
    static_assert(sizeof(JournalBlock) <= kJournalBlockBytes);

    struct JournalStream final
    {
        JournalBlock* first{};
        JournalBlock* last{};
        std::size_t block_count{};
        std::size_t count{};
        std::uint64_t oldest_sequence{1};
        std::uint64_t next_sequence{1};
        std::uint64_t minimum_available{1};
        std::uint64_t last_write{};
        mutable std::size_t pins{};
    };

    struct JournalPosition final
    {
        JournalBlock* block{};
        std::size_t offset{};
    };

    class LUX_ENGINE_ECS_CORE_PUBLIC EcsChangeLog final
    {
      public:
        explicit EcsChangeLog(EcsChangeLogConfigValue config);
        ~EcsChangeLog() noexcept;

        EcsChangeLog(const EcsChangeLog&) = delete;
        EcsChangeLog& operator=(const EcsChangeLog&) = delete;

        [[nodiscard]] ChangeRecorder recorder() noexcept;
        [[nodiscard]] BoundEcsChangeStream bindComponent(
            std::uint64_t storage
        ) noexcept;

        [[nodiscard]] bool recordComponent(
            std::uint64_t storage,
            Entity entity,
            EComponentChangeKind kind
        ) noexcept;
        [[nodiscard]] bool recordEntity(
            Entity entity,
            EEntityChangeKind kind
        ) noexcept;

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> read(
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            return ComponentChanges<Component>::fromDetail(
                readComponentRaw(
                    entt::type_hash<Component>::value(),
                    cursor.epoch_,
                    cursor.sequence_
                )
            );
        }

        [[nodiscard]] ChangeRangeData readComponentRaw(
            std::uint64_t storage,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) const noexcept;

        [[nodiscard]] EntityChanges read(EntityChangeCursor& cursor) const noexcept;

        [[nodiscard]] ChangeRangeData readEntityRaw(
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) const noexcept;

        void establishBaseline() noexcept;
        void markHistoryLoss() noexcept;

        [[nodiscard]] std::uint64_t epoch() const noexcept
        {
            return epoch_;
        }

        [[nodiscard]] std::uint64_t recordWriteCountForTest() const noexcept
        {
            return record_write_count_;
        }

        [[nodiscard]] std::size_t dynamicBlockAcquisitionsForTest() const noexcept
        {
            return dynamic_block_acquisition_count_;
        }

        [[nodiscard]] std::uint64_t streamBindCountForTest() const noexcept
        {
            return stream_bind_count_;
        }

        [[nodiscard]] std::uint64_t perRecordLookupCountForTest() const noexcept
        {
            return per_record_lookup_count_;
        }

        void failNextStreamDescriptorForTest() noexcept
        {
            fail_next_stream_descriptor_for_test_ = true;
        }

        void failNextBlockAcquisitionForTest() noexcept
        {
            fail_next_block_acquisition_for_test_ = true;
        }

        void failNextBlockAttachForTest() noexcept
        {
            fail_next_block_attach_for_test_ = true;
        }

      private:
        [[nodiscard]] JournalStream* ensureStream(std::uint64_t storage) noexcept;
        [[nodiscard]] bool append(
            JournalStream& stream,
            Entity entity,
            std::uint8_t kind
        ) noexcept;
        [[nodiscard]] JournalBlock* acquireBlock() noexcept;
        void attachBlock(JournalStream& stream, JournalBlock& block) noexcept;
        [[nodiscard]] JournalBlock* detachFrontBlock(
            JournalStream& stream
        ) noexcept;
        void releaseBlock(JournalBlock& block) noexcept;
        void discardStream(JournalStream& stream) noexcept;
        [[nodiscard]] JournalStream* oldestEvictableStream() noexcept;
        [[nodiscard]] static JournalPosition positionAt(
            const JournalStream& stream,
            std::uint64_t sequence
        ) noexcept;

        std::unordered_map<std::uint64_t, std::unique_ptr<JournalStream>>
            component_streams_;
        JournalStream entity_stream_;
        EcsChangeLogConfigValue config_;
        std::vector<std::unique_ptr<JournalBlock>> owned_blocks_;
        JournalBlock* free_blocks_{};
        std::size_t max_blocks_{};
        std::uint64_t write_sequence_{};
        std::uint64_t epoch_{1};
        std::uint64_t record_write_count_{};
        std::uint64_t stream_bind_count_{};
        std::uint64_t per_record_lookup_count_{};
        std::size_t dynamic_block_acquisition_count_{};
        bool fail_next_stream_descriptor_for_test_{};
        bool fail_next_block_acquisition_for_test_{};
        bool fail_next_block_attach_for_test_{};
    };
} // namespace lux::ecs::detail
