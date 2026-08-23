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
    namespace
    {
        [[noreturn]] void failObjectContract() noexcept
        {
#if defined(_MSC_VER)
            __fastfail(7u);
#else
            std::abort();
#endif
        }

        class NotifyScope final
        {
        public:
            explicit NotifyScope(ObjectState& state) noexcept : state_(&state)
            {
                ++state_->active_notify_depth;
            }

            ~NotifyScope() { state_->finishNotify(); }

        private:
            ObjectState* state_;
        };
    } // namespace

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
        : object(value),
          dispatcher(std::move(dispatcher_value)),
          affinity(affinity_value)
    {}

    ObjectState::~ObjectState()
    {
        assert(object.load(std::memory_order_acquire) == nullptr);
        assert(connections.empty());
        assert(incoming.empty());
    }

    void ObjectState::append(SignalBucket& bucket, ConnectionControl& control, EDelivery delivery)
    {
        control.delivery = delivery;
        auto* value = std::addressof(control);
        if (active_notify_depth != 0)
        {
            maintenance_requested.store(true, std::memory_order_release);
            control.lane = EListenerLane::PENDING;
            control.position = bucket.pending.size();
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            const auto capacity = bucket.pending.capacity();
#endif
            bucket.pending.push_back(value);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            storage_growth_count += bucket.pending.capacity() != capacity;
#endif
        }
        else if (delivery == EDelivery::QUEUED)
        {
            control.lane = EListenerLane::QUEUED;
            control.position = bucket.queued.size();
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            const auto capacity = bucket.queued.capacity();
#endif
            bucket.queued.push_back(value);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            storage_growth_count += bucket.queued.capacity() != capacity;
#endif
        }
        else
        {
            control.lane = EListenerLane::DIRECT;
            control.position = bucket.direct.size();
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            const auto capacity = bucket.direct.capacity();
#endif
            bucket.direct.push_back(value);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            storage_growth_count += bucket.direct.capacity() != capacity;
#endif
        }
    }

    lux::cxx::intrusive_ptr<ConnectionControl> ObjectState::install(
        const SignalRuntime& signal,
        lux::cxx::intrusive_ptr<ObjectState> receiver_value,
        ObjectDispatcherRef receiver_dispatcher_value,
        ObjectInvokeThunk invoke_value,
        EDelivery delivery,
        QueuedMessageFactory queue_factory,
        std::shared_ptr<void> context_value
    )
    {
        maintain();
        ensureSignalCapacity((std::max)(
            signal.lineage_size,
            signal.index.value() + 1u
        ));

        auto control = lux::cxx::make_intrusive<ConnectionControl>();
        control->id = next_connection_id++;
        control->signal_index = signal.index.value();
        control->receiver = std::move(receiver_value);
        control->receiver_dispatcher = std::move(receiver_dispatcher_value);
        control->invoke = invoke_value;
        control->queue_factory = queue_factory;
        control->context = std::move(context_value);

        const auto id = control->id;
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        const auto connection_buckets = connections.bucket_count();
#endif
        connections.emplace(id, control);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        storage_growth_count += connections.bucket_count() != connection_buckets;
#endif
        if (control->receiver)
        {
            control->receiver->addIncoming(
                lux::cxx::intrusive_ptr<ObjectState>{this},
                control
            );
        }
        append(buckets[signal.index.value()], *control, delivery);
        return control;
    }

    void ObjectState::notify(const SignalRuntime& signal, const void* payload)
    {
        if (signal.index.value() >= buckets.size())
            return;

        const auto signal_index = signal.index.value();
        NotifyScope notify_scope{*this};
        const auto direct_count = buckets[signal_index].direct.size();
        for (std::size_t index = 0; index < direct_count; ++index)
        {
            auto* control = buckets[signal_index].direct[index];
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

        const auto queued_count = buckets[signal_index].queued.size();
        for (std::size_t index = 0; index < queued_count; ++index)
        {
            auto* control = buckets[signal_index].queued[index];
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
    }

    void ObjectState::requestDisconnect(ConnectionControl* control) noexcept
    {
        if (!control)
            return;
        control->connected.store(false, std::memory_order_release);
        maintenance_requested.store(true, std::memory_order_release);
        if (std::this_thread::get_id() == affinity)
        {
            removeConnection(control);
            return;
        }

        auto keep_alive = lux::cxx::intrusive_ptr<ObjectState>{this};
        auto keep_control = lux::cxx::intrusive_ptr<ConnectionControl>{control};
        auto message = makeObjectMessage(
            [state = std::move(keep_alive), control = std::move(keep_control)]
            { state->removeConnection(control.get()); }
        );
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

    void ObjectState::ensureSignalCapacity(std::size_t required_count)
    {
        if (buckets.size() < required_count)
        {
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            const auto capacity = buckets.capacity();
#endif
            buckets.resize(required_count);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
            storage_growth_count += buckets.capacity() != capacity;
#endif
        }
    }

    void ObjectState::removeConnection(ConnectionControl* control_value) noexcept
    {
        if (std::this_thread::get_id() != affinity || !control_value)
            return;
        const auto id = control_value->id;
        const auto found = connections.find(id);
        if (found == connections.end() || found->second.get() != control_value)
            return;
        found->second->connected.store(false, std::memory_order_release);
        if (active_notify_depth != 0)
            return;

        auto control = found->second;
        removePhysical(*control);
        connections.erase(found);
        if (control->receiver)
            control->receiver->removeIncoming(this, control.get());
    }

    void ObjectState::maintain() noexcept
    {
        if (active_notify_depth != 0)
            return;
        if (!maintenance_requested.exchange(false, std::memory_order_acq_rel))
            return;

        for (auto iterator = connections.begin(); iterator != connections.end();)
        {
            auto* control = iterator->second.get();
            const bool cancelled =
                !control->connected.load(std::memory_order_acquire);
            ++iterator;
            if (cancelled)
                removeConnection(control);
        }

        for (auto& bucket : buckets)
        {
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

    void ObjectState::finishNotify() noexcept
    {
        assert(active_notify_depth != 0);
        --active_notify_depth;
        if (active_notify_depth == 0 &&
            maintenance_requested.load(std::memory_order_acquire))
        {
            maintain();
        }
    }

    void ObjectState::addIncoming(
        lux::cxx::intrusive_ptr<ObjectState> sender,
        lux::cxx::intrusive_ptr<ConnectionControl> control
    )
    {
        std::scoped_lock lock{incoming_mutex};
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        const auto capacity = incoming.capacity();
#endif
        incoming.push_back({std::move(sender), std::move(control)});
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        storage_growth_count += incoming.capacity() != capacity;
#endif
    }

    void ObjectState::removeIncoming(
        const ObjectState* sender,
        const ConnectionControl* control
    ) noexcept
    {
        std::scoped_lock lock{incoming_mutex};
        std::erase_if(
            incoming,
            [sender, control](const IncomingLink& link)
            {
                return link.sender.get() == sender &&
                       link.control.get() == control;
            }
        );
    }

    void ObjectState::closeOwner() noexcept
    {
        object.store(nullptr, std::memory_order_release);
        if (active_notify_depth != 0)
            failObjectContract();

        std::vector<IncomingLink> incoming_copy;
        {
            std::scoped_lock lock{incoming_mutex};
            incoming_copy = std::move(incoming);
            incoming.clear();
        }
        for (auto& link : incoming_copy)
        {
            link.sender->requestDisconnect(link.control.get());
        }

        for (auto& [id, control] : connections)
        {
            static_cast<void>(id);
            control->connected.store(false, std::memory_order_release);
            if (control->receiver)
                control->receiver->removeIncoming(this, control.get());
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
        control->invoke(receiver, payload, control->context.get());
    }

    bool sendEventErased(LuxObject& target, EventView& event) noexcept
    {
#if !defined(NDEBUG) || defined(LUX_OBJECT_CONTRACT_CHECKS)
        target.assertAffinity();
#endif
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
        sender.assertAffinity();
        if (!sender.isObjectType(signal.owner))
            return lux::cxx::unexpected(EObserveError::WRONG_SIGNAL_OWNER);
        return sender.observeIndexed(
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
            detail::failObjectContract();
    }

    LuxObject::~LuxObject()
    {
#if !defined(NDEBUG) || defined(LUX_OBJECT_CONTRACT_CHECKS)
        assertAffinity();
#endif
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

    void LuxObject::assertAffinity() const noexcept
    {
        if (std::this_thread::get_id() != affinity_)
            detail::failObjectContract();
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

#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
    std::uint64_t LuxObject::storageGrowthCountForTest() const noexcept
    {
        const auto* state = state_.load(std::memory_order_acquire);
        return state ? state->storage_growth_count : 0;
    }
#endif

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeIndexed(
        const SignalRuntime& signal,
        LuxObject& receiver,
        detail::ObjectInvokeThunk invoke,
        EDelivery delivery,
        detail::QueuedMessageFactory queue_factory,
        std::shared_ptr<void> context
    )
    {
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
        auto control = sender_state->install(
            signal,
            receiver.ensureState(),
            receiver.dispatcherRef(),
            invoke,
            resolved,
            queue_factory,
            std::move(context)
        );
        return Connection{std::move(sender_state), std::move(control)};
    }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeStaticIndexed(
        const SignalRuntime& signal,
        detail::ObjectInvokeThunk invoke
    )
    {
        auto sender_state = ensureState();
        auto control =
            sender_state
                ->install(signal, {}, {}, invoke, EDelivery::DIRECT, nullptr, {});
        return Connection{std::move(sender_state), std::move(control)};
    }

    lux::cxx::expected<Connection, EObserveError>
    LuxObject::observeCallableIndexed(
        const SignalRuntime& signal,
        detail::ObjectInvokeThunk invoke,
        std::shared_ptr<void> context
    )
    {
        auto sender_state = ensureState();
        auto control = sender_state->install(
            signal,
            {},
            {},
            invoke,
            EDelivery::DIRECT,
            nullptr,
            std::move(context)
        );
        return Connection{std::move(sender_state), std::move(control)};
    }

    void LuxObject::notifyIndexed(
        const SignalRuntime& signal,
        const void* payload
    ) noexcept
    {
        auto* state = state_.load(std::memory_order_relaxed);
        if (!state)
            return;
        state->notify(signal, payload);
    }
} // namespace lux::object
