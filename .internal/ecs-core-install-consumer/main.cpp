#include <lux/engine/ecs/Registry.hpp>

int main()
{
    lux::ecs::Registry registry;
    const lux::ecs::Entity entity = registry.create();
    const bool valid = entity != lux::ecs::kNullEntity && registry.valid(entity);
    registry.destroy(entity);
    return valid ? 0 : 1;
}
