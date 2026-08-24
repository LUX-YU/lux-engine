#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>
#include <lux/engine/ecs/System.hpp>
#include <lux/engine/ecs/transform/visibility.h>

#include <memory>

namespace lux::ecs
{
    class LUX_ENGINE_ECS_TRANSFORM_PUBLIC Transform2DSystem final
        : public System
    {
      public:
        explicit Transform2DSystem(HierarchyIndex& hierarchy);
        ~Transform2DSystem() override;

        void update(SystemFrame& frame) noexcept override;

        [[nodiscard]] SystemAccess access() const noexcept override;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class LUX_ENGINE_ECS_TRANSFORM_PUBLIC Transform3DSystem final
        : public System
    {
      public:
        explicit Transform3DSystem(HierarchyIndex& hierarchy);
        ~Transform3DSystem() override;

        void update(SystemFrame& frame) noexcept override;

        [[nodiscard]] SystemAccess access() const noexcept override;

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs
