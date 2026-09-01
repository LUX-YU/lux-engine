#include <lux/engine/simulation/SimulationSystem.hpp>

class untrusted_system_access_negative final
{
public:
    inline static constexpr lux::simulation::ecs::SystemAccessSpec Access{};
    void update(lux::simulation::ecs::SystemContext&) noexcept
    {
    }
};

static_assert(lux::simulation::ecs::System<untrusted_system_access_negative>);
