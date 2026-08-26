#include <lux/engine/ecs/SystemRegistry.hpp>

namespace
{
    struct ProbeSystem final
    {
        inline static constexpr auto Access =
            lux::ecs::makeSystemAccessSpec<>();
    };
}

int main()
{
    lux::ecs::SystemRegistry systems;
    const auto system = systems.emplace<ProbeSystem>();
    if (!system)
        return 1;
    const auto lease = systems.retain<ProbeSystem>(*system);
    return lease && systems.contains(*system) ? 0 : 2;
}
