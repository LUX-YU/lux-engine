#pragma once

#include <lux/engine/ecs/TransformSystem.hpp>

namespace lux::ecs::detail
{
    struct TransformSystemTestAccess final
    {
        [[nodiscard]] static std::size_t visitedNodes(
            const Transform2DSystem& system
        ) noexcept
        {
            return system.visitedNodesLastUpdate();
        }

        [[nodiscard]] static std::size_t retainedDenseBytes(
            const Transform2DSystem& system
        ) noexcept
        {
            return system.retainedDenseBytes();
        }

        [[nodiscard]] static std::size_t retainedDenseBytes(
            const Transform3DSystem& system
        ) noexcept
        {
            return system.retainedDenseBytes();
        }

        [[nodiscard]] static std::size_t visitedNodes(
            const Transform3DSystem& system
        ) noexcept
        {
            return system.visitedNodesLastUpdate();
        }
    };
} // namespace lux::ecs::detail
