#include <lux/engine/ecs/HierarchySystem.hpp>

#include <lux/engine/ecs/Parent.hpp>

namespace lux::ecs
{
    HierarchySystem::HierarchySystem(
        World& world,
        HierarchyIndex& hierarchy
    ) noexcept
        : world_(std::addressof(world)),
          hierarchy_(std::addressof(hierarchy))
    {
        detail::require(hierarchy.boundTo(world));
    }

    void HierarchySystem::update(SystemContext& context) noexcept
    {
        detail::require(world_ != nullptr && hierarchy_ != nullptr);
        hierarchy_->synchronize(context);
    }
} // namespace lux::ecs
