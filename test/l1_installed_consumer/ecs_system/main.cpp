#include <lux/engine/simulation/SystemRegistry.hpp>

namespace
{
    struct ProbeSystem final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.consumer.l1-ecs-system",
            .version = 1};
    };
}

int main()
{
    lux::simulation::SystemRegistry systems;
    const auto system = systems.emplace<ProbeSystem>();
    if (!system)
        return 1;
    const auto lease = systems.retain<ProbeSystem>(*system);
    return lease && systems.contains(*system) ? 0 : 2;
}
