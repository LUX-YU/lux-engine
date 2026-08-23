#include <lux/engine/object/Object.hpp>

#include <thread>

namespace {
class ProbeObject final : public lux::object::Object<ProbeObject> {};
} // namespace

int main() {
  ProbeObject object;
  auto weak = object.weakRef();
  std::thread wrong_affinity([weak = std::move(weak)]() mutable {
    static_cast<void>(weak.getOnCurrent());
  });
  wrong_affinity.join();
  return 0;
}
