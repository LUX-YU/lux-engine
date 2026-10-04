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

    bool ObjectState::reclaimOwner(Reclamation& node) noexcept
    {
        auto& state = static_cast<ObjectState&>(node);
        auto* value = state.object.load(std::memory_order_acquire);
        if (value)
        {
            auto* parent = value->parent_;
            const bool is_busy = value->hasActiveTree() || (parent && !parent->acceptsCallbacks());
            if (is_busy)
                return false;
            if (parent && value->owned_edge_)
            {
                parent->changing_children_ = true;
                parent->destroyOwnedChild(*value);
                parent->changing_children_ = false;
            }
        }
        state.destruction_requested = false;
        releaseReclamation(state.dispatcher);
        intrusive_ptr_release(&state);
        return true;
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
        if (std::this_thread::get_id() == affinity && depth == 0 && !maintaining)
            maintain();
        else
            scheduleSignalMaintenance(dispatcher, *this);
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
        if (std::this_thread::get_id() != affinity || depth || maintaining)
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
        if (std::this_thread::get_id() != affinity || depth)
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
            switch (post(control->receiver->dispatcher, std::move(message)))
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
    struct LuxObject::OwnedEdge final
    {
        ObjectDeleter destroy;
    };

    EObjectOwnership LuxObject::ownership() const noexcept
    {
        // A reserved edge is not ownership until the relation is committed.
        return owned_edge_ && parent_ ? EObjectOwnership::PARENT_OWNED : EObjectOwnership::EXTERNAL;
    }

    ObjectResult<void> LuxObject::requestDestruction() noexcept
    {
        if (!isOnAffinityThread())
            return lux::cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        if (closing_)
            return lux::cxx::unexpected(EObjectTreeError::CLOSED);
        const bool has_parent_owner = owned_edge_ && parent_;
        if (!has_parent_owner)
            return lux::cxx::unexpected(EObjectTreeError::NOT_OWNED);
        if (!acceptsCallbacks())
            return lux::cxx::unexpected(EObjectTreeError::BUSY);
        const bool has_live_dispatcher = dispatcher_ && dispatcher_.isCurrent();
        if (!has_live_dispatcher)
            return lux::cxx::unexpected(EObjectTreeError::WRONG_DISPATCHER);
        auto state = ensureState();
        if (!state->destruction_requested)
        {
            state->destruction_requested = true;
            state->reclaim = &detail::ObjectState::reclaimOwner;
            detail::intrusive_ptr_add_ref(state.get());
            detail::retainReclamation(dispatcher_);
            detail::scheduleReclamation(dispatcher_, *state);
        }
        return {};
    }

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

    ObjectResult<void> LuxObject::validateChild(const LuxObject& child) const noexcept
    {
        const bool is_wrong_thread = !isOnAffinityThread() || !child.isOnAffinityThread();
        if (is_wrong_thread)
            return lux::cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        if (dispatcher_ != child.dispatcher_)
            return lux::cxx::unexpected(EObjectTreeError::WRONG_DISPATCHER);
        const bool is_closed = closing_ || child.closing_;
        if (is_closed)
            return lux::cxx::unexpected(EObjectTreeError::CLOSED);
        const bool is_busy = isDispatching() || !acceptsCallbacks() || child.hasActiveTree();
        if (is_busy)
            return lux::cxx::unexpected(EObjectTreeError::BUSY);
        if (child.parent_)
            return lux::cxx::unexpected(EObjectTreeError::ALREADY_ATTACHED);
        for (auto* ancestor = this; ancestor; ancestor = ancestor->parent_)
        {
            if (ancestor->active_events_)
                return lux::cxx::unexpected(EObjectTreeError::BUSY);
            if (ancestor == &child)
                return lux::cxx::unexpected(EObjectTreeError::INVALID_TREE);
        }
        return {};
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

    ObjectResult<void> LuxObject::attachChild(LuxObject& child) noexcept
    {
        auto valid = validateChild(child);
        if (!valid)
            return valid;
        linkChild(child);
        return {};
    }

    ObjectResult<void> LuxObject::beginAdoption(LuxObject& child) noexcept
    {
        auto valid = validateChild(child);
        if (!valid)
            return valid;
        changing_children_ = child.changing_children_ = true;
        child.owned_edge_ = std::make_unique<OwnedEdge>();
        return {};
    }

    void LuxObject::finishAdoption(LuxObject& child, ObjectDeleter deleter) noexcept
    {
        child.owned_edge_->destroy = std::move(deleter);
        linkChild(child);
        changing_children_ = child.changing_children_ = false;
    }

    ObjectResult<void> LuxObject::adoptChildren(
        std::span<LuxObject* const> children, cxx::function_ref<ObjectDeleter(std::size_t)> transfer
    ) noexcept
    {
        std::vector<std::unique_ptr<OwnedEdge>> edges;
        edges.reserve(children.size());
        for (std::size_t i{}; i < children.size(); ++i)
        {
            auto* child = children[i];
            if (!child)
                return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
            auto valid = validateChild(*child);
            if (!valid)
                return valid;
            for (std::size_t j{}; j < i; ++j)
                if (children[j] == child)
                    return cxx::unexpected(EObjectTreeError::INVALID_TREE);
            edges.push_back(std::make_unique<OwnedEdge>());
        }
        // Moving a stateful deleter can enter foreign code. Protect the complete candidate batch,
        // including siblings not yet transferred; no parent relation is visible until all moves finish.
        changing_children_ = true;
        for (auto* child : children)
            child->changing_children_ = true;
        for (std::size_t i{}; i < children.size(); ++i)
            edges[i]->destroy = transfer(i);
        for (std::size_t i{}; i < children.size(); ++i)
        {
            children[i]->owned_edge_ = std::move(edges[i]);
            linkChild(*children[i]);
        }
        for (auto* child : children)
            child->changing_children_ = false;
        changing_children_ = false;
        return {};
    }

    ObjectResult<void> LuxObject::detachChild(LuxObject& child) noexcept
    {
        const bool is_wrong_thread = !isOnAffinityThread() || !child.isOnAffinityThread();
        if (is_wrong_thread)
            return lux::cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        if (child.parent_ != this)
            return lux::cxx::unexpected(EObjectTreeError::INVALID_TREE);
        if (child.owned_edge_)
            return lux::cxx::unexpected(EObjectTreeError::OWNED_CHILD);
        const bool is_busy = isDispatching() || !acceptsCallbacks() || child.hasActiveTree();
        if (is_busy)
            return lux::cxx::unexpected(EObjectTreeError::BUSY);
        child.unlinkParent();
        return {};
    }

    void LuxObject::destroyOwnedChild(LuxObject& child) noexcept
    {
        child.closing_ = true;
        if (auto* state = child.state_.load(std::memory_order_acquire))
            state->closeOwner();
        child.unlinkParent();
        auto edge = std::move(child.owned_edge_);
        edge->destroy(&child);
    }

    void LuxObject::clearChildren() noexcept
    {
        assertAffinity();
        if (hasActiveTree())
            detail::failObjectContract();
        changing_children_ = true;
        auto* owner = this;
        for (;;)
        {
            if (auto* child = owner->last_child_)
            {
                if (child->owned_edge_)
                {
                    // Postorder mechanical cleanup: no recursive subtree scans or destructor stack.
                    child->closing_ = child->changing_children_ = true;
                    if (auto* state = child->state_.load(std::memory_order_acquire))
                        state->closeOwner();
                    owner = child;
                }
                else
                    child->unlinkParent();
            }
            else
            {
                if (owner == this)
                    break;
                auto* parent = owner->parent_;
                owner->changing_children_ = false;
                parent->destroyOwnedChild(*owner);
                owner = parent;
            }
        }
        changing_children_ = false;
    }

    LuxObject::LuxObject(ObjectDispatcherRef dispatcher) noexcept
        : affinity_(std::this_thread::get_id()), dispatcher_(std::move(dispatcher))
    {
        if (dispatcher_ && !dispatcher_.isCurrent())
            detail::failObjectContract();
    }

    LuxObject::LuxObject(LuxObject* parent) noexcept
        : LuxObject(parent ? parent->dispatcherRef() : ObjectDispatcherRef{})
    {
        if (!parent || !parent->allowsGenericChildren())
            detail::failObjectContract();
        attachTo(*parent);
    }

    void LuxObject::attachTo(LuxObject& parent) noexcept
    {
        if (!parent.attachChild(*this))
            detail::failObjectContract();
    }

    void LuxObject::detachFromParent() noexcept
    {
        assertAffinity();
        const bool is_active = active_events_ != 0 || (parent_ && parent_->active_events_ != 0);
        if (is_active || owned_edge_)
            detail::failObjectContract();
        unlinkParent();
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
        // Closing rejects callbacks immediately. Keep the identity pointer until physical destruction:
        // an already queued reclamation still needs it to release the original owning edge.
    }

    LuxObject::~LuxObject()
    {
        assertAffinity();
        const bool is_illegal_destruction = owned_edge_ || active_events_ != 0 || callback_borrows_ != 0;
        if (is_illegal_destruction)
            detail::failObjectContract();
        closing_ = true;
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
        return std::this_thread::get_id() == affinity_;
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
            auto* candidate = new detail::ObjectState{const_cast<LuxObject*>(this), dispatcher_, affinity_};
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

    ObjectIdentity LuxObject::identity() const noexcept
    {
        return ObjectIdentity{ensureState()};
    }

    ObjectResult<LuxObject*> ObjectIdentity::resolve() const noexcept
    {
        if (!state_)
        {
            return cxx::unexpected(EObjectTreeError::INVALID_OBJECT);
        }
        if (state_->affinity != std::this_thread::get_id())
        {
            return cxx::unexpected(EObjectTreeError::WRONG_THREAD);
        }
        auto* object = state_->object.load(std::memory_order_acquire);
        if (!object || object->isClosing())
        {
            return cxx::unexpected(EObjectTreeError::CLOSED);
        }
        return object;
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
            delivery = !receiver || receiver->affinity_ == affinity_ ? EDelivery::DIRECT : EDelivery::QUEUED;
        if (delivery == EDelivery::DIRECT && receiver && receiver->affinity_ != affinity_)
            return lux::cxx::unexpected(EConnectError::DIRECT_CROSS_AFFINITY);
        if (delivery == EDelivery::QUEUED && !factory)
            return lux::cxx::unexpected(EConnectError::PAYLOAD_NOT_QUEUEABLE);
        if (delivery == EDelivery::QUEUED && (!receiver || !receiver->dispatcher_))
            return lux::cxx::unexpected(EConnectError::RECEIVER_HAS_NO_DISPATCHER);
        if (delivery != EDelivery::DIRECT && delivery != EDelivery::QUEUED)
            return lux::cxx::unexpected(EConnectError::INVALID_ARGUMENT);
        try
        {
            if (!storage)
                storage = lux::cxx::make_intrusive<detail::SignalStorage>(dispatcher_, factory);
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
