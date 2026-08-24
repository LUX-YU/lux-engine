#pragma once
#include <lux/engine/ecs/Registry.hpp>

namespace lux::ecs
{
    /// Links an entity to its parent in the scene hierarchy.
    /// Entities without this component are treated as root entities.
    ///
    /// STRUCTURAL WRITE CONTRACT: the parent link is
    /// ENCAPSULATED — reads go through parent(), and the only legal mutation
    /// path is setParent()/clearParent() (HierarchicalTransformSystem.hpp),
    /// which cycle-check and mutate via emplace_or_replace/remove so entt's
    /// on_construct/on_update/on_destroy signals fire for EVERY change.
    /// Event-driven consumers (the HierarchyIndex) can therefore trust the
    /// signals; a raw field write no longer compiles. Deliberately unreflected:
    /// cooked EntitySection data stores a batch-local entity ordinal and the
    /// materializer relocates that ordinal through setParent(). A live EnTT
    /// handle is never persisted as content.
    struct ParentComponent final
    {
        [[nodiscard]] lux::ecs::Entity parent() const noexcept { return parent_; }

    private:
        friend struct ParentAccess;   // the ONE key, held by setParent/clearParent
        lux::ecs::Entity parent_ = lux::ecs::kNullEntity;
    };

    /// The single point with write access to ParentComponent's link —
    /// used exclusively by setParent()/clearParent(). Not an API: consumers
    /// call those functions, never this.
    struct ParentAccess final
    {
        [[nodiscard]] static ParentComponent make(lux::ecs::Entity parent) noexcept
        {
            ParentComponent h;
            h.parent_ = parent;
            return h;
        }
    };

} // namespace lux::ecs
