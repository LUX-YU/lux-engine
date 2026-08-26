#include <lux/engine/simulation/ecs/HierarchySystem.hpp>

#include <lux/engine/simulation/ecs/Parent.hpp>

namespace lux::simulation::ecs
{
    HierarchySystem::HierarchySystem(
        EcsState& world,
        HierarchyIndex& hierarchy
    ) noexcept
        : world_(std::addressof(world)),
          hierarchy_(std::addressof(hierarchy))
    {
        detail::require(hierarchy.boundTo(world));
    }

    void HierarchySystem::update(
        EcsState& world,
        EcsCommands commands
    ) noexcept
    {
        detail::require(world_ != nullptr && hierarchy_ != nullptr);
        detail::require(world_ == std::addressof(world));
        hierarchy_->synchronize(commands);
    }
} // namespace lux::simulation::ecs
