#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchySystem final
        : public StaticSystemAccess<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>
        >
    {
      public:
        HierarchySystem(World& world, HierarchyIndex& hierarchy) noexcept;

        void update(SystemContext& context) noexcept;

      private:
        World* world_{};
        HierarchyIndex* hierarchy_{};
    };
} // namespace lux::ecs
