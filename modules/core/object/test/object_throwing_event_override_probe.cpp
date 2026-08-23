#include <lux/engine/object/ObjectModel.hpp>

class ThrowingEventReceiver final
    : public lux::object::Object<ThrowingEventReceiver>
{
protected:
    void event(lux::object::EventView&) override {}
};

int main() {}
