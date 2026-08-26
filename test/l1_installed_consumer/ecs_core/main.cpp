#include <lux/engine/simulation/ecs/EcsState.hpp>

int main()
{
    lux::simulation::ecs::EcsState state;
    lux::simulation::ecs::Entity entity{};
    {
        auto mutation = state.mutate();
        if (!mutation)
            return 1;
        entity = mutation->create();
    }
    return state.valid(entity) ? 0 : 2;
}
