#pragma once

#include <lux/engine/ecs/Parent.hpp>
#include <lux/engine/ecs/World.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class EHierarchyError : std::uint8_t
    {
        INVALID_ENTITY,
        SELF_PARENT,
        CYCLE,
        INVALID_PARENT,
        ALLOCATION_FAILURE,
    };

    class HierarchyIndex;

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    lux::cxx::expected<void, EHierarchyError> setParent(
        WorldEdit& edit,
        HierarchyIndex& hierarchy,
        Entity child,
        Entity parent
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    lux::cxx::expected<void, EHierarchyError> clearParent(
        WorldEdit& edit,
        HierarchyIndex& hierarchy,
        Entity child
    ) noexcept;

    class LUX_ENGINE_ECS_HIERARCHY_PUBLIC HierarchyIndex final
    {
      public:
        explicit HierarchyIndex(World& world) noexcept;

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        rebuild() noexcept;

        [[nodiscard]] bool boundTo(const World& world) const noexcept
        {
            return world_ == std::addressof(world);
        }

        [[nodiscard]] Entity parent(Entity entity) const noexcept;
        [[nodiscard]] std::span<const Entity> subtree(Entity entity) const noexcept;
        [[nodiscard]] std::span<const Entity> preorder() const noexcept;
        [[nodiscard]] bool canSetParent(Entity child, Entity parent) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;

        void clear() noexcept;

      private:
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        setEdge(Entity child, Entity parent) noexcept;
        void eraseEdge(Entity child) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError>
        rebuildOrder() noexcept;

        struct Interval final
        {
            Entity entity{NullEntity};
            std::uint32_t begin{};
            std::uint32_t end{};
        };

        std::vector<std::pair<Entity, Entity>> parents_;
        std::vector<Entity> preorder_;
        std::vector<Interval> intervals_;
        World* world_{};

        friend LUX_ENGINE_ECS_HIERARCHY_PUBLIC
        lux::cxx::expected<void, EHierarchyError>
        setParent(WorldEdit&, HierarchyIndex&, Entity, Entity) noexcept;
        friend LUX_ENGINE_ECS_HIERARCHY_PUBLIC
        lux::cxx::expected<void, EHierarchyError>
        clearParent(WorldEdit&, HierarchyIndex&, Entity) noexcept;
    };

} // namespace lux::ecs
