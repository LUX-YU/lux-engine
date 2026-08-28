#include <lux/engine/simulation/SystemId.hpp>

int
main()
{
    lux::simulation::ecs::SystemId id;
    return static_cast<int>(id.index);
}
