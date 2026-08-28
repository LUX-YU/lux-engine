#pragma once

#include <lux/engine/simulation/ecs/HierarchyIndex.hpp>

#include <entt/signal/sigh.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::simulation::ecs::detail
{
    enum class EHierarchyMutationKind : std::uint8_t
    {
        SET_PARENT,
        REMOVE_PARENT,
        ENTITY_DESTROYED,
    };

    struct HierarchyMutation final
    {
        EHierarchyMutationKind kind{EHierarchyMutationKind::REMOVE_PARENT};
        Entity entity{NullEntity};
        Entity parent{NullEntity};
    };

    class LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC HierarchyMaintenance final
    {
    public:
        HierarchyMaintenance(Registry& registry, HierarchyIndex& hierarchy, HierarchyDeltaBatch& deltas);
        ~HierarchyMaintenance() noexcept = default;
        HierarchyMaintenance(const HierarchyMaintenance&) = delete;
        HierarchyMaintenance& operator=(const HierarchyMaintenance&) = delete;
        HierarchyMaintenance(HierarchyMaintenance&&) = delete;
        HierarchyMaintenance& operator=(HierarchyMaintenance&&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> prepare(std::size_t mutation_capacity) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> update() noexcept;

    private:
        void onParentConstruct(Registry& registry, Entity entity) noexcept;
        void onParentUpdate(Registry& registry, Entity entity) noexcept;
        void onParentDestroy(Registry& registry, Entity entity) noexcept;
        [[nodiscard]] bool append(HierarchyMutation mutation) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EHierarchyError> rebuildFromRegistry() noexcept;

        Registry* registry_{};
        HierarchyIndex* hierarchy_{};
        HierarchyDeltaBatch* deltas_{};
        std::vector<HierarchyMutation> mutations_;
        std::vector<Entity> invalid_entities_;
        std::size_t capacity_{};
        bool exact_{true};
        bool rebuild_required_{true};
        bool suppress_signals_{};
        entt::scoped_connection constructed_;
        entt::scoped_connection updated_;
        entt::scoped_connection destroyed_;
    };
}
