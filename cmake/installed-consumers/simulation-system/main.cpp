#include <lux/engine/simulation/SystemRegistry.hpp>
#include <lux/engine/simulation/SystemAccessSpec.hpp>

namespace
{
    struct Probe final : lux::simulation::StaticSystemAccess<> {};
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
