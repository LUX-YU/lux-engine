#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <cassert>

struct Item final : lux::object::LuxObject
{
    int received{};
    lux::object::TSignal<int> changed{*this};
    void receive(int value) noexcept
    {
        assert(isDispatching());
        received += value;
    }
    static bool dispatching() noexcept { return isDispatching(); }
    lux::object::SignalDelivery publish(int value) noexcept { return emit(changed, value); }
    void event(lux::object::EventView& event) noexcept override
    {
        assert(isDispatching());
        if (auto* value = event.getIf<int>())
            received += *value;
    }
};
int main()
{
    Item item;
    assert(item.isOnAffinityThread());
    Item child;
    assert(item.addChild(child));
    assert(item.firstChild() == &child && child.parent() == &item);
    int value = 2;
    assert(!lux::object::sendEvent(child, value));
    assert(item.received == 0 && child.received == 2);
    assert(!lux::object::routeEvent(child, item, value));
    assert(item.received == 2 && child.received == 4);
    assert(!Item::dispatching());
    {
        auto connected = lux::object::LuxObject::connect(&item, &Item::changed, &child, &Item::receive);
        assert(connected && connected->connected());
        assert(item.publish(3).direct == 1 && child.received == 7);
    }
    assert(item.publish(3).direct == 0 && child.received == 7);
    assert(!Item::dispatching());
}
