#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    void Root::deferChange(Pane& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
        {
            detail::failContract();
        }
        impl_->queueChange(Impl::store(*this, target, target), apply);
    }

    void Root::deferChange(Element& target, ChangeCallback apply) noexcept
    {
        requireOwner();
        if (target.attachedRoot() != this)
        {
            detail::failContract();
        }
        impl_->queueChange(Impl::store(*this, target.pane(), target), apply);
    }

    Root::Impl::StoredTarget Root::Impl::store(Root& root, Pane& pane, object::LuxObject& target) noexcept
    {
        return {root.objectId(), pane.id_, target.objectId()};
    }

    object::LuxObject* Root::Impl::resolve(Root& root, StoredTarget target) noexcept
    {
        if (target.root != root.objectId())
        {
            return nullptr;
        }
        const auto* pane = root.findPane(target.pane);
        if (!pane)
        {
            return nullptr;
        }
        auto resolved = object::ObjectRuntime::instance().resolve(target.object);
        if (!resolved)
        {
            return nullptr;
        }
        for (auto* ancestor = *resolved; ancestor; ancestor = ancestor->parent())
        {
            if (ancestor == pane)
            {
                return *resolved;
            }
        }
        return nullptr;
    }

    void Root::Impl::queueChange(StoredTarget target, ChangeCallback apply) noexcept
    {
        if (!apply)
        {
            detail::failContract();
        }
        const Change change{target, apply};
        const auto pending = change_state.pending.begin() + change_state.batch_size;
        if (std::find(pending, change_state.pending.end(), change) == change_state.pending.end())
        {
            change_state.pending.push_back(change);
        }
    }

    void Root::Impl::cancelChanges(object::LuxObject& target) noexcept
    {
        for (auto& call : menu_state.calls)
        {
            if (call.target.object == target.objectId())
            {
                call.target = {};
            }
        }
        // The active batch keeps its indices even if a preceding callback destroys a later target.
        for (std::size_t index{}; index < change_state.batch_size; ++index)
        {
            if (change_state.pending[index].target.object == target.objectId())
            {
                change_state.pending[index] = {};
            }
        }
        const auto pending = change_state.pending.begin() + change_state.batch_size;
        const auto end = std::remove_if(
            pending,
            change_state.pending.end(),
            [&target](const Change& change) noexcept { return change.target.object == target.objectId(); }
        );
        change_state.pending.erase(end, change_state.pending.end());
    }

    void Root::applyPendingChanges() noexcept
    {
        requireOwner();
        const auto count = impl_->menu_state.calls.size();
        impl_->applyPendingChanges(*this);
        for (std::size_t index{}; index < count; ++index)
        {
            const auto call = impl_->menu_state.calls[index];
            auto* target = Impl::resolve(*this, call.target);
            if (!target)
            {
                continue;
            }
            Command command{call.command.view()};
            static_cast<void>(object::routeEvent(*target, *this, command));
            if (command.enabled)
            {
                command.phase = ECommandPhase::EXECUTE;
                static_cast<void>(object::routeEvent(*target, *this, command));
            }
        }
        impl_->menu_state.calls.erase(impl_->menu_state.calls.begin(), impl_->menu_state.calls.begin() + count);
    }

    void Root::Impl::applyPendingChanges(Root& root) noexcept
    {
        const bool is_active_visit = drawing || updating || layout_state.depth != 0;
        const bool is_active_callback = change_state.batch_size != 0 || change_state.active || Root::isDispatching();
        if (is_active_visit || is_active_callback)
        {
            detail::failContract();
        }
        change_state.batch_size = change_state.pending.size();
        for (std::size_t index{}; index < change_state.batch_size; ++index)
        {
            // No vector reference survives the call: it may enqueue or destroy other targets.
            const auto change = std::exchange(change_state.pending[index], Change{});
            auto* target = resolve(root, change.target);
            if (!target)
            {
                continue;
            }
            change_state.active = target;
            Root::beginCallbackBorrow(*target);
            change.apply(*target);
            Root::endCallbackBorrow(*target);
            change_state.active = nullptr;
        }
        change_state.pending.erase(
            change_state.pending.begin(),
            change_state.pending.begin() + change_state.batch_size
        );
        change_state.batch_size = 0;
    }

} // namespace lux::ui
