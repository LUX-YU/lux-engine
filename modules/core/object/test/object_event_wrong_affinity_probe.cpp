#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectEvent.hpp>

#include <thread>

namespace {
struct Ping final {};

class Receiver final : public lux::object::Object<Receiver> {
protected:
  void event(lux::object::EventView &) noexcept override {}
};
} // namespace

int main() {
  Receiver receiver;
  std::thread wrong_thread([&] {
    Ping ping;
    static_cast<void>(lux::object::sendEvent(receiver, ping));
  });
  wrong_thread.join();
}
