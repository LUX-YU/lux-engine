#include <lux/engine/simulation/SimulationSystem.hpp>

class system_missing_access_negative final
{
public:
    void update(lux::simulation::ecs::SystemContext&) noexcept
    {
    }
};

static_assert(lux::simulation::ecs::System<system_missing_access_negative>);
