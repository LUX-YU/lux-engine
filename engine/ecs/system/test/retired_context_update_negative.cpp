#include <lux/engine/ecs/SystemContext.hpp>

struct Position final
{
};

void retired_context_update_negative(
    lux::ecs::SystemContext& context,
    lux::ecs::Entity entity
)
{
    context.update<Position>(entity, [](Position&) noexcept {});
}
