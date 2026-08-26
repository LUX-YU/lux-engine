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
            ExternalWrite<HierarchyIndex>>();
        inline static constexpr auto TaskAccess = access<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>>;

        HierarchySystem(EcsState& world, HierarchyIndex& hierarchy) noexcept;

        void update(EcsState& world, EcsCommands commands) noexcept;
        void invokeTask(
            EcsState& world,
            EcsChangeBatch&,
            EcsCommands commands
        ) noexcept
        {
            update(world, commands);
        }

      private:
        EcsState* world_{};
        HierarchyIndex* hierarchy_{};
    };
} // namespace lux::simulation::ecs
