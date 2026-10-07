#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    PaneHandle Root::paneHandle(const Pane& pane) const noexcept
    {
        requireOwner();
        return pane.root_ == this ? PaneHandle{objectId(), pane.id_} : PaneHandle{};
    }

    Pane* Root::resolvePane(PaneHandle handle) const noexcept
    {
        requireOwner();
        return handle.root_ == objectId() ? findPane(handle.pane_) : nullptr;
    }

    Pane* Root::findPane(PaneId id) const noexcept
    {
        requireOwner();
        auto* owner = impl_->panes.tryGet(id);
        return owner ? owner->get() : nullptr;
    }

    PaneResult<void> Root::forEachPane(cxx::function_ref<void(Pane&) noexcept> visit) noexcept
    {
        if (auto ready = checkStructureSafe(); !ready)
        {
            return ready;
        }
        Mutation guard{impl_->updating};
        beginCallbackBorrow(*this);
        for (const auto& owner : impl_->panes.values())
        {
            beginCallbackBorrow(*owner);
            visit(*owner);
            endCallbackBorrow(*owner);
        }
        endCallbackBorrow(*this);
        return {};
    }

    bool Root::attachmentSafe() const noexcept
    {
        return !impl_->drawing && !impl_->updating && impl_->layout_state.depth == 0 && !impl_->committing_structure &&
               !isDispatching();
    }

    PaneResult<void> Root::checkStructureSafe() const noexcept
    {
        if (!isOnAffinityThread())
        {
            return cxx::unexpected(EPaneError::WRONG_THREAD);
        }
        if (isClosing())
        {
            return cxx::unexpected(EPaneError::CLOSED);
        }
        if (!attachmentSafe())
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        return {};
    }

    PaneResult<std::reference_wrapper<Pane>> Root::addPane(std::unique_ptr<Pane>&& candidate) noexcept
    {
        auto* pane = candidate.get();
        auto result = addPanes(std::span{&candidate, 1});
        if (!result)
        {
            return cxx::unexpected(result.error());
        }
        return std::ref(*pane);
    }

    PaneResult<void> Root::addPanes(std::span<std::unique_ptr<Pane>> candidates) noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return ready;
        }
        const auto available = impl_->pane_capacity - std::min(impl_->panes.size(), impl_->pane_capacity);
        if (candidates.size() > available)
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        std::vector<Pane*> prepared;
        prepared.reserve(candidates.size());
        for (const auto& candidate : candidates)
        {
            if (!candidate)
            {
                return cxx::unexpected(EPaneError::INVALID_TREE);
            }
            const bool already_attached = candidate->root_ || candidate->parent() || candidate->id_.isValid();
            if (already_attached)
            {
                return cxx::unexpected(EPaneError::ALREADY_ATTACHED);
            }
            if (std::ranges::find(prepared, candidate.get()) != prepared.end())
            {
                return cxx::unexpected(EPaneError::DUPLICATE_PANE);
            }
            auto relation = validateRelation(*candidate, this);
            if (!relation)
            {
                return cxx::unexpected(
                    relation.error() == object::EObjectTreeError::CLOSED ? EPaneError::CLOSED : EPaneError::BUSY
                );
            }
            prepared.push_back(candidate.get());
        }
        if (!impl_->panes.prepareInsert(candidates.size()))
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        for (auto* pane : prepared)
        {
            pane->imgui_label_.reserve(pane->title_.size() + 64);
        }
        Mutation commit{impl_->committing_structure};
        for (std::size_t index{}; index < candidates.size(); ++index)
        {
            auto* pane = prepared[index];
            pane->id_ = impl_->panes.insert(std::move(candidates[index]));
            pane->root_ = this;
            pane->rebuildImGuiLabel();
            commitRelation(*pane, this);
        }
        for (auto* pane : prepared)
        {
            pane->beginTreeVisit();
            static_cast<void>(emit(paneChanged, PaneChanged{pane, true}));
            pane->endTreeVisit();
        }
        return {};
    }

    PaneResult<std::unique_ptr<Pane>> Root::removePane(Pane& pane) noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (pane.root_ != this || findPane(pane.id_) != &pane)
        {
            return cxx::unexpected(EPaneError::NOT_ATTACHED);
        }
        if (!validateRelation(pane, nullptr))
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        Mutation commit{impl_->committing_structure};
        const auto id = pane.id_;
        releasePane(pane);
        commitRelation(pane, nullptr);
        pane.root_ = nullptr;
        pane.id_ = {};
        std::unique_ptr<Pane> owner;
        impl_->panes.extract(id, owner); // Move before swap-and-pop; no user destructor runs in the container.
        notifyRemoved(pane);
        static_cast<void>(emit(paneChanged, PaneChanged{&pane, false}));
        return owner;
    }

    PaneResult<void> Root::clearPanes() noexcept
    {
        auto ready = checkStructureSafe();
        if (!ready)
        {
            return ready;
        }
        for (const auto& owner : impl_->panes.values())
        {
            if (!validateRelation(*owner, nullptr))
            {
                return cxx::unexpected(EPaneError::BUSY);
            }
        }
        struct Removed final
        {
            PaneId id;
            std::unique_ptr<Pane> owner;
        };
        std::vector<Removed> removed(impl_->panes.size());
        Mutation commit{impl_->committing_structure};
        for (auto& entry : removed)
        {
            entry.id = impl_->panes.keys().back();
            auto& pane = **impl_->panes.tryGet(entry.id);
            releasePane(pane);
            commitRelation(pane, nullptr);
            pane.root_ = nullptr;
            pane.id_ = {};
            impl_->panes.extract(entry.id, entry.owner);
        }
        for (auto& entry : removed)
        {
            notifyRemoved(*entry.owner);
            static_cast<void>(emit(paneChanged, PaneChanged{entry.owner.get(), false}));
        }
        // Every registration has gone before notification or destruction can observe the Root.
        removed.clear();
        return {};
    }

    PaneResult<void> Root::compose(
        Pane* pane,
        Element* element,
        Element& child,
        Element* previous,
        bool replace
    ) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(EPaneError::WRONG_THREAD);
        }
        auto& parent = pane ? static_cast<object::LuxObject&>(*pane) : *element;
        auto* root = pane ? pane->root_ : element->attachedRoot();
        auto* old_root = child.attachedRoot();
        const bool is_busy = isDispatching() || (root && !root->attachmentSafe()) ||
                             (old_root && old_root != root && !old_root->attachmentSafe());
        if (is_busy)
        {
            return cxx::unexpected(EPaneError::BUSY);
        }
        if (previous && previous->parent() != &parent)
        {
            return cxx::unexpected(EPaneError::NOT_ATTACHED);
        }
        auto validate = [&]() noexcept -> PaneResult<void>
        {
            const auto valid = validateRelation(child, &parent);
            if (!valid)
            {
                using enum object::EObjectTreeError;
                if (valid.error() == CLOSED)
                {
                    return cxx::unexpected(EPaneError::CLOSED);
                }
                return cxx::unexpected(valid.error() == BUSY ? EPaneError::BUSY : EPaneError::INVALID_TREE);
            }
            if (previous && previous != &child && !validateRelation(*previous, nullptr))
            {
                return cxx::unexpected(EPaneError::BUSY);
            }
            return {};
        };
        if (auto valid = validate(); !valid)
        {
            return valid;
        }
        if (child.parent() == &parent && (!previous || previous == &child))
        {
            return {};
        }
        if (previous && !replace)
        {
            return cxx::unexpected(EPaneError::OCCUPIED);
        }

        bool detached_mutation{}, other_mutation{};
        Mutation destination{root ? root->impl_->committing_structure : detached_mutation};
        Mutation source{old_root && old_root != root ? old_root->impl_->committing_structure : other_mutation};
        // Complete active interaction while the old routing and containing Pane are still valid.
        // Every borrowed participant remains alive, including detached composites.
        if (pane)
        {
            pane->beginTreeVisit();
        }
        else
        {
            element->beginTreeVisit();
        }
        child.beginTreeVisit();
        if (previous && previous != &child)
        {
            previous->beginTreeVisit();
        }
        bool child_in_previous{};
        for (auto* ancestor = child.parent(); ancestor && !child_in_previous; ancestor = ancestor->parent())
        {
            child_in_previous = ancestor == previous;
        }
        if (child.parent() && !child_in_previous)
        {
            visitSubtree(child, [](object::LuxObject& node) noexcept { static_cast<Element&>(node).finishEdit(); });
        }
        if (previous && previous != &child)
        {
            visitSubtree(*previous, [](object::LuxObject& node) noexcept { static_cast<Element&>(node).finishEdit(); });
        }
        if (previous && previous != &child)
        {
            previous->endTreeVisit();
        }
        child.endTreeVisit();
        if (pane)
        {
            pane->endTreeVisit();
        }
        else
        {
            element->endTreeVisit();
        }
        if (auto valid = validate(); !valid)
        {
            return valid;
        }

        if (old_root)
        {
            old_root->releaseElement(child);
        }
        if (child.pane_ && child.pane_->content_ == &child)
        {
            child.pane_->content_ = nullptr;
        }
        if (previous)
        {
            if (root)
            {
                root->releaseElement(*previous);
            }
            commitRelation(*previous, nullptr);
            previous->assignPane(nullptr);
            previous->element_parent_ = nullptr;
        }
        commitRelation(child, &parent);
        child.element_parent_ = element;
        child.assignPane(pane ? pane : element->pane_);
        if (pane)
        {
            pane->content_ = &child;
        }
        if (old_root)
        {
            ++old_root->impl_->layout_state.epoch;
        }
        if (root && root != old_root)
        {
            ++root->impl_->layout_state.epoch;
        }
        if (old_root)
        {
            old_root->notifyRemoved(child);
        }
        if (root && previous)
        {
            root->notifyRemoved(*previous);
        }
        return {};
    }

    void Root::notifyRemoved(object::LuxObject& subtree) noexcept
    {
        beginCallbackBorrow(subtree);
        visitSubtree(
            subtree,
            [&](object::LuxObject& node) noexcept { static_cast<void>(emit(objectRemoved, ObjectRemoved{node})); }
        );
        endCallbackBorrow(subtree);
    }

    void Root::releasePane(Pane& pane) noexcept
    {
        if (pane.content_)
        {
            releaseElement(*pane.content_);
        }
        impl_->dock_state.pending.reset();
        if (impl_->menu_state.pane == &pane)
        {
            impl_->menu_state.pane = nullptr;
            impl_->menu_state.element = nullptr;
        }
        impl_->cancelChanges(pane);
        for (auto** target :
             {&impl_->focus_state.focused,
              &impl_->focus_state.hovered,
              &impl_->focus_state.pending_focus,
              &impl_->focus_state.draw_focused,
              &impl_->focus_state.draw_hovered,
              &impl_->focus_state.modal})
        {
            if (*target == &pane)
            {
                *target = nullptr;
            }
        }
        if (impl_->focus_state.pointer_capture.pane == &pane)
        {
            impl_->focus_state.pointer_capture = {};
        }
        pane.focused_ = pane.hovered_ = false;
        pane.close_requested_ = false;
    }

    void Root::releaseElement(Element& subtree) noexcept
    {
        visitSubtree(
            subtree,
            [&](object::LuxObject& node) noexcept
            {
                auto& element = static_cast<Element&>(node);
                if (impl_->menu_state.element == &element)
                {
                    impl_->menu_state.element = nullptr;
                }
                impl_->cancelChanges(element);
                if (impl_->focus_state.focused_element == &element)
                {
                    detail::ContextActivation context{impl_->context->native()};
                    ImGui::ClearActiveID();
                }
                for (auto** target :
                     {&impl_->focus_state.focused_element,
                      &impl_->focus_state.hovered_element,
                      &impl_->focus_state.pending_element,
                      &impl_->focus_state.draw_focused_element,
                      &impl_->focus_state.draw_hovered_element})
                {
                    if (*target == &element)
                    {
                        *target = nullptr;
                    }
                }
                if (impl_->focus_state.pointer_capture.element == &element)
                {
                    impl_->focus_state.pointer_capture = {};
                }
                element.hovered_ = false;
            }
        );
    }

    void Root::checkDestruction(const object::LuxObject& object) const noexcept
    {
        requireOwner();
        for (auto* callback : {impl_->active_update, impl_->change_state.active})
        {
            for (auto* active = callback; active; active = active->parent())
            {
                if (active == &object)
                {
                    detail::failContract();
                }
            }
        }
    }

    void Root::checkContentChange() const noexcept
    {
        requireOwner();
        if (!attachmentSafe())
        {
            detail::failContract();
        }
    }

} // namespace lux::ui
