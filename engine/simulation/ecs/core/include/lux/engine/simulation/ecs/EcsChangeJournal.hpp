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
        EcsChangeHistoryBudget() = delete;

        constexpr EcsChangeHistoryBudget(
            std::size_t initial,
            std::size_t maximum
        ) noexcept
            : initial_bytes(initial), max_bytes(maximum)
        {
        }

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

        /**
         * Borrow exact component changes using a consumer-owned cursor.
         *
         * Different cursors may read concurrently while the caller holds a
         * shared ecsChanges READ resource. The returned range is lexical and
         * must be destroyed before that resource lifetime ends. Journal writes
         * and invalidation require exclusive ecsChanges WRITE ownership.
         */
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

        /** Entity ranges have the same lexical MRSW contract as component ranges. */
        [[nodiscard]] EntityChanges read(
            EntityChangeCursor& cursor
        ) const noexcept;

        void invalidateHistory() noexcept;

        [[nodiscard]] std::uint64_t epoch() const noexcept;

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
