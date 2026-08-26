#include <lux/engine/simulation/ecs/ComponentOperations.hpp>

struct component_operations_clone_negative final
{
    int value{};
};

int main()
{
    const auto operations =
        lux::simulation::ecs::componentOperations<component_operations_clone_negative>();
    return operations.cloneable() ? 0 : 1;
}
