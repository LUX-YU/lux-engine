#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>

namespace
{
    struct Probe final
    {
        inline static constexpr auto Access =
            lux::simulation::makeSystemAccessSpec<>();
        inline static constexpr lux::simulation::SystemDescription Description{
            .canonical_name = "lux.consumer.simulation-system",
            .version = 1};
    };
}

int main()
{
    lux::simulation::SystemRegistry systems;
    const auto id = systems.emplace<Probe>();
    if (!id)
        return 1;
    const auto lease = systems.retain<Probe>(*id);
    return lease ? 0 : 1;
}
