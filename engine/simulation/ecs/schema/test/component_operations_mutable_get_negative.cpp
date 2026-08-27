#include <lux/engine/simulation/ecs/ComponentOperations.hpp>

struct component_operations_mutable_get_negative final
{
    int value{};
};

int
main()
{
    lux::simulation::ecs::EcsState world;
    const auto operations = lux::simulation::ecs::componentOperations<component_operations_mutable_get_negative>();
    void* mutable_value = operations.get(world, lux::simulation::ecs::NullEntity);
    (void)mutable_value;
}
