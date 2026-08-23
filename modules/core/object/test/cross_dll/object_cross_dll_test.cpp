#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "../pinclude/ObjectDiagnostics.hpp"
#include "DerivedObject.hpp"
#include <lux/engine/object/ObjectReflection.hpp>
#include <type_traits>

int main() {
  using lux::object::cross_dll::BaseObject;
  using lux::object::cross_dll::DerivedObject;

  static_assert(!std::is_same_v<decltype(BaseObject::baseChanged),
                                decltype(DerivedObject::derivedChanged)>);
  using lux::object::detail::ObjectDiagnosticsAccess;
  assert(ObjectDiagnosticsAccess::denseIndex(BaseObject::baseChanged) == 0u);
  assert(ObjectDiagnosticsAccess::denseIndex(BaseObject::baseChangedB) == 1u);
  assert(ObjectDiagnosticsAccess::denseIndex(DerivedObject::derivedChanged) ==
         2u);
  assert(ObjectDiagnosticsAccess::denseIndex(DerivedObject::derivedChangedB) ==
         3u);
  assert(ObjectDiagnosticsAccess::lineageSize(DerivedObject::derivedChangedB) ==
         4u);

  DerivedObject sender;
  DerivedObject receiver;
  auto typed = sender.observe<BaseObject::baseChanged, &DerivedObject::receive,
                              lux::object::EDelivery::DIRECT>(receiver);
  assert(typed);
  sender.publishBase(41);
  assert(receiver.last_value == 41);

  lux::meta::ReflectionRegistry::initRegistry();
  auto &registry = lux::meta::ReflectionRegistry::instance();
  const auto *base_class =
      registry.findClass("lux::object::cross_dll::BaseObject");
  const auto *derived_class =
      registry.findClass("lux::object::cross_dll::DerivedObject");
  assert(base_class);
  assert(derived_class);
  assert(registry.findClass("lux::object::cross_dll::InternalObject"));
  const auto dynamic_signal = lux::object::reflection::findSignal(
      registry, *derived_class, "derivedChanged");
  assert(dynamic_signal);
  const auto inherited_signal = lux::object::reflection::findSignal(
      registry, *derived_class, "baseChanged");
  assert(inherited_signal);
  const auto inherited_signal_b = lux::object::reflection::findSignal(
      registry, *derived_class, "baseChangedB");
  assert(inherited_signal_b);

  const lux::meta::RefMethod *receive = nullptr;
  const lux::meta::RefMethod *receive_b = nullptr;
  for (const auto &method : derived_class->methods) {
    if (method.invokable.name == "receive")
      receive = &method;
    if (method.invokable.name == "receiveB")
      receive_b = &method;
  }
  assert(receive);
  assert(receive_b);
  auto dynamic = lux::object::reflection::observe(
      sender, dynamic_signal, receiver, *receive,
      lux::object::EDelivery::DIRECT);
  assert(dynamic);
  sender.publishDerived(73);
  assert(receiver.last_value == 73);

  auto inherited_dynamic = lux::object::reflection::observe(
      sender, inherited_signal_b, receiver, *receive_b,
      lux::object::EDelivery::DIRECT);
  assert(inherited_dynamic);
  sender.publishBaseB(109);
  assert(receiver.last_value_b == 109);

  lux::meta::ReflectionRegistry::destroyRegistry();
}
