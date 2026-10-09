#include <lux/engine/ui/detail/RootImpl.hpp>

#include <type_traits>

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
        const auto pending = safe_point_state.pending.begin() + safe_point_state.batch_size;
        const auto same = [target, apply](const UiSafePointAction& action) noexcept
        {
            const auto* mutation = std::get_if<DeferredMutation>(&action.payload);
            return mutation && action.target == target && mutation->apply == apply;
        };
        if (std::find_if(pending, safe_point_state.pending.end(), same) == safe_point_state.pending.end())
        {
            safe_point_state.pending.push_back({target, DeferredMutation{apply}});
        }
    }

    void Root::Impl::cancelActions(object::LuxObject& target) noexcept
    {
        // The active batch keeps its indices even if a preceding callback destroys a later target.
        for (std::size_t index{}; index < safe_point_state.batch_size; ++index)
        {
            if (safe_point_state.pending[index].target.object == target.objectId())
            {
                safe_point_state.pending[index] = {};
            }
        }
        const auto pending = safe_point_state.pending.begin() + safe_point_state.batch_size;
        const auto end = std::remove_if(
            pending,
            safe_point_state.pending.end(),
            [&target](const UiSafePointAction& action) noexcept { return action.target.object == target.objectId(); }
        );
        safe_point_state.pending.erase(end, safe_point_state.pending.end());
    }

    void Root::applyPendingChanges() noexcept
    {
        requireOwner();
        impl_->applyPendingChanges(*this);
    }

    void Root::Impl::applyPendingChanges(Root& root) noexcept
    {
        const bool is_active_visit = drawing || updating || layout_state.depth != 0;
        const bool is_active_callback =
            safe_point_state.batch_size != 0 || safe_point_state.active || Root::isDispatching();
        if (is_active_visit || is_active_callback)
        {
            detail::failContract();
        }
        safe_point_state.batch_size = safe_point_state.pending.size();
        for (std::size_t index{}; index < safe_point_state.batch_size; ++index)
        {
            // No vector reference survives the call: it may enqueue or destroy other targets.
            auto action = std::exchange(safe_point_state.pending[index], UiSafePointAction{});
            auto* target = resolve(root, action.target);
            const bool is_stale_target = !action.target.object.isNull() && !target;
            if (is_stale_target)
            {
                continue;
            }
            safe_point_state.active = target;
            if (target)
            {
                Root::beginCallbackBorrow(*target);
            }
            std::visit(
                [&](const auto& payload) noexcept
                {
                    using T = std::remove_cvref_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, DeferredMutation>)
                    {
                        if (target && payload.apply)
                        {
                            payload.apply(*target);
                        }
                    }
                    else
                    {
                        Command command{payload.command.view()};
                        routeCommand(root, target, command);
                        if (command.enabled)
                        {
                            command.phase = ECommandPhase::EXECUTE;
                            routeCommand(root, target, command);
                        }
                    }
                },
                action.payload
            );
            if (target)
            {
                Root::endCallbackBorrow(*target);
            }
            safe_point_state.active = nullptr;
        }
        safe_point_state.pending.erase(
            safe_point_state.pending.begin(),
            safe_point_state.pending.begin() + safe_point_state.batch_size
        );
        safe_point_state.batch_size = 0;
    }

} // namespace lux::ui
