#include <lux/engine/world/WorldObjectId.hpp>

int main()
{
    return lux::world::WorldObjectId{}.valid() ? 1 : 0;
}
