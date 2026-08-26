#pragma once

#include <lux/engine/simulation/ecs/ComponentChanges.hpp>
#include <lux/engine/simulation/ecs/EntityChanges.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include <entt/core/type_info.hpp>

namespace lux::simulation::ecs
{
    struct EcsChangeHistoryBudget final
    {
        std::size_t initial_bytes;
        std::size_t max_bytes;
    };

    namespace detail
    {
        struct EcsChangeJournalAccess;
    }

    /**
     * Simulation-owned observation history for one ECS state. The journal is
     * independent from EcsState; callers decide which canonical mutations are
     * observable and when history must be invalidated.
     */
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EcsChangeJournal final
    {
      public:
        explicit EcsChangeJournal(EcsChangeHistoryBudget budget);
        ~EcsChangeJournal() noexcept;
        EcsChangeJournal(EcsChangeJournal&&) noexcept;
        EcsChangeJournal& operator=(EcsChangeJournal&&) noexcept;
        EcsChangeJournal(const EcsChangeJournal&) = delete;
        EcsChangeJournal& operator=(const EcsChangeJournal&) = delete;

        template <class Component>
        [[nodiscard]] ComponentChanges<Component> read(
            ChangeCursor<Component>& cursor
        ) const noexcept
        {
            return ComponentChanges<Component>::fromDetail(
                readComponentRaw(
                    entt::type_hash<Component>::value(),
                    detail::ChangeCursorAccess::epoch(cursor),
                    detail::ChangeCursorAccess::sequence(cursor)
                )
            );
        }

        [[nodiscard]] EntityChanges read(
            EntityChangeCursor& cursor
        ) const noexcept;

        void invalidateHistory() noexcept;

        [[nodiscard]] std::uint64_t epoch() const noexcept;
        [[nodiscard]] std::uint64_t recordWriteCountForTest() const noexcept;
        [[nodiscard]] std::size_t dynamicBlockAcquisitionsForTest() const noexcept;
        [[nodiscard]] std::uint64_t streamBindCountForTest() const noexcept;
        [[nodiscard]] std::uint64_t perRecordLookupCountForTest() const noexcept;
        void failNextStreamDescriptorForTest() noexcept;
        void failNextBlockAcquisitionForTest() noexcept;
        void failNextBlockAttachForTest() noexcept;

      private:
        struct Impl;

        [[nodiscard]] detail::ChangeRangeData readComponentRaw(
            std::uint64_t storage,
            std::uint64_t& cursor_epoch,
            std::uint64_t& cursor_sequence
        ) const noexcept;

        std::unique_ptr<Impl> impl_;

        friend struct detail::EcsChangeJournalAccess;
    };
} // namespace lux::simulation::ecs
