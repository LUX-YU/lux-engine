#include <lux/engine/simulation/SystemConcept.hpp>

class system_throwing_update_negative final : public lux::simulation::ecs::StaticSystemAccess<>
{
public:
    void update(lux::simulation::ecs::SystemContext&)
    {
    }
};

static_assert(lux::simulation::ecs::System<system_throwing_update_negative>);
