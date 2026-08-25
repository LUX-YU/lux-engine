#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/SystemContext.hpp>
#include <lux/engine/ecs/SystemStart.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <memory>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchySystem final
        : public StaticSystemAccess<
            Read<Parent>,
            ExternalWrite<HierarchyIndex>
        >
    {
      public:
        explicit HierarchySystem(HierarchyIndex& hierarchy);
        ~HierarchySystem();

        [[nodiscard]] lux::cxx::expected<void, SystemStartError>
        start(SystemStart& start) noexcept;

        void update(SystemContext& context) noexcept;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
