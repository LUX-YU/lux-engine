#include <lux/engine/simulation/ecs/EcsState.hpp>

int main()
{
    lux::simulation::ecs::EcsState state({
        .changes = {
            .initial_bytes = 4U * 1024U,
            .max_bytes = 64U * 1024U,
        },
    });
    lux::simulation::ecs::Entity entity{};
    {
        auto mutation = state.mutate();
        if (!mutation)
            return 1;
        entity = mutation->create();
    }
    return state.valid(entity) ? 0 : 2;
}
