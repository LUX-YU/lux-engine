#include <lux/engine/object/LuxObject.hpp>
#include <exception>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/object/detail/ObjectState.hpp>
#include <cstdlib>
#include <vector>
#include <stdexcept>

namespace
{
    thread_local std::size_t dispatch_depth{};

    struct DispatchScope final
    {
        DispatchScope() noexcept
        {
            ++dispatch_depth;
        }
        ~DispatchScope()
        {
            --dispatch_depth;
        }
    };
}

namespace lux::object::detail
{
    [[noreturn]] void failObjectContract() noexcept
    {
        std::abort();
    }

    void intrusive_ptr_add_ref(ObjectState* value) noexcept
    {
        value->refs.fetch_add(1, std::memory_order_relaxed);
    }
    void intrusive_ptr_release(ObjectState* value) noexcept
    {
        if (value->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete value;
    }
    void intrusive_ptr_add_ref(SignalStorage* value) noexcept
    {
        value->refs.fetch_add(1, std::memory_order_relaxed);
    }
    void intrusive_ptr_release(SignalStorage* value) noexcept
    {
        if (value->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete value;
    }
    void intrusive_ptr_add_ref(ConnectionControl* value) noexcept
    {
        value->refs.fetch_add(1, std::memory_order_relaxed);
    }
    void intrusive_ptr_release(ConnectionControl* value) noexcept
    {
        if (value->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete value;
    }

    bool ObjectState::addIncoming(ConnectionControl& control) noexcept
    {
        std::scoped_lock lock{mutex};
        if (!object.load(std::memory_order_acquire))
            return false;
        intrusive_ptr_add_ref(&control);
        control.next_incoming = incoming;
        if (incoming)
            incoming->previous_incoming = &control;
        incoming = &control;
        control.incoming_linked = true;
        return true;
    }

    void ObjectState::removeIncoming(ConnectionControl& control) noexcept
    {
        {
            std::scoped_lock lock{mutex};
            if (!control.incoming_linked)
                return;
            if (control.previous_incoming)
                control.previous_incoming->next_incoming = control.next_incoming;
            else
                incoming = control.next_incoming;
            if (control.next_incoming)
                control.next_incoming->previous_incoming = control.previous_incoming;
            control.previous_incoming = control.next_incoming = nullptr;
            control.incoming_linked = false;
        }
        intrusive_ptr_release(&control); // Never destroy a user callable under the endpoint mutex.
    }

    void ObjectState::closeOwner() noexcept
    {
        ConnectionControl* batch{};
        {
            std::scoped_lock lock{mutex};
            object.store(nullptr, std::memory_order_release);
            batch = std::exchange(incoming, nullptr);
            for (auto* control = batch; control; control = control->next_incoming)
                control->incoming_linked = false;
        }
        while (batch)
        {
            lux::cxx::intrusive_ptr<ConnectionControl> control{batch, false}; // Adopt the incoming list reference.
            batch = control->next_incoming;
            control->previous_incoming = control->next_incoming = nullptr;
            control->storage->cancel(*control);
        }
    }

    void SignalStorage::cancel(ConnectionControl& control) noexcept
    {
        if (!control.connected.exchange(false, std::memory_order_acq_rel))
            return;
        {
            std::scoped_lock lock{cancel_mutex};
            if (closed.load(std::memory_order_acquire))
                return;
            intrusive_ptr_add_ref(&control);
            control.next_cancelled = cancelled;
            cancelled = &control;
        }
        if (ObjectRuntime::instance().isCurrent() && depth == 0 && !maintaining)
            maintain();
        else
            scheduleSignalMaintenance(*this);
    }

    void SignalStorage::remove(ConnectionControl& control) noexcept
    {
        auto* found = records.find(control.key);
        if (!found || found->get() != &control)
            return;
        auto held = *found;
        if (control.receiver)
            control.receiver->removeIncoming(control);
        records.erase(control.key);
    }

    void SignalStorage::maintain() noexcept
    {
        if (!ObjectRuntime::instance().isCurrent() || depth || maintaining)
            return;
        maintaining = true;
        ConnectionControl* batch{};
        {
            std::scoped_lock lock{cancel_mutex};
            batch = std::exchange(cancelled, nullptr);
        }
        while (batch)
        {
            lux::cxx::intrusive_ptr<ConnectionControl> control{batch, false};
            batch = control->next_cancelled;
            control->next_cancelled = nullptr;
            remove(*control);
        }
        maintaining = false;
    }

    void SignalStorage::close() noexcept
    {
        if (!ObjectRuntime::instance().isCurrent() || depth)
            failObjectContract();
        {
            std::scoped_lock lock{cancel_mutex};
            if (closed.exchange(true, std::memory_order_acq_rel))
                return;
        }
        while (!records.empty())
        {
            auto control = *records.begin();
            control->connected.store(false, std::memory_order_release);
            remove(*control);
        }
        maintain();
    }

    void closeSignal(SignalStorage* storage) noexcept
    {
        if (storage)
            storage->close();
    }

    void invokeConnection(ConnectionControl* control, const void* payload) noexcept
    {
        // Acquiring this observed live state admits the invocation. Disconnect is not join.
        if (!control->connected.load(std::memory_order_acquire))
            return;
        LuxObject* receiver{};
        if (control->receiver)
        {
            receiver = control->receiver->object.load(std::memory_order_acquire);
            if (!receiver)
                return;
            if (!receiver->isOnAffinityThread())
                failObjectContract();
            if (!receiver->acceptsCallbacks())
                return;
        }
        if (receiver)
            ++receiver->active_events_;
        const DispatchScope dispatch;
        control->callback(receiver, payload);
        if (receiver)
            --receiver->active_events_;
    }

    SignalDelivery SignalStorage::emit(const void* payload) noexcept
    {
        SignalDelivery result;
        if (closed.load(std::memory_order_acquire))
            return result;
        if (depth == 0)
        {
            maintain();
            visible_count = records.size();
        }
        ++depth;
        auto iterator = records.begin();
        for (std::size_t index{}; index < visible_count; ++index, ++iterator)
        {
            // A local reference also protects the callable against cancellation during value copying.
            auto control = *iterator;
            if (!control->connected.load(std::memory_order_acquire))
                continue;
            if (control->receiver && !control->receiver->object.load(std::memory_order_acquire))
            {
                cancel(*control);
                continue;
            }
            if (control->delivery == EDelivery::DIRECT)
            {
                invokeConnection(control.get(), payload);
                ++result.direct;
                continue;
            }
            auto message = queue_factory(control, payload);
            switch (post(std::move(message)))
            {
            case EPostStatus::POSTED:
                ++result.queued;
                break;
            case EPostStatus::FULL:
                ++result.full;
                break;
            case EPostStatus::CLOSED:
                ++result.closed;
                cancel(*control);
                break;
            }
        }
        if (--depth == 0)
            maintain();
        return result;
    }

    bool sendEventErased(LuxObject& target, EventView& event) noexcept
    {
        target.assertAffinity();
        if (!target.acceptsCallbacks())
            return false;
        const DispatchScope dispatch;
        ++target.active_events_;
        if (!event.accepted())
            target.event(event);
        --target.active_events_;
        return event.accepted();
    }

    bool routeEventErased(LuxObject& target, LuxObject& boundary, EventView& event) noexcept
    {
        target.assertAffinity();
        boundary.assertAffinity();
        if (!target.acceptsCallbacks())
            return false;
        const DispatchScope dispatch;
        auto* ancestor = &target;
        while (ancestor && ancestor != &boundary)
            ancestor = ancestor->parent_;
        if (!ancestor)
            failObjectContract();

        // No callbacks run until every borrowed route object is protected.
        for (auto* object = &target;; object = object->parent_)
        {
            ++object->active_events_;
            if (object == &boundary)
                break;
        }
        target.filterAncestors(target, boundary, event);
        for (auto* object = &target; !event.accepted(); object = object->parent_)
        {
            object->event(event);
            if (object == &boundary)
                break;
        }
        for (auto* object = &target;; object = object->parent_)
        {
            --object->active_events_;
            if (object == &boundary)
                break;
        }
        return event.accepted();
    }

}

namespace lux::object
{
    bool LuxObject::hasActiveTree() const noexcept
    {
        auto* node = this;
        for (;;)
        {
            const bool is_active = node->active_events_ || node->callback_borrows_ || node->changing_children_;
            if (is_active)
                return true;
            if (node->first_child_)
                node = node->first_child_;
            else
            {
                while (node != this && !node->next_sibling_)
                    node = node->parent_;
                if (node == this)
                    return false;
                node = node->next_sibling_;
            }
        }
    }

    bool LuxObject::acceptsCallbacks() const noexcept
    {
        for (auto* object = this; object; object = object->parent_)
        {
            const bool is_unavailable = object->closing_ || object->changing_children_;
            if (is_unavailable)
                return false;
        }
        return true;
    }

    ObjectResult<void> LuxObject::validateRelation(LuxObject& child, LuxObject* parent) noexcept
    {
        if (!ObjectRuntime::instance().isCurrent())
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        const bool is_closed = child.closing_ || (parent && parent->closing_);
        if (is_closed)
            return cxx::unexpected(EObjectTreeError::CLOSED);
        if (child.parent_ == parent)
            return {};
        const bool is_busy = isDispatching() || child.hasActiveTree() || !child.acceptsCallbacks() ||
            (parent && !parent->acceptsCallbacks());
        if (is_busy)
            return cxx::unexpected(EObjectTreeError::BUSY);
        for (auto* ancestor = parent; ancestor; ancestor = ancestor->parent_)
        {
            if (ancestor == &child)
                return cxx::unexpected(EObjectTreeError::INVALID_TREE);
            if (ancestor->active_events_)
                return cxx::unexpected(EObjectTreeError::BUSY);
        }
        for (auto* ancestor = child.parent_; ancestor; ancestor = ancestor->parent_)
            if (ancestor->active_events_)
                return cxx::unexpected(EObjectTreeError::BUSY);
        return {};
    }

    void LuxObject::commitRelation(LuxObject& child, LuxObject* parent) noexcept
    {
        if (child.parent_ == parent)
            return;
        child.unlinkParent();
        if (parent)
            parent->linkChild(child);
    }

    ObjectResult<void> LuxObject::setParent(LuxObject* parent) noexcept
    {
        if (!ObjectRuntime::instance().isCurrent())
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        const bool invalid_topology = !allowsGenericStructure() ||
            (parent && !parent->allowsGenericStructure()) || (parent_ && !parent_->allowsGenericStructure());
        if (invalid_topology)
            return cxx::unexpected(EObjectTreeError::INVALID_TREE);
        auto valid = validateRelation(*this, parent);
        if (!valid)
            return valid;
        commitRelation(*this, parent);
        return {};
    }

    ObjectResult<void> LuxObject::addChild(LuxObject& child) noexcept
    {
        return child.setParent(this);
    }

    ObjectResult<void> LuxObject::removeChild(LuxObject& child) noexcept
    {
        if (!ObjectRuntime::instance().isCurrent())
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        if (child.parent_ != this)
            return cxx::unexpected(EObjectTreeError::INVALID_TREE);
        return child.setParent(nullptr);
    }

    void LuxObject::linkChild(LuxObject& child) noexcept
    {
        child.parent_ = this;
        child.previous_sibling_ = last_child_;
        if (last_child_)
            last_child_->next_sibling_ = &child;
        else
            first_child_ = &child;
        last_child_ = &child;
    }

    void LuxObject::clearChildren() noexcept
    {
        assertAffinity();
        if (hasActiveTree())
            detail::failObjectContract();
        while (last_child_)
            last_child_->unlinkParent();
    }

    LuxObject::LuxObject() noexcept : id_(ObjectRuntime::instance().registerObject(*this)) {}

    LuxObject::LuxObject(LuxObject* parent) noexcept
        : LuxObject()
    {
        if (!setParent(parent))
            detail::failObjectContract();
    }

    void LuxObject::unlinkParent() noexcept
    {
        if (parent_)
        {
            if (previous_sibling_)
                previous_sibling_->next_sibling_ = next_sibling_;
            else
                parent_->first_child_ = next_sibling_;
            if (next_sibling_)
                next_sibling_->previous_sibling_ = previous_sibling_;
            else
                parent_->last_child_ = previous_sibling_;
        }
        parent_ = previous_sibling_ = next_sibling_ = nullptr;
    }

    void LuxObject::beginDestruction() noexcept
    {
        assertAffinity();
        const bool is_active = hasActiveTree() || isDispatching();
        if (is_active)
            detail::failObjectContract();
        closing_ = true;
        ObjectRuntime::instance().unregisterObject(id_);
        if (auto* state = state_.load(std::memory_order_acquire))
            state->closeOwner();
    }

    LuxObject::~LuxObject()
    {
        assertAffinity();
        const bool is_illegal_destruction = active_events_ != 0 || callback_borrows_ != 0;
        if (is_illegal_destruction)
            detail::failObjectContract();
        closing_ = true;
        ObjectRuntime::instance().unregisterObject(id_);
        clearChildren();
        unlinkParent();
        auto* state = state_.exchange(nullptr, std::memory_order_acq_rel);
        if (!state)
            return;
        state->closeOwner();
        detail::intrusive_ptr_release(state);
    }

    void LuxObject::beginCallbackBorrow(LuxObject& value) noexcept
    {
        value.assertAffinity();
        ++value.callback_borrows_;
    }

    void LuxObject::endCallbackBorrow(LuxObject& value) noexcept
    {
        value.assertAffinity();
        if (!value.callback_borrows_)
            detail::failObjectContract();
        --value.callback_borrows_;
    }

    void LuxObject::beginTreeVisit() noexcept
    {
        assertAffinity();
        auto* node = this;
        for (;;)
        {
            ++node->active_events_;
            if (node->first_child_)
                node = node->first_child_;
            else
            {
                while (node != this && !node->next_sibling_)
                    node = node->parent_;
                if (node == this)
                    return;
                node = node->next_sibling_;
            }
        }
    }

    void LuxObject::endTreeVisit() noexcept
    {
        assertAffinity();
        auto* node = this;
        for (;;)
        {
            if (!node->active_events_)
                detail::failObjectContract();
            --node->active_events_;
            if (node->first_child_)
                node = node->first_child_;
            else
            {
                while (node != this && !node->next_sibling_)
                    node = node->parent_;
                if (node == this)
                    return;
                node = node->next_sibling_;
            }
        }
    }

    void LuxObject::filterAncestors(LuxObject& target, LuxObject& boundary, EventView& event) noexcept
    {
        if (this != &boundary)
            parent_->filterAncestors(target, boundary, event);
        if (this != &target && !event.accepted())
            filterEvent(target, event);
    }

    bool LuxObject::isOnAffinityThread() const noexcept
    {
        return ObjectRuntime::instance().isCurrent();
    }

    void LuxObject::assertAffinity() const noexcept
    {
        if (!ObjectRuntime::instance().isCurrent())
            detail::failObjectContract();
    }

    lux::cxx::intrusive_ptr<detail::ObjectState> LuxObject::ensureState() const
    {
        auto* state = state_.load(std::memory_order_acquire);
        if (!state)
        {
            auto* candidate = new detail::ObjectState{const_cast<LuxObject*>(this), id_};
            detail::intrusive_ptr_add_ref(candidate); // object ownership
            if (!state_.compare_exchange_strong(state, candidate, std::memory_order_release, std::memory_order_acquire))
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

    LuxObject::ConnectResult LuxObject::connectSignal(
        lux::cxx::intrusive_ptr<detail::SignalStorage>& storage,
        LuxObject* receiver,
        detail::QueuedMessageFactory factory,
        EDelivery delivery,
        detail::SignalCallback callback
    ) noexcept
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EConnectError::WRONG_THREAD);
        const bool is_closed = !acceptsCallbacks() ||
            (receiver && receiver->isOnAffinityThread() && !receiver->acceptsCallbacks());
        if (is_closed)
            return lux::cxx::unexpected(EConnectError::OBJECT_CLOSED);
        if (delivery == EDelivery::AUTO)
            delivery = EDelivery::DIRECT;
        if (delivery == EDelivery::QUEUED && !factory)
            return lux::cxx::unexpected(EConnectError::PAYLOAD_NOT_QUEUEABLE);
        if (delivery == EDelivery::QUEUED && !receiver)
            return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
        if (delivery != EDelivery::DIRECT && delivery != EDelivery::QUEUED)
            return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
        try
        {
            if (!storage)
                storage = lux::cxx::make_intrusive<detail::SignalStorage>(factory);
            if (storage->closed.load(std::memory_order_acquire))
                return lux::cxx::unexpected(EConnectError::OBJECT_CLOSED);
            storage->maintain();
            auto control = lux::cxx::make_intrusive<detail::ConnectionControl>();
            control->storage = storage;
            control->delivery = delivery;
            control->callback = std::move(callback);
            if (receiver)
                control->receiver = receiver->ensureState();
            control->key = storage->records.emplace(control);
            if (control->receiver && !control->receiver->addIncoming(*control))
            {
                storage->records.erase(control->key);
                return lux::cxx::unexpected(EConnectError::OBJECT_CLOSED);
            }
            return Connection{std::move(control)};
        }

        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(EConnectError::CAPACITY_EXHAUSTED);
        }
    }

    SignalDelivery LuxObject::emitSignal(LuxObject* owner, detail::SignalStorage* storage, const void* payload) noexcept
    {
        assertAffinity();
        if (owner != this)
            detail::failObjectContract();
        if (!acceptsCallbacks())
            return {};
        const DispatchScope dispatch;
        ++active_events_;
        const auto result = storage ? storage->emit(payload) : SignalDelivery{};
        --active_events_;
        return result;
    }

    bool LuxObject::isDispatching() noexcept
    {
        return dispatch_depth != 0;
    }
}
