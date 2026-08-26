#pragma once

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

namespace lux::simulation::ecs::detail
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
} // namespace lux::simulation::ecs::detail
