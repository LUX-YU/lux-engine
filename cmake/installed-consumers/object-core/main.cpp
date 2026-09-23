#include <lux/engine/object/Object.hpp>
#include <cassert>

struct Item final : lux::object::Object<Item> {};
int main()
{
    Item item;
    assert(item.isObjectType(lux::cxx::typeToken<Item>()));
    assert(item.isObjectType(lux::cxx::typeToken<lux::object::LuxObject>()));
    assert(item.isOnAffinityThread());
}
