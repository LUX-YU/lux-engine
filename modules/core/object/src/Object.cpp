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
            explicit NotifyScope(ObjectState &state) noexcept : state_(&state)
            {
                ++state_->active_notify_depth;
            }

            ~NotifyScope()
            {
                state_->finishNotify();
            }

          private:
            ObjectState *state_;
        };
    } // namespace

    void intrusive_ptr_add_ref(ObjectState *state) noexcept
    {
        state->refs.fetch_add(1, std::memory_order_relaxed);
    }

    void intrusive_ptr_release(ObjectState *state) noexcept
    {
        if (state->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete state;
    }

    void intrusive_ptr_add_ref(ConnectionControl *control) noexcept
    {
        control->refs.fetch_add(1, std::memory_order_relaxed);
    }

    void intrusive_ptr_release(ConnectionControl *control) noexcept
    {
        if (control->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete control;
    }

    ObjectState::ObjectState(LuxObject *value, ObjectDispatcherRef dispatcher_value,
                             std::thread::id affinity_value) noexcept
        : object(value), dispatcher(std::move(dispatcher_value)), affinity(affinity_value)
    {
    }

    ObjectState::~ObjectState()
    {
        assert(object.load(std::memory_order_acquire) == nullptr);
        assert(owned_connections.empty());
        assert(incoming.empty());
    }

    void ObjectState::append(SignalBucket &bucket, ConnectionControl &control, EDelivery delivery)
    {
        control.delivery = delivery;
        auto *value = std::addressof(control);
        if (active_notify_depth != 0)
        {
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
        const SignalDescriptor &signal, lux::cxx::intrusive_ptr<ObjectState> receiver_value,
        ObjectInvokeThunk invoke_value, EDelivery delivery, std::shared_ptr<void> context_value)
    {
        ensureSignalCapacity((std::max)(signal.lineage_size_, signal.dense_index_ + 1u));

        auto control = lux::cxx::make_intrusive<ConnectionControl>();
        control->owner_position = owned_connections.size();
        control->signal_index = signal.dense_index_;
        control->receiver = std::move(receiver_value);
        control->invoke = invoke_value;
        control->context = std::move(context_value);

#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        const auto connection_capacity = owned_connections.capacity();
#endif
        owned_connections.push_back(control);
#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
        storage_growth_count += owned_connections.capacity() != connection_capacity;
#endif
        if (control->receiver)
        {
            control->receiver->addIncoming(lux::cxx::intrusive_ptr<ObjectState>{this}, control);
        }
        append(buckets[signal.dense_index_], *control, delivery);
        return control;
    }

    void ObjectState::notify(const SignalDescriptor &signal, const void *payload)
    {
        if (signal.dense_index_ >= buckets.size())
            return;

        const auto signal_index = signal.dense_index_;
        NotifyScope notify_scope{*this};
        const auto direct_count = buckets[signal_index].direct.size();
        for (std::size_t index = 0; index < direct_count; ++index)
        {
            auto *control = buckets[signal_index].direct[index];
            if (!control->connected.load(std::memory_order_acquire))
                continue;
            LuxObject *receiver_object = nullptr;
            if (control->receiver)
            {
                receiver_object = control->receiver->object.load(std::memory_order_acquire);
                if (!receiver_object)
                {
                    requestDisconnect(control);
                    continue;
                }
            }
            control->invoke(receiver_object, payload, control->context.get());
        }

        const auto queued_count = buckets[signal_index].queued.size();
        for (std::size_t index = 0; index < queued_count; ++index)
        {
            auto *control = buckets[signal_index].queued[index];
            if (!control->connected.load(std::memory_order_acquire))
                continue;
            if (!control->receiver || !control->receiver->object.load(std::memory_order_acquire))
            {
                requestDisconnect(control);
                continue;
            }
            if (!signal.queued_message_factory_)
                std::abort();
            auto message = signal.queued_message_factory_(
                lux::cxx::intrusive_ptr<ConnectionControl>{control}, payload);
            if (post(control->receiver->dispatcher, std::move(message)) == EPostStatus::CLOSED)
            {
                requestDisconnect(control);
            }
        }
    }

    void ObjectState::requestDisconnect(ConnectionControl *control) noexcept
    {
        if (!control)
            return;
        if (!control->connected.exchange(false, std::memory_order_acq_rel))
            return;
        if (std::this_thread::get_id() == affinity)
        {
            if (active_notify_depth != 0)
                pending_removals.push_back(control);
            else
                removeConnection(control);
            return;
        }

        auto keep_alive = lux::cxx::intrusive_ptr<ObjectState>{this};
        auto keep_control = lux::cxx::intrusive_ptr<ConnectionControl>{control};
        auto message =
            makeMessage([state = std::move(keep_alive), control = std::move(keep_control)] {
                state->removeConnection(control.get());
            });
        if (post(dispatcher, std::move(message)) == EPostStatus::CLOSED)
            failObjectContract();
    }

    void ObjectState::removePhysical(ConnectionControl &control) noexcept
    {
        if (control.signal_index >= buckets.size())
            return;
        auto &bucket = buckets[control.signal_index];
        auto remove_at = [&control](auto &lane) {
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

    void ObjectState::removeConnection(ConnectionControl *control_value) noexcept
    {
        if (std::this_thread::get_id() != affinity || !control_value)
            return;
        const auto owner_position = control_value->owner_position;
        if (owner_position >= owned_connections.size() ||
            owned_connections[owner_position].get() != control_value)
        {
            return;
        }
        control_value->connected.store(false, std::memory_order_release);
        if (active_notify_depth != 0)
        {
            pending_removals.push_back(control_value);
            return;
        }

        auto control = owned_connections[owner_position];
        removePhysical(*control);
        if (control->receiver)
            control->receiver->removeIncoming(this, control.get());

        const auto last = owned_connections.size() - 1u;
        if (owner_position != last)
        {
            owned_connections[owner_position] = std::move(owned_connections[last]);
            owned_connections[owner_position]->owner_position = owner_position;
        }
        owned_connections.pop_back();
    }

    void ObjectState::maintainAfterNotify() noexcept
    {
        if (active_notify_depth != 0)
            return;

        const auto removal_count = pending_removals.size();
        for (std::size_t index = 0; index < removal_count; ++index)
            removeConnection(pending_removals[index]);
        pending_removals.clear();

        for (auto &bucket : buckets)
        {
            const auto pending_count = bucket.pending.size();
            for (std::size_t index = 0; index < pending_count; ++index)
            {
                auto *control = bucket.pending[index];
                if (!control->connected.load(std::memory_order_acquire))
                    continue;
                append(bucket, *control, control->delivery);
            }
            bucket.pending.clear();
        }
    }

    void ObjectState::finishNotify() noexcept
    {
        assert(active_notify_depth != 0);
        --active_notify_depth;
        if (active_notify_depth == 0)
            maintainAfterNotify();
    }

    void ObjectState::addIncoming(lux::cxx::intrusive_ptr<ObjectState> sender,
                                  lux::cxx::intrusive_ptr<ConnectionControl> control)
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

    void ObjectState::removeIncoming(const ObjectState *sender,
                                     const ConnectionControl *control) noexcept
    {
        std::scoped_lock lock{incoming_mutex};
        std::erase_if(incoming, [sender, control](const IncomingLink &link) {
            return link.sender.get() == sender && link.control.get() == control;
        });
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
        for (auto &link : incoming_copy)
        {
            link.sender->requestDisconnect(link.control.get());
        }

        for (auto &control : owned_connections)
        {
            control->connected.store(false, std::memory_order_release);
            if (control->receiver)
                control->receiver->removeIncoming(this, control.get());
        }
        owned_connections.clear();
        pending_removals.clear();
        buckets.clear();
    }

    void invokeQueuedConnection(ConnectionControl *control, const void *payload) noexcept
    {
        if (!control || !control->connected.load(std::memory_order_acquire) || !control->receiver)
        {
            return;
        }
        auto *receiver = control->receiver->object.load(std::memory_order_acquire);
        if (!receiver)
            return;
        control->invoke(receiver, payload, control->context.get());
    }

    bool sendEventErased(LuxObject &target, EventView &event) noexcept
    {
#if !defined(NDEBUG) || defined(LUX_OBJECT_CONTRACT_CHECKS)
        target.assertAffinity();
#endif
        target.event(event);
        return event.accepted();
    }

    lux::cxx::expected<Connection, EObserveError> observeDynamicErased(
        LuxObject &sender, const SignalDescriptor &signal, LuxObject &receiver,
        ObjectInvokeThunk invoke, std::shared_ptr<void> context, EDelivery delivery)
    {
        sender.assertAffinity();
        return sender.observeIndexed(signal, receiver, invoke, delivery, std::move(context));
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
        auto *state = state_.exchange(nullptr, std::memory_order_acq_rel);
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

    bool LuxObject::isOnAffinityThread() const noexcept
    {
        return std::this_thread::get_id() == affinity_;
    }

    void LuxObject::assertAffinity() const noexcept
    {
        if (std::this_thread::get_id() != affinity_)
            detail::failObjectContract();
    }

    lux::cxx::intrusive_ptr<detail::ObjectState> LuxObject::ensureState() const
    {
        auto *state = state_.load(std::memory_order_acquire);
        if (!state)
        {
            auto *candidate =
                new detail::ObjectState{const_cast<LuxObject *>(this), dispatcher_, affinity_};
            detail::intrusive_ptr_add_ref(candidate); // object ownership
            if (!state_.compare_exchange_strong(state, candidate, std::memory_order_release,
                                                std::memory_order_acquire))
            {
                candidate->object.store(nullptr, std::memory_order_release);
                detail::intrusive_ptr_release(candidate);
            }
            else
            {
                state = candidate;
            }
        }
        return lux::cxx::intrusive_ptr<detail::ObjectState>{state};
    }

    ObjectWeakRef LuxObject::weakRef() const
    {
        return ObjectWeakRef{ensureState()};
    }

#if defined(LUX_OBJECT_TEST_DIAGNOSTICS)
    void LuxObject::closeForTest() noexcept
    {
        ensureState()->object.store(nullptr, std::memory_order_release);
    }

    std::uint64_t LuxObject::storageGrowthCountForTest() const noexcept
    {
        const auto *state = state_.load(std::memory_order_acquire);
        return state ? state->storage_growth_count : 0;
    }

    std::size_t LuxObject::ownedConnectionCountForTest() const noexcept
    {
        const auto *state = state_.load(std::memory_order_acquire);
        return state ? state->owned_connections.size() : 0;
    }

    std::size_t LuxObject::incomingConnectionCountForTest() const noexcept
    {
        auto *state = state_.load(std::memory_order_acquire);
        if (!state)
            return 0;
        std::scoped_lock lock{state->incoming_mutex};
        return state->incoming.size();
    }
#endif

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeIndexed(
        const detail::SignalDescriptor &signal, LuxObject &receiver,
        detail::ObjectInvokeThunk invoke, EDelivery delivery, std::shared_ptr<void> context)
    {
        auto resolved = delivery;
        if (resolved == EDelivery::AUTO)
        {
            resolved = receiver.affinity_ == affinity_ ? EDelivery::DIRECT : EDelivery::QUEUED;
        }
        if (resolved == EDelivery::DIRECT && receiver.affinity_ != affinity_)
        {
            return lux::cxx::unexpected(EObserveError::DIRECT_CROSS_AFFINITY);
        }
        if (resolved == EDelivery::QUEUED && !signal.queued_message_factory_)
        {
            return lux::cxx::unexpected(EObserveError::PAYLOAD_NOT_QUEUEABLE);
        }
        if (resolved == EDelivery::QUEUED && !dispatcher_)
        {
            return lux::cxx::unexpected(EObserveError::SENDER_HAS_NO_DISPATCHER);
        }
        if (resolved == EDelivery::QUEUED && !receiver.dispatcherRef())
        {
            return lux::cxx::unexpected(EObserveError::RECEIVER_HAS_NO_DISPATCHER);
        }
        auto sender_state = ensureState();
        if (!sender_state->object.load(std::memory_order_acquire))
            return lux::cxx::unexpected(EObserveError::OBJECT_CLOSED);
        auto receiver_state = receiver.ensureState();
        if (!receiver_state->object.load(std::memory_order_acquire))
            return lux::cxx::unexpected(EObserveError::OBJECT_CLOSED);
        auto control = sender_state->install(signal, std::move(receiver_state), invoke, resolved,
                                             std::move(context));
        return Connection{std::move(sender_state), std::move(control)};
    }

    Connection LuxObject::observeStaticIndexed(const detail::SignalDescriptor &signal,
                                               detail::ObjectInvokeThunk invoke)
    {
        auto sender_state = ensureState();
        auto control = sender_state->install(signal, {}, invoke, EDelivery::DIRECT, {});
        return Connection{std::move(sender_state), std::move(control)};
    }

    Connection LuxObject::observeCallableIndexed(const detail::SignalDescriptor &signal,
                                                 detail::ObjectInvokeThunk invoke,
                                                 std::shared_ptr<void> context)
    {
        auto sender_state = ensureState();
        auto control =
            sender_state->install(signal, {}, invoke, EDelivery::DIRECT, std::move(context));
        return Connection{std::move(sender_state), std::move(control)};
    }

    void LuxObject::notifyIndexed(const detail::SignalDescriptor &signal,
                                  const void *payload) noexcept
    {
        auto *state = state_.load(std::memory_order_relaxed);
        if (!state)
            return;
        state->notify(signal, payload);
    }
} // namespace lux::object
