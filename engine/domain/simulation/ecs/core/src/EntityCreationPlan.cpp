#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>

#include <algorithm>

namespace lux::simulation::ecs
{
    namespace
    {
        template <class Visitor>
        lux::cxx::expected<void, EEntityPlanError> visitPlanned(const Registry &registry, std::size_t count,
                                                                Visitor &&visit)
        {
            const auto *pool = registry.storage<Entity>();
            const auto size = pool ? pool->size() : 0;
            std::size_t available = pool ? pool->free_list() : 0;
            using Traits = entt::entt_traits<Entity>;
            typename Traits::entity_type next{};
            for (std::size_t index{}; index < size; ++index)
            {
                next = (std::max)(next, Traits::to_entity(pool->data()[index]) + 1);
            }
            for (std::size_t index{}; index < count; ++index)
            {
                if (available == size && next >= Traits::entity_mask)
                {
                    return lux::cxx::unexpected(EEntityPlanError::EXHAUSTED);
                }
                const auto entity = available < size ? pool->data()[available++] : Traits::construct(next++, 0);
                if (!visit(index, entity))
                {
                    return lux::cxx::unexpected(EEntityPlanError::STRUCTURE_CHANGED);
                }
            }
            return {};
        }
    } // namespace

    lux::cxx::expected<EntityCreationPlan, EEntityPlanError> planEntityCreation(const Registry &registry,
                                                                                std::size_t count)
    {
        EntityCreationPlan result;
        result.entities_.reserve(count);
        const auto planned = visitPlanned(registry, count,
                                          [&](std::size_t, Entity entity)
                                          {
                                              result.entities_.push_back(entity);
                                              return true;
                                          });
        if (!planned)
        {
            return lux::cxx::unexpected(planned.error());
        }
        return result;
    }

    lux::cxx::expected<void, EEntityPlanError> validateEntityCreation(const Registry &registry,
                                                                      const EntityCreationPlan &plan)
    {
        return visitPlanned(registry, plan.entities().size(),
                            [&](std::size_t index, Entity entity) { return plan.entities()[index] == entity; });
    }
} // namespace lux::simulation::ecs
