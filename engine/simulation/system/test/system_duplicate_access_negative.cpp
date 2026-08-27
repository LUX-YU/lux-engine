#include <lux/engine/simulation/SystemConcept.hpp>

struct Position final
{
};

class system_duplicate_access_negative final
    : public lux::simulation::ecs::
          StaticSystemAccess<lux::simulation::ecs::Read<Position>, lux::simulation::ecs::Write<Position>>
{
public:
    void update(lux::simulation::ecs::SystemContext&) noexcept
    {
    }
};

static_assert(lux::simulation::ecs::System<system_duplicate_access_negative>);
