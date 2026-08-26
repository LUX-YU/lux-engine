#pragma once

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>
#include <lux/engine/simulation/ecs/EcsTaskAccess.hpp>
#include <lux/engine/simulation/ecs/EcsState.hpp>
#include <lux/engine/simulation/ecs/EcsCommands.hpp>
#include <lux/engine/simulation/ecs/hierarchy/visibility.h>

namespace lux::simulation::ecs
{
    class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC HierarchySystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>,
            ExternalWrite<HierarchyMutationBatch>,
            ExternalWrite<HierarchyDeltaBatch>>();
        inline static constexpr auto TaskAccess = access<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>,
            ExternalWrite<HierarchyMutationBatch>,
            ExternalWrite<HierarchyDeltaBatch>>;
        inline static constexpr auto EcsChangesAccess = ecsChangesRead();

        HierarchySystem(
            EcsState& state,
            HierarchyIndex& hierarchy,
            HierarchyMutationBatch& mutations,
            HierarchyDeltaBatch& deltas
        ) noexcept;

        void update(
            EcsState& state,
            EcsChangeJournal& journal,
            EcsCommands commands
        ) noexcept;
        void invokeTask(
            EcsState& state,
            EcsChangeJournal& journal,
            EcsChangeBatch&,
            EcsCommands commands
        ) noexcept
        {
            update(state, journal, commands);
        }

      private:
        EcsState* state_{};
        HierarchyIndex* hierarchy_{};
        HierarchyMutationBatch* mutations_{};
        HierarchyDeltaBatch* deltas_{};
        ChangeCursor<Parent> parent_cursor_;
        EntityChangeCursor entity_cursor_;
        bool rebuild_required_{true};
    };
} // namespace lux::simulation::ecs
