#include <lux/engine/domain/WorldObjectId.hpp>

int main()
{
    return lux::domain::WorldObjectId{}.valid() ? 1 : 0;
}
