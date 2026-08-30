#include <lux/engine/domain/WorldObjectId.hpp>

#include <cassert>
#include <type_traits>

int main()
{
    static_assert(std::is_trivially_copyable_v<lux::domain::WorldObjectId>);

    const lux::domain::WorldObjectId nil{};
    assert(!nil.valid());

    const lux::domain::WorldObjectId first{uuids::uuid::from_string("00000000-0000-0000-0000-000000000001").value()};
    const lux::domain::WorldObjectId second{uuids::uuid::from_string("00000000-0000-0000-0000-000000000002").value()};
    assert(first.valid());
    assert(lux::domain::WorldObjectIdLess{}(first, second));
    assert(lux::domain::WorldObjectIdHash{}(first) == std::hash<uuids::uuid>{}(first.value));
    return 0;
}
