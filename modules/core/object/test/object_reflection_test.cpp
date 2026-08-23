#include "object_reflection_fixture.hpp"

#include <cassert>
#include <new>

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();

    const auto* reflected = registry.findClass("lux::object::test::ReflectedObject");
    assert(reflected);
    assert(reflected->construct);
    assert(reflected->destruct);
    assert(reflected->static_fields.size() == 1);
    assert(reflected->methods.size() >= 3);

    const auto signal = lux::object::reflection::findSignal(
        *reflected,
        "changed"
    );
    assert(signal);
    assert(signal.signal->key.owner == lux::cxx::typeToken<lux::object::test::ReflectedObject>());

    const lux::meta::RefMethod* on_changed = nullptr;
    const lux::meta::RefMethod* wrong_payload = nullptr;
    const lux::meta::RefMethod* save = nullptr;
    for (const auto& method : reflected->methods)
    {
        if (method.invokable.name == "save") save = &method;
        if (method.invokable.name == "onChanged") on_changed = &method;
        if (method.invokable.name == "onWrongPayload")
            wrong_payload = &method;
    }
    assert(save);
    assert(save->annotations().has("command"));
    assert(on_changed);
    assert(wrong_payload);

    lux::object::test::ReflectedObject sender;
    lux::object::test::ReflectedObject receiver;
    auto dynamic_connection = lux::object::reflection::observe(
        sender,
        signal,
        receiver,
        *on_changed,
        lux::object::EDelivery::DIRECT
    );
    assert(dynamic_connection);
    sender.publish(73);
    assert(receiver.last_value == 73);

    auto incompatible = lux::object::reflection::observe(
        sender,
        signal,
        receiver,
        *wrong_payload,
        lux::object::EDelivery::DIRECT
    );
    assert(!incompatible);
    assert(incompatible.error()
        == lux::object::reflection::EDynamicObserveError::PARAMETER_TYPE_MISMATCH);

    const auto constructions_before = lux::object::test::constructions;
    const auto destructions_before = lux::object::test::destructions;
    void* storage = ::operator new(reflected->type.size);
    reflected->construct(storage);
    assert(lux::object::test::constructions == constructions_before + 1);
    reflected->destruct(storage);
    assert(lux::object::test::destructions == destructions_before + 1);
    ::operator delete(storage);

    const auto* non_default = registry.findClass("lux::object::test::NonDefaultObject");
    assert(non_default);
    assert(non_default->construct == nullptr);
    assert(non_default->destruct != nullptr);

    lux::meta::ReflectionRegistry::destroyRegistry();
}
