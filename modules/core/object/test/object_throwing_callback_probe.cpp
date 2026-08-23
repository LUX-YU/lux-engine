#include "ObjectTestSignals.hpp"

class ThrowingReceiver final : public lux::object::Object<ThrowingReceiver> {
public:
  void receive(const int &) {}
};

int main() {
  lux::object::test::fixture::IntSender sender;
  ThrowingReceiver receiver;
  auto connection =
      sender.observe<lux::object::test::fixture::IntSender::changed,
                     &ThrowingReceiver::receive>(receiver);
  static_cast<void>(connection);
}
