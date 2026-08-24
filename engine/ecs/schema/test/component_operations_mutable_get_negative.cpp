#include <lux/engine/ecs/ComponentOperations.hpp>

struct component_operations_mutable_get_negative final
{
    int value{};
};

int main()
{
    lux::ecs::World world;
    const auto operations =
        lux::ecs::componentOperations<component_operations_mutable_get_negative>();
    void* mutable_value = operations.get(world, lux::ecs::NullEntity);
    (void)mutable_value;
}
