#include <lux/engine/simulation/ecs/EcsState.hpp>

int main()
{
    lux::simulation::ecs::EcsState state;
    auto mutation = state.mutate();
    if (!mutation)
        return 1;
    const auto entity = mutation->create();
    return state.valid(entity) ? 0 : 2;
}
