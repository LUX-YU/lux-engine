#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/System.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <memory>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchySystem final
        : public System
    {
      public:
        explicit HierarchySystem(HierarchyIndex& hierarchy);
        ~HierarchySystem() override;

        [[nodiscard]] SystemAccess access() const noexcept override;

        [[nodiscard]] lux::cxx::expected<void, SystemStartError>
        start(SystemStart& start) noexcept override;

        void update(SystemFrame& frame) noexcept override;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
