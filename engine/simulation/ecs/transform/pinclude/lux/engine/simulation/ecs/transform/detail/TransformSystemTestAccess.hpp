#pragma once

#include <lux/engine/simulation/ecs/TransformSystem.hpp>

namespace lux::simulation::ecs::detail
{
    struct TransformSystemTestAccess final
    {
        [[nodiscard]] static std::size_t visitedNodes(const Transform2DSystem& system) noexcept
        {
            return system.visitedNodesLastUpdate();
        }

        [[nodiscard]] static std::size_t retainedDenseBytes(const Transform2DSystem& system) noexcept
        {
            return system.retainedDenseBytes();
        }

        [[nodiscard]] static std::size_t retainedDenseBytes(const Transform3DSystem& system) noexcept
        {
            return system.retainedDenseBytes();
        }

        [[nodiscard]] static std::size_t visitedNodes(const Transform3DSystem& system) noexcept
        {
            return system.visitedNodesLastUpdate();
        }
    };
} // namespace lux::simulation::ecs::detail
