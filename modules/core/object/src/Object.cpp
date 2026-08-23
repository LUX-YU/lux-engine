#include <lux/engine/object/LuxObject.hpp>

#include <algorithm>
#include <cassert>

#include <lux/engine/object/detail/ObjectState.hpp>

namespace lux::object
{
    LuxObject::LuxObject() noexcept
        : affinity_(std::this_thread::get_id())
    {
    }

    LuxObject::~LuxObject()
    {
        if (!state_) return;
        state_->control->object.store(nullptr, std::memory_order_release);
        for (auto& [key, bucket] : state_->signals)
        {
            static_cast<void>(key);
            for (auto& slot : bucket.active)
                slot->connected.store(false, std::memory_order_release);
            for (auto& slot : bucket.pending)
                slot->connected.store(false, std::memory_order_release);
        }
    }

    lux::cxx::TypeToken LuxObject::objectType() const noexcept
    {
        return lux::cxx::typeToken<LuxObject>();
    }

    bool LuxObject::isObjectType(lux::cxx::TypeToken type) const noexcept
    {
        return type == lux::cxx::typeToken<LuxObject>();
    }

    std::shared_ptr<detail::ObjectState> LuxObject::ensureState() const
    {
        if (state_) return state_;
        assert(std::this_thread::get_id() == affinity_);
        auto state = std::make_shared<detail::ObjectState>();
        state->control = std::make_shared<detail::ObjectControl>();
        state->control->object.store(
            const_cast<LuxObject*>(this),
            std::memory_order_release
        );
        state_ = std::move(state);
        return state_;
    }

    ObjectWeakRef LuxObject::weakRef() const
    {
        return ObjectWeakRef{ensureState()->control};
    }

    void LuxObject::setDispatcher(ObjectDispatcher* dispatcher)
    {
        if (dispatcher && dispatcher->ownerThread() != affinity_) std::abort();
        // A queued cross-affinity connection may be created by the sender's
        // thread. Establish this object's weak-control block while we are
        // still on its owner thread so that observing never has to mutate the
        // receiver from another affinity.
        if (dispatcher) static_cast<void>(ensureState());
        dispatcher_ = dispatcher;
    }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeErased(
        const SignalHeader& signal,
        lux::cxx::move_only_function<void(const void*)> callback,
        LuxObject* receiver,
        EDelivery delivery
    )
    {
        if (std::this_thread::get_id() != affinity_) std::abort();
        if (!isObjectType(signal.key.owner))
            return lux::cxx::unexpected(EObserveError::WRONG_SIGNAL_OWNER);

        auto state = ensureState();
        if (!state->control->object.load(std::memory_order_acquire))
            return lux::cxx::unexpected(EObserveError::OBJECT_CLOSED);

        auto resolved_delivery = delivery;
        ObjectDispatcher* receiver_dispatcher = nullptr;
        std::weak_ptr<detail::ObjectControl> receiver_control;
        if (receiver)
        {
            if (delivery == EDelivery::AUTO)
            {
                resolved_delivery = receiver->affinity() == affinity_
                    ? EDelivery::DIRECT
                    : EDelivery::QUEUED;
            }
            if (resolved_delivery == EDelivery::DIRECT
                && receiver->affinity() != affinity_)
            {
                return lux::cxx::unexpected(EObserveError::DIRECT_CROSS_AFFINITY);
            }
            if (resolved_delivery == EDelivery::QUEUED)
            {
                receiver_dispatcher = receiver->dispatcher();
                if (!receiver_dispatcher)
                    return lux::cxx::unexpected(EObserveError::RECEIVER_HAS_NO_DISPATCHER);
            }
            receiver_control = receiver->ensureState()->control;
        }

        auto slot = std::make_shared<detail::ObjectSlot>();
        slot->callback = std::move(callback);
        slot->receiver = std::move(receiver_control);
        slot->has_receiver = receiver != nullptr;
        slot->dispatcher = receiver_dispatcher;
        slot->delivery = resolved_delivery;

        auto& bucket = state->signals[std::addressof(signal)];
        if (bucket.notify_depth == 0) bucket.active.push_back(slot);
        else bucket.pending.push_back(slot);
        return Connection{slot};
    }

    void LuxObject::notifyErased(
        const SignalHeader& signal,
        const void* payload,
        PayloadClone clone_payload
    )
    {
        if (std::this_thread::get_id() != affinity_) std::abort();
        if (!isObjectType(signal.key.owner)) std::abort();
        if (!state_) return;
        auto found = state_->signals.find(std::addressof(signal));
        if (found == state_->signals.end()) return;

        auto& bucket = found->second;
        ++bucket.notify_depth;
        const auto visible_count = bucket.active.size();
        std::shared_ptr<const void> queued_payload;
        for (std::size_t index = 0; index < visible_count; ++index)
        {
            auto slot = bucket.active[index];
            if (!slot->connected.load(std::memory_order_acquire)) continue;

            if (slot->delivery == EDelivery::QUEUED)
            {
                if (!queued_payload) queued_payload = clone_payload(payload);
                auto weak_slot = std::weak_ptr<detail::ObjectSlot>{slot};
                auto receiver = slot->receiver;
                const auto status = slot->dispatcher->post(
                    [weak_slot, receiver, payload = queued_payload]
                    {
                        const auto live_slot = weak_slot.lock();
                        const auto live_receiver = receiver.lock();
                        if (!live_slot || !live_receiver) return;
                        if (!live_slot->connected.load(std::memory_order_acquire)) return;
                        if (!live_receiver->object.load(std::memory_order_acquire)) return;
                        live_slot->callback(payload.get());
                    }
                );
                if (status == EPostStatus::CLOSED)
                    slot->connected.store(false, std::memory_order_release);
            }
            else
            {
                if (slot->has_receiver)
                {
                    const auto receiver = slot->receiver.lock();
                    if (!receiver || !receiver->object.load(std::memory_order_acquire)) continue;
                }
                slot->callback(payload);
            }
        }
        --bucket.notify_depth;

        if (bucket.notify_depth == 0)
        {
            std::erase_if(bucket.active, [](const auto& slot)
            {
                return !slot->connected.load(std::memory_order_acquire);
            });
            for (auto& slot : bucket.pending)
            {
                if (slot->connected.load(std::memory_order_acquire))
                    bucket.active.push_back(std::move(slot));
            }
            bucket.pending.clear();
        }
    }

    lux::cxx::expected<Connection, EObserveError> LuxObject::observeDynamic(
        const SignalHeader& signal,
        lux::cxx::move_only_function<void(const void*)> callback,
        LuxObject& receiver,
        EDelivery delivery
    )
    {
        return observeErased(
            signal,
            std::move(callback),
            &receiver,
            delivery
        );
    }
}
