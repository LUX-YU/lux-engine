#include <lux/engine/simulation/ecs/SystemContext.hpp>

struct Position final
{
};

void
retired_context_update_negative(lux::simulation::ecs::SystemContext& context, lux::simulation::ecs::Entity entity)
{
    context.update<Position>(entity, [](Position&) noexcept {});
}
