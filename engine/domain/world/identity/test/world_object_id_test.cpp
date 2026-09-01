#include <lux/engine/world/WorldObjectId.hpp>

#include <cassert>
#include <type_traits>

int main()
{
    static_assert(std::is_trivially_copyable_v<lux::world::WorldObjectId>);

    const lux::world::WorldObjectId nil{};
    assert(!nil.valid());

    const lux::world::WorldObjectId first{uuids::uuid::from_string("00000000-0000-0000-0000-000000000001").value()};
    const lux::world::WorldObjectId second{uuids::uuid::from_string("00000000-0000-0000-0000-000000000002").value()};
    assert(first.valid());
    assert(lux::world::WorldObjectIdLess{}(first, second));
    assert(lux::world::WorldObjectIdHash{}(first) == std::hash<uuids::uuid>{}(first.value));
    return 0;
}
