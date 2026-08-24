#pragma once

#include <lux/engine/ecs/HierarchyIndex.hpp>

namespace lux::ecs::detail
{
    struct HierarchyIndexTestAccess final
    {
        [[nodiscard]] static std::size_t visitedNodes(
            const HierarchyIndex& hierarchy
        ) noexcept
        {
            return hierarchy.visitedNodesLastUpdate();
        }
    };
} // namespace lux::ecs::detail
