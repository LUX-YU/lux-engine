#include <lux/engine/simulation/ecs/EcsChangeLog.hpp>

int
main()
{
    lux::simulation::ecs::WorldChangeCursor retired{};
    return static_cast<int>(retired.sequence);
}
