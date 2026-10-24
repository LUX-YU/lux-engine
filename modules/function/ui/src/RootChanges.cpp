#include <lux/engine/ui/detail/RootImpl.hpp>

#include <type_traits>

namespace lux::ui
{
    void Root::updatePane(object::LuxObject& value) noexcept
    {
        auto& pane = static_cast<Pane&>(value);
        pane.update_requested_ = false;
        ++pane.root_->impl_->statistics.panes;
        pane.update();
    }

    void Root::updateElement(object::LuxObject& value) noexcept
    {
        auto& element = static_cast<Element&>(value);
        element.update_requested_ = false;
        ++element.root().impl_->statistics.elements;
        element.update();
    }

    void Root::Impl::setContinuousUpdate(StoredTarget target, ChangeCallback apply, bool enabled) noexcept
    {
        auto found =
            std::ranges::find_if(continuous_updates, [&](const auto& entry) { return entry.target == target; });
        if (enabled)
        {
            if (found == continuous_updates.end())
            {
                continuous_updates.push_back({target, apply});
            }
            else
            {
                found->apply = apply;
            }
        }
        else if (found != continuous_updates.end())
        {
            if (updating)
            {
                found->apply = nullptr;
                removed_updates = true;
            }
            else
            {
                continuous_updates.erase(found);
            }
        }
    }

    void Root::setContinuousUpdate(Pane& pane, bool enabled) noexcept
    {
        requireOwner();
        impl_->setContinuousUpdate(Impl::store(*this, pane, pane), &Root::updatePane, enabled);
    }

    void Root::setContinuousUpdate(Element& element, bool enabled) noexcept
    {
        requireOwner();
        impl_->setContinuousUpdate(Impl::store(*this, element.pane(), element), &Root::updateElement, enabled);
    }

    void Root::activateUpdates(Element& subtree) noexcept
    {
        visitSubtree(
            subtree,
            [&](object::LuxObject& object) noexcept
            {
                auto& element = static_cast<Element&>(object);
                if (element.continuous_update_)
                {
                    setContinuousUpdate(element, true);
                }
                else if (element.update_requested_)
                {
                    element.requestUpdate();
                }
            }
        );
    }

    void Root::activateUpdates(Pane& pane) noexcept
    {
        if (pane.continuous_update_)
        {
            setContinuousUpdate(pane, true);
        }
        else if (pane.update_requested_)
        {
            pane.requestUpdate();
        }
        if (pane.content_)
        {
            activateUpdates(*pane.content_);
        }
    }

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
        // This is a cold detach path; stable frames visit only explicitly continuous objects.
        std::erase_if(
            continuous_updates,
            [&](const auto& update) { return update.target.object == target.objectId(); }
        );
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
