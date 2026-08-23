#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "DerivedObject.hpp"
#include <type_traits>

int main()
{
    using lux::object::cross_dll::BaseObject;
    using lux::object::cross_dll::DerivedObject;

    static_assert(!std::is_same_v<
                  decltype(BaseObject::baseChanged),
                  decltype(DerivedObject::derivedChanged)>);
    assert(BaseObject::baseChanged.index().value() == 0u);
    assert(DerivedObject::derivedChanged.index().value() == 1u);

    DerivedObject sender;
    DerivedObject receiver;
    auto typed = sender.observe<
        BaseObject::baseChanged,
        &DerivedObject::receive,
        lux::object::EDelivery::DIRECT>(receiver);
    assert(typed);
    sender.publishBase(41);
    assert(receiver.last_value == 41);

    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();
    const auto* base_class = registry.findClass("lux::object::cross_dll::BaseObject");
    const auto* derived_class =
        registry.findClass("lux::object::cross_dll::DerivedObject");
    assert(base_class);
    assert(derived_class);
    assert(registry.findClass("lux::object::cross_dll::InternalObject"));
    const auto dynamic_signal = lux::object::reflection::findSignal(
        registry,
        *derived_class,
        "derivedChanged"
    );
    assert(dynamic_signal);
    const auto inherited_signal = lux::object::reflection::findSignal(
        registry,
        *derived_class,
        "baseChanged"
    );
    assert(inherited_signal);

    const lux::meta::RefMethod* receive = nullptr;
    for (const auto& method : derived_class->methods)
    {
        if (method.invokable.name == "receive")
            receive = &method;
    }
    assert(receive);
    auto dynamic = lux::object::reflection::observe(
        sender,
        dynamic_signal,
        receiver,
        *receive,
        lux::object::EDelivery::DIRECT
    );
    assert(dynamic);
    sender.publishDerived(73);
    assert(receiver.last_value == 73);

    lux::meta::ReflectionRegistry::destroyRegistry();
}
