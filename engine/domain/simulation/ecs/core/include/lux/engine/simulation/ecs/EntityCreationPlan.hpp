#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/ecs/core/visibility.h>

#include <span>
#include <vector>

namespace lux::simulation::ecs
{
    enum class EEntityPlanError
    {
        EXHAUSTED,
        STRUCTURE_CHANGED
    };

    class EntityCreationPlan;
    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<EntityCreationPlan, EEntityPlanError>
    planEntityCreation(const Registry &registry, std::size_t count);

    // A prediction, not a reservation. The Registry owner must keep structural exclusion
    // from validation through creation of all entities in the returned order.
    class LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC EntityCreationPlan final
    {
      public:
        EntityCreationPlan(EntityCreationPlan &&) noexcept = default;
        EntityCreationPlan &operator=(EntityCreationPlan &&) noexcept = default;
        EntityCreationPlan(const EntityCreationPlan &) = delete;
        EntityCreationPlan &operator=(const EntityCreationPlan &) = delete;
        [[nodiscard]] std::span<const Entity> entities() const noexcept
        {
            return entities_;
        }

      private:
        EntityCreationPlan() = default;
        std::vector<Entity> entities_;
        friend LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<EntityCreationPlan, EEntityPlanError>
        planEntityCreation(const Registry &, std::size_t);
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_ECS_CORE_PUBLIC lux::cxx::expected<void, EEntityPlanError>
    validateEntityCreation(const Registry &registry, const EntityCreationPlan &plan);
} // namespace lux::simulation::ecs
