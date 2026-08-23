#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

class ThrowingEventReceiver final
    : public lux::object::Object<ThrowingEventReceiver> {
protected:
  void event(lux::object::EventView &) override {}
};

int main() {}
