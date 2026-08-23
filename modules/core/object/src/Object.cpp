#include <lux/engine/object/LuxObject.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object::detail
{
    void intrusive_ptr_add_ref(ObjectState* state) noexcept
    {
        state->refs.fetch_add(1, std::memory_order_relaxed);
    }

    void intrusive_ptr_release(ObjectState* state) noexcept
    {
        if (state->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete state;
    }

    void intrusive_ptr_add_ref(ConnectionControl* control) noexcept
    {
        control->refs.fetch_add(1, std::memory_order_relaxed);
    }

    void intrusive_ptr_release(ConnectionControl* control) noexcept
    {
        if (control->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete control;
    }

    ObjectState::ObjectState(
        LuxObject* value,
        ObjectDispatcherRef dispatcher_value,
        std::thread::id affinity_value
    ) noexcept
        : object(value), dispatcher(std::move(dispatcher_value)),
          affinity(affinity_value)
    {
    }

    ObjectState::~ObjectState()
    {
        assert(object.load(std::memory_order_acquire) == nullptr);
        assert(connections.empty());
        assert(incoming.empty());
    }

    void ObjectState::append(
        SignalBucket& bucket,
        ConnectionControl& control,
        EDelivery delivery
    )
    {
        control.delivery = delivery;
        auto* value = std::addressof(control);
        if (active_notify_depth != 0)
        {
            maintenance_requested.store(true, std::memory_order_release);
            control.lane = EListenerLane::PENDING;
            control.position = bucket.pending.size();
            bucket.pending.push_back(value);
        }
        else if (delivery == EDelivery::QUEUED)
        {
            control.lane = EListenerLane::QUEUED;
            control.position = bucket.queued.size();
            bucket.queued.push_back(value);
        }
        else
        {
            control.lane = EListenerLane::DIRECT;
            control.position = bucket.direct.size();
            bucket.direct.push_back(value);
        }
    }

    InstalledConnection ObjectState::install(
        const SignalRuntime& signal,
        lux::cxx::intrusive_ptr<ObjectState> receiver_value,
        ObjectDispatcherRef receiver_dispatcher_value,
        ObjectInvokeThunk invoke_value,
        EDelivery delivery,
        QueuedMessageFactory queue_factory,
        std::shared_ptr<void> context_value
    )
    {
        if (std::this_thread::get_id() != affinity)
            std::abort();
        maintain();
        if (buckets.size() <= signal.index.value)
            buckets.resize(signal.index.value + 1);

        auto control = lux::cxx::make_intrusive<ConnectionControl>();
        control->id = next_connection_id++;
        control->signal_index = signal.index.value;
        control->receiver = std::move(receiver_value);
        control->receiver_dispatcher = std::move(receiver_dispatcher_value);
        control->invoke = invoke_value;
        control->queue_factory = queue_factory;
        control->context = std::move(context_value);

        const auto id = control->id;
        connections.emplace(id, control);
        if (control->receiver)
        {
            control->receiver
                ->addIncoming(lux::cxx::intrusive_ptr<ObjectState>{this}, control, id);
        }
        append(buckets[signal.index.value], *control, delivery);
        return {std::move(control), id};
    }

    void ObjectState::notify(const SignalRuntime& signal, const void* payload)
    {
        if (std::this_thread::get_id() != affinity)
            std::abort();
        maintain();
        if (signal.index.value >= buckets.size())
            return;

        auto& bucket = buckets[signal.index.value];
        ++active_notify_depth;
        const auto direct_count = bucket.direct.size();
        for (std::size_t index = 0; index < direct_count; ++index)
        {
            auto* control = bucket.direct[index];
            if (!control->connected.load(std::memory_order_acquire))
                continue;
            LuxObject* receiver_object = nullptr;
            if (control->receiver)
            {
                receiver_object =
                    control->receiver->object.load(std::memory_order_acquire);
                if (!receiver_object)
                    continue;
            }
            control->invoke(receiver_object, payload, control->context.get());
        }

        const auto queued_count = bucket.queued.size();
        for (std::size_t index = 0; index < queued_count; ++index)
        {
            auto* control = bucket.queued[index];
            if (!control->connected.load(std::memory_order_acquire))
                continue;
            if (!control->receiver ||
                !control->receiver->object.load(std::memory_order_acquire))
            {
                control->connected.store(false, std::memory_order_release);
                continue;
            }
            if (!control->queue_factory)
                std::abort();
            auto message = control->queue_factory(
                lux::cxx::intrusive_ptr<ConnectionControl>{control},
                payload
            );
            if (control->receiver_dispatcher.post(std::move(message)) ==
                EPostStatus::CLOSED)
            {
                control->connected.store(false, std::memory_order_release);
            }
        }
        --active_notify_depth;
        if (active_notify_depth == 0)
            maintain();
    }

    void ObjectState::requestDisconnect(
        std::uint64_t id,
        ConnectionControl* control
    ) noexcept
    {
        if (!control)
            return;
        control->connected.store(false, std::memory_order_release);
        maintenance_requested.store(true, std::memory_order_release);
        if (std::this_thread::get_id() == affinity)
        {
            removeConnection(id);
            return;
        }

        auto keep_alive = lux::cxx::intrusive_ptr<ObjectState>{this};
        auto message = makeObjectMessage([state = std::move(keep_alive), id]
                                         { state->removeConnection(id); });
        static_cast<void>(dispatcher.post(std::move(message)));
    }

    void ObjectState::removePhysical(ConnectionControl& control) noexcept
    {
        if (control.signal_index >= buckets.size())
            return;
        auto& bucket = buckets[control.signal_index];
        auto remove_at = [&control](auto& lane)
        {
            if (control.position >= lane.size())
                return;
            const auto last = lane.size() - 1;
            if (control.position != last)
            {
                lane[control.position] = lane[last];
                lane[control.position]->position = control.position;
            }
            lane.pop_back();
        };
        switch (control.lane)
        {
        case EListenerLane::DIRECT:
            remove_at(bucket.direct);
            break;
        case EListenerLane::QUEUED:
            remove_at(bucket.queued);
            break;
        case EListenerLane::PENDING:
            remove_at(bucket.pending);
            break;
        }
    }

    void ObjectState::removeConnection(std::uint64_t id) noexcept
    {
        if (std::this_thread::get_id() != affinity)
            return;
        const auto found = connections.find(id);
        if (found == connections.end())
            return;
        found->second->connected.store(false, std::memory_order_release);
        if (active_notify_depth != 0)
            return;

        auto control = found->second;
        removePhysical(*control);
        connections.erase(found);
        if (control->receiver)
            control->receiver->removeIncoming(this, id);
    }

    void ObjectState::compactBucket(SignalBucket& bucket) noexcept
    {
        static_cast<void>(bucket);
        // Physical removal is centralized in removeConnection so location
        // correction and reverse-link cleanup remain one transaction.
    }

    void ObjectState::maintain() noexcept
    {
        if (active_notify_depth != 0)
            return;
        if (!maintenance_requested.exchange(false, std::memory_order_acq_rel))
            return;

        for (auto iterator = connections.begin(); iterator != connections.end();)
        {
            const auto id = iterator->first;
            const bool cancelled =
                !iterator->second->connected.load(std::memory_order_acquire);
            ++iterator;
            if (cancelled)
                removeConnection(id);
        }

        for (auto& bucket : buckets)
        {
            compactBucket(bucket);
            auto pending = std::move(bucket.pending);
            bucket.pending.clear();
            for (auto* control : pending)
            {
                if (!control->connected.load(std::memory_order_acquire))
                    continue;
                append(bucket, *control, control->delivery);
            }
        }
    }

    void ObjectState::addIncoming(
        lux::cxx::intrusive_ptr<ObjectState> sender,
        lux::cxx::intrusive_ptr<ConnectionControl> control,
        std::uint64_t id
    )
    {
        std::scoped_lock lock{incoming_mutex};
        incoming.push_back({std::move(sender), std::move(control), id});
    }

    void
    ObjectState::removeIncoming(const ObjectState* sender, std::uint64_t id) noexcept
    {
        std::scoped_lock lock{incoming_mutex};
        std::erase_if(
            incoming,
            [sender, id](const IncomingLink& link)
            { return link.sender.get() == sender && link.connection_id == id; }
        );
    }

    void ObjectState::closeOwner() noexcept
    {
        object.store(nullptr, std::memory_order_release);
        if (active_notify_depth != 0)
        {
#if defined(_MSC_VER)
            // A synchronous sender self-destruction is an Object contract
            // violation. Fast-fail keeps the Debug probe deterministic on
            // Windows instead of entering the CRT assertion dialog.
            __fastfail(7u);
#else
            std::abort();
#endif
        }

        std::vector<IncomingLink> incoming_copy;
        {
            std::scoped_lock lock{incoming_mutex};
            incoming_copy = std::move(incoming);
            incoming.clear();
        }
        for (auto& link : incoming_copy)
        {
            link.sender->requestDisconnect(link.connection_id, link.control.get());
        }

        for (auto& [id, control] : connections)
        {
            control->connected.store(false, std::memory_order_release);
            if (control->receiver)
                control->receiver->removeIncoming(this, id);
        }
        connections.clear();
        buckets.clear();
    }

    void
    invokeQueuedConnection(ConnectionControl* control, const void* payload) noexcept
    {
        if (!control || !control->connected.load(std::memory_order_acquire) ||
            !control->receiver)
        {
            return;
        }
        auto* receiver = control->receiver->object.load(std::memory_order_acquire);
        if (!receiver)
            return;
        try
        {
            control->invoke(receiver, payload, control->context.get());
        }
        catch (...)
        {
            std::terminate();
        }
    }

    bool sendEventErased(LuxObject& target, EventView& event)
    {
        target.event(event);
        return event.accepted();
    }

    lux::cxx::expected<Connection, EObserveError> observeDynamicErased(
        LuxObject& sender,
        const SignalRuntime& signal,
        LuxObject& receiver,
        ObjectInvokeThunk invoke,
        std::shared_ptr<void> context,
        EDelivery delivery
    )
    {
        return sender.observeErased(
            signal,
            receiver,
            invoke,
            delivery,
            signal.queued_message_factory,
            std::move(context)
        );
    }
} // namespace lux::object::detail

namespace lux::object
{
    LuxObject::LuxObject(ObjectDispatcherRef dispatcher) noexcept
        : affinity_(std::this_thread::get_id()), dispatcher_(std::move(dispatcher))
    {
        if (dispatcher_ && !dispatcher_.isCurrent())
            std::abort();
    }

    LuxObject::~LuxObject()
    {
        auto* state = state_.exchange(nullptr, std::memory_order_acq_rel);
        if (!state)
            return;
        state->closeOwner();
        detail::intrusive_ptr_release(state);
    }

    lux::cxx::TypeToken LuxObject::objectType() const noexcept
    {
        return lux::cxx::typeToken<LuxObject>();
    }

    bool LuxObject::isObjectType(lux::cxx::TypeToken type) const noexcept
    {
        return type == lux::cxx::typeToken<LuxObject>();
    }

    lux::cxx::intrusive_ptr<detail::ObjectState> LuxObject::ensureState() const
    {
        auto* state = state_.load(std::memory_order_acquire);
        if (!state)
        {
            auto* candidate = new detail::ObjectState{
                const_cast<LuxObject*>(this),
                dispatcher_,
                affinity_
            };
            detail::intrusive_ptr_add_ref(candidate); // object ownership
            if (!state_.compare_exchange_strong(
                    state,
                    candidate,
                    std::memory_order_release,
                    std::memory_order_acquire
                ))
            {
                detail::intrusive_ptr_release(candidate);
            }
            else
            {
                state = candidate;
            }
        }
        return lux::cxx::intrusive_ptr<detail::ObjectState>{state};
    }

    ObjectWeakRef LuxObject::weakRef() const { return ObjectWeakRef{ensureState()}; }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeErased(
        const SignalRuntime& signal,
        LuxObject& receiver,
        detail::ObjectInvokeThunk invoke,
        EDelivery delivery,
        detail::QueuedMessageFactory queue_factory,
        std::shared_ptr<void> context
    )
    {
        if (std::this_thread::get_id() != affinity_)
            std::abort();
        if (!isObjectType(signal.owner))
            return lux::cxx::unexpected(EObserveError::WRONG_SIGNAL_OWNER);

        auto resolved = delivery;
        if (resolved == EDelivery::AUTO)
        {
            resolved = receiver.affinity() == affinity_ ? EDelivery::DIRECT
                                                        : EDelivery::QUEUED;
        }
        if (resolved == EDelivery::DIRECT && receiver.affinity() != affinity_)
        {
            return lux::cxx::unexpected(EObserveError::DIRECT_CROSS_AFFINITY);
        }
        if (resolved == EDelivery::QUEUED && !receiver.dispatcherRef())
        {
            return lux::cxx::unexpected(EObserveError::RECEIVER_HAS_NO_DISPATCHER);
        }
        if (resolved == EDelivery::QUEUED && !queue_factory)
        {
            return lux::cxx::unexpected(EObserveError::PAYLOAD_NOT_QUEUEABLE);
        }

        auto sender_state = ensureState();
        if (!sender_state->object.load(std::memory_order_acquire))
            return lux::cxx::unexpected(EObserveError::OBJECT_CLOSED);
        auto installed = sender_state->install(
            signal,
            receiver.ensureState(),
            receiver.dispatcherRef(),
            invoke,
            resolved,
            queue_factory,
            std::move(context)
        );
        return Connection{
            std::move(sender_state),
            std::move(installed.control),
            installed.id
        };
    }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeStaticErased(
        const SignalRuntime& signal,
        detail::ObjectInvokeThunk invoke
    )
    {
        if (std::this_thread::get_id() != affinity_)
            std::abort();
        if (!isObjectType(signal.owner))
            return lux::cxx::unexpected(EObserveError::WRONG_SIGNAL_OWNER);
        auto sender_state = ensureState();
        auto installed =
            sender_state
                ->install(signal, {}, {}, invoke, EDelivery::DIRECT, nullptr, {});
        return Connection{
            std::move(sender_state),
            std::move(installed.control),
            installed.id
        };
    }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeCallableErased(
        const SignalRuntime& signal,
        detail::ObjectInvokeThunk invoke,
        std::shared_ptr<void> context
    )
    {
        if (std::this_thread::get_id() != affinity_)
            std::abort();
        if (!isObjectType(signal.owner))
            return lux::cxx::unexpected(EObserveError::WRONG_SIGNAL_OWNER);
        auto sender_state = ensureState();
        auto installed = sender_state->install(
            signal,
            {},
            {},
            invoke,
            EDelivery::DIRECT,
            nullptr,
            std::move(context)
        );
        return Connection{
            std::move(sender_state),
            std::move(installed.control),
            installed.id
        };
    }

    void LuxObject::notifyErased(const SignalRuntime& signal, const void* payload)
    {
        if (std::this_thread::get_id() != affinity_)
            std::abort();
        if (!isObjectType(signal.owner))
            std::abort();
        auto* state = state_.load(std::memory_order_acquire);
        if (!state)
            return;
        state->notify(signal, payload);
    }
} // namespace lux::object
