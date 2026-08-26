#include <lux/engine/ecs/EcsChangeLog.hpp>

int main()
{
    lux::ecs::WorldChangeCursor retired{};
    return static_cast<int>(retired.sequence);
}
