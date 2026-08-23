#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "object_reflection_fixture.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();

    const auto* reflected = registry.findClass("lux::object::test::ReflectedObject");
    assert(reflected);
    assert(reflected->construct);
    assert(reflected->destruct);
    assert(reflected->static_fields.size() == 2);
    assert(reflected->methods.size() >= 4);

    const auto signal = lux::object::reflection::findSignal(
        registry,
        *reflected,
        "changed"
    );
    assert(signal);
    assert(
        signal.signal->owner ==
        lux::cxx::typeToken<lux::object::test::ReflectedObject>()
    );
    assert(signal.signal->index.value() == 0u);
    const auto saved_signal = lux::object::reflection::findSignal(
        registry,
        *reflected,
        "saved"
    );
    assert(saved_signal);
    assert(!saved_signal.signal->hasPayload());
    assert(saved_signal.signal->index.value() == 1u);

    const lux::meta::RefMethod* on_changed = nullptr;
    const lux::meta::RefMethod* wrong_payload = nullptr;
    const lux::meta::RefMethod* save = nullptr;
    const lux::meta::RefMethod* on_saved = nullptr;
    for (const auto& method : reflected->methods)
    {
        if (method.invokable.name == "save")
            save = &method;
        if (method.invokable.name == "onChanged")
            on_changed = &method;
        if (method.invokable.name == "onWrongPayload")
            wrong_payload = &method;
        if (method.invokable.name == "onSaved")
            on_saved = &method;
    }
    assert(save);
    assert(save->annotations().has("command"));
    assert(on_changed);
    assert(wrong_payload);
    assert(on_saved);

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

    auto saved_connection = lux::object::reflection::observe(
        sender,
        saved_signal,
        receiver,
        *on_saved,
        lux::object::EDelivery::DIRECT
    );
    assert(saved_connection);
    sender.publishSaved();
    assert(receiver.save_count == 1);

    std::mutex queued_mutex;
    std::condition_variable queued_condition;
    lux::object::test::ReflectedObject* queued_receiver = nullptr;
    bool queued_ready = false;
    bool queued_drain = false;
    int queued_value = 0;
    std::thread queued_thread(
        [&]
        {
            lux::object::ObjectMessageQueue queue;
            auto instance = std::make_unique<lux::object::test::ReflectedObject>(
                queue.dispatcherRef()
            );
            {
                std::scoped_lock lock{queued_mutex};
                queued_receiver = instance.get();
                queued_ready = true;
            }
            queued_condition.notify_all();
            {
                std::unique_lock lock{queued_mutex};
                queued_condition.wait(lock, [&] { return queued_drain; });
            }
            assert(queue.dispatchPending() == 1);
            queued_value = instance->last_value;
        }
    );
    {
        std::unique_lock lock{queued_mutex};
        queued_condition.wait(lock, [&] { return queued_ready; });
    }
    auto queued_connection = lux::object::reflection::observe(
        sender,
        signal,
        *queued_receiver,
        *on_changed,
        lux::object::EDelivery::AUTO
    );
    assert(queued_connection);
    sender.publish(91);
    {
        std::scoped_lock lock{queued_mutex};
        queued_drain = true;
    }
    queued_condition.notify_all();
    queued_thread.join();
    assert(queued_value == 91);
    assert(!queued_connection->connected());

    auto incompatible = lux::object::reflection::observe(
        sender,
        signal,
        receiver,
        *wrong_payload,
        lux::object::EDelivery::DIRECT
    );
    assert(!incompatible);
    assert(
        incompatible.error() ==
        lux::object::reflection::EDynamicObserveError::PARAMETER_TYPE_MISMATCH
    );

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

    const auto* reflected_base = registry.findClass("lux::object::test::ReflectedBase");
    const auto* reflected_derived =
        registry.findClass("lux::object::test::ReflectedDerived");
    assert(reflected_base);
    assert(reflected_derived);
    const auto base_signal = lux::object::reflection::findSignal(
        registry,
        *reflected_base,
        "baseChanged"
    );
    const auto derived_signal = lux::object::reflection::findSignal(
        registry,
        *reflected_derived,
        "derivedChanged"
    );
    assert(base_signal);
    assert(derived_signal);
    assert(base_signal.signal->index.value() == 0u);
    assert(derived_signal.signal->index.value() == 1u);
    const auto inherited_signal = lux::object::reflection::findSignal(
        registry,
        *reflected_derived,
        "baseChanged"
    );
    assert(inherited_signal);
    assert(inherited_signal.signal->index.value() == 0u);

    lux::meta::ReflectionRegistry::destroyRegistry();
}
