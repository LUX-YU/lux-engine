#include <lux/engine/ecs/SystemId.hpp>

int main()
{
    lux::ecs::SystemId id;
    return static_cast<int>(id.index);
}
