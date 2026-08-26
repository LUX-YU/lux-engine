#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/EcsTaskAccess.hpp>
#include <lux/engine/ecs/EcsState.hpp>
#include <lux/engine/ecs/WorldCommands.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchySystem final
    {
      public:
        inline static constexpr auto Access = makeSystemAccessSpec<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>>();
        inline static constexpr auto TaskAccess = access<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>>;

        HierarchySystem(EcsState& world, HierarchyIndex& hierarchy) noexcept;

        void update(EcsState& world, WorldCommands commands) noexcept;
        void invokeTask(
            EcsState& world,
            WorldChangeBatch&,
            WorldCommands commands
        ) noexcept
        {
            update(world, commands);
        }

      private:
        EcsState* world_{};
        HierarchyIndex* hierarchy_{};
    };
} // namespace lux::ecs
