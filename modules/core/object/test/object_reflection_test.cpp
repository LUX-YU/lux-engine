#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "object_reflection_fixture.hpp"
#include <ObjectDiagnostics.hpp>
#include <condition_variable>
#include <array>
#include <lux/engine/object/ObjectReflection.hpp>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

int main()
{
    lux::meta::ReflectionRegistry::initRegistry();
    auto &registry = lux::meta::ReflectionRegistry::instance();

    const auto *reflected = registry.findClass("lux::object::test::ReflectedObject");
    assert(reflected);
    assert(reflected->construct);
    assert(reflected->destruct);
    assert(reflected->static_fields.size() == 3);
    assert(reflected->methods.size() == 6);
    assert(reflected->type.alignment == alignof(
        lux::object::test::ReflectedObject
    ));

    const auto signal = lux::object::reflection::findSignal(registry, *reflected, "changed");
    assert(signal);
    assert(signal.ownerType() == lux::cxx::typeToken<lux::object::test::ReflectedObject>());
    assert(signal.name() == "changed");
    assert(signal.hasPayload());
    const auto saved_signal = lux::object::reflection::findSignal(registry, *reflected, "saved");
    assert(saved_signal);
    assert(!saved_signal.hasPayload());
    assert(saved_signal.name() == "saved");
    const auto move_only_signal =
        lux::object::reflection::findSignal(registry, *reflected, "moveOnly");
    assert(move_only_signal);

    const lux::meta::RefMethod *on_changed = nullptr;
    const lux::meta::RefMethod *wrong_payload = nullptr;
    const lux::meta::RefMethod *save = nullptr;
    const lux::meta::RefMethod *on_saved = nullptr;
    const lux::meta::RefMethod *on_move_only = nullptr;
    const lux::meta::RefMethod *on_throwing = nullptr;
    const lux::meta::RefMethod *unmarked_helper = nullptr;
    for (const auto &method : reflected->methods)
    {
        if (method.invokable.name == "save")
            save = &method;
        if (method.invokable.name == "onChanged")
            on_changed = &method;
        if (method.invokable.name == "onWrongPayload")
            wrong_payload = &method;
        if (method.invokable.name == "onSaved")
            on_saved = &method;
        if (method.invokable.name == "onMoveOnly")
            on_move_only = &method;
        if (method.invokable.name == "onThrowing")
            on_throwing = &method;
        if (method.invokable.name == "unmarkedPublicHelper")
            unmarked_helper = &method;
    }
    assert(save);
    assert(save->annotations().has("command"));
    assert(on_changed);
    assert(wrong_payload);
    assert(on_saved);
    assert(on_move_only);
    assert(on_throwing);
    assert(on_changed->is_noexcept);
    assert(!on_throwing->is_noexcept);
    assert(!unmarked_helper);

    const auto parameter_id = lux::cxx::type_hash<std::int32_t>();
    const std::array parameter_ids{parameter_id};
    const auto* free_noexcept = registry.findFunction(
        "lux::object::test::reflectedFreeNoexcept",
        parameter_ids
    );
    const auto* free_throwing = registry.findFunction(
        "lux::object::test::reflectedFreeThrowing",
        parameter_ids
    );
    assert(free_noexcept);
    assert(free_noexcept->is_noexcept);
    assert(free_throwing);
    assert(!free_throwing->is_noexcept);

    lux::object::ObjectMessageQueue sender_queue;
    lux::object::test::ReflectedObject sender{sender_queue.dispatcherRef()};
    lux::object::test::ReflectedObject receiver;
    auto dynamic_connection = lux::object::reflection::observe(
        sender, signal, receiver, *on_changed, lux::object::EDelivery::DIRECT);
    assert(dynamic_connection);
    sender.publish(73);
    assert(receiver.last_value == 73);

    auto throwing_connection = lux::object::reflection::observe(
        sender, signal, receiver, *on_throwing, lux::object::EDelivery::DIRECT);
    assert(!throwing_connection);
    assert(throwing_connection.error() ==
           lux::object::reflection::EDynamicObserveError::METHOD_MUST_BE_NOEXCEPT);

    auto saved_connection = lux::object::reflection::observe(
        sender, saved_signal, receiver, *on_saved, lux::object::EDelivery::DIRECT);
    assert(saved_connection);
    sender.publishSaved();
    assert(receiver.save_count == 1);

    std::mutex queued_mutex;
    std::condition_variable queued_condition;
    lux::object::test::ReflectedObject *queued_receiver = nullptr;
    bool queued_ready = false;
    bool queued_drain = false;
    int queued_value = 0;
    std::thread queued_thread([&] {
        lux::object::ObjectMessageQueue queue;
        auto instance = std::make_unique<lux::object::test::ReflectedObject>(queue.dispatcherRef());
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
    });
    {
        std::unique_lock lock{queued_mutex};
        queued_condition.wait(lock, [&] { return queued_ready; });
    }
    auto queued_connection = lux::object::reflection::observe(
        sender, signal, *queued_receiver, *on_changed, lux::object::EDelivery::AUTO);
    assert(queued_connection);
    auto direct_cross_affinity = lux::object::reflection::observe(
        sender, signal, *queued_receiver, *on_changed, lux::object::EDelivery::DIRECT);
    assert(!direct_cross_affinity);
    assert(direct_cross_affinity.error() ==
           lux::object::reflection::EDynamicObserveError::DIRECT_CROSS_AFFINITY);
    sender.publish(91);
    {
        std::scoped_lock lock{queued_mutex};
        queued_drain = true;
    }
    queued_condition.notify_all();
    queued_thread.join();
    assert(queued_value == 91);
    assert(!queued_connection->connected());

    auto incompatible = lux::object::reflection::observe(sender, signal, receiver, *wrong_payload,
                                                         lux::object::EDelivery::DIRECT);
    assert(!incompatible);
    assert(incompatible.error() ==
           lux::object::reflection::EDynamicObserveError::PARAMETER_TYPE_MISMATCH);

    {
        lux::object::test::ReflectedObject sender_without_dispatcher;
        auto missing_sender_dispatcher =
            lux::object::reflection::observe(sender_without_dispatcher, signal, receiver,
                                             *on_changed, lux::object::EDelivery::QUEUED);
        assert(!missing_sender_dispatcher);
        assert(missing_sender_dispatcher.error() ==
               lux::object::reflection::EDynamicObserveError::SENDER_HAS_NO_DISPATCHER);
    }

    {
        lux::object::test::ReflectedObject receiver_without_dispatcher;
        auto missing_receiver_dispatcher =
            lux::object::reflection::observe(sender, signal, receiver_without_dispatcher,
                                             *on_changed, lux::object::EDelivery::QUEUED);
        assert(!missing_receiver_dispatcher);
        assert(missing_receiver_dispatcher.error() ==
               lux::object::reflection::EDynamicObserveError::RECEIVER_HAS_NO_DISPATCHER);

        auto payload_not_queueable =
            lux::object::reflection::observe(sender, move_only_signal, receiver_without_dispatcher,
                                             *on_move_only, lux::object::EDelivery::QUEUED);
        assert(!payload_not_queueable);
        assert(payload_not_queueable.error() ==
               lux::object::reflection::EDynamicObserveError::PAYLOAD_NOT_QUEUEABLE);
    }

    const auto constructions_before = lux::object::test::constructions;
    const auto destructions_before = lux::object::test::destructions;
    void *storage = ::operator new(reflected->type.size);
    reflected->construct(storage);
    assert(lux::object::test::constructions == constructions_before + 1);
    reflected->destruct(storage);
    assert(lux::object::test::destructions == destructions_before + 1);
    ::operator delete(storage);

    const auto *non_default = registry.findClass("lux::object::test::NonDefaultObject");
    assert(non_default);
    assert(non_default->construct == nullptr);
    assert(non_default->destruct != nullptr);

    const auto *reflected_base = registry.findClass("lux::object::test::ReflectedBase");
    const auto *reflected_derived = registry.findClass("lux::object::test::ReflectedDerived");
    assert(reflected_base);
    assert(reflected_derived);
    const auto base_signal =
        lux::object::reflection::findSignal(registry, *reflected_base, "baseChanged");
    const auto derived_signal =
        lux::object::reflection::findSignal(registry, *reflected_derived, "derivedChanged");
    assert(base_signal);
    assert(derived_signal);
    assert(base_signal.name() == "baseChanged");
    assert(derived_signal.name() == "derivedChanged");
    const auto inherited_signal =
        lux::object::reflection::findSignal(registry, *reflected_derived, "baseChanged");
    assert(inherited_signal);
    assert(inherited_signal.name() == "baseChanged");

    auto wrong_owner = lux::object::reflection::observe(sender, base_signal, receiver, *on_changed,
                                                        lux::object::EDelivery::DIRECT);
    assert(!wrong_owner);
    assert(wrong_owner.error() ==
           lux::object::reflection::EDynamicObserveError::SIGNAL_OWNER_MISMATCH);

    {
        lux::object::test::ReflectedObject closed_sender{sender_queue.dispatcherRef()};
        lux::object::detail::ObjectDiagnosticsAccess::closeForTest(closed_sender);
        auto object_closed = lux::object::reflection::observe(
            closed_sender, signal, receiver, *on_changed, lux::object::EDelivery::DIRECT);
        assert(!object_closed);
        assert(object_closed.error() ==
               lux::object::reflection::EDynamicObserveError::OBJECT_CLOSED);
    }

    {
        lux::object::test::ReflectedObject closed_receiver;
        lux::object::detail::ObjectDiagnosticsAccess::closeForTest(closed_receiver);
        auto object_closed = lux::object::reflection::observe(
            sender, signal, closed_receiver, *on_changed, lux::object::EDelivery::DIRECT);
        assert(!object_closed);
        assert(object_closed.error() ==
               lux::object::reflection::EDynamicObserveError::OBJECT_CLOSED);
    }

    lux::meta::ReflectionRegistry::destroyRegistry();
}
