#include <lux/engine/ui/detail/RootImpl.hpp>

namespace lux::ui
{
    PaneResult<std::vector<std::unique_ptr<Pane>>> Root::replacePanes(
        std::span<const PaneHandle> remove,
        std::span<std::unique_ptr<Pane>> add,
        PaneCommit on_commit
    ) noexcept
    {
        if (auto ready = checkStructureSafe(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        std::vector<Pane*> old_panes, new_panes;
        old_panes.reserve(remove.size());
        new_panes.reserve(add.size());
        for (const auto handle : remove)
        {
            auto* pane = resolvePane(handle);
            if (!pane)
            {
                continue;
            }
            if (std::ranges::find(old_panes, pane) != old_panes.end())
            {
                return cxx::unexpected(EPaneError::DUPLICATE_PANE);
            }
            if (!validateRelation(*pane, nullptr))
            {
                return cxx::unexpected(EPaneError::BUSY);
            }
            old_panes.push_back(pane);
        }
        for (const auto& candidate : add)
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
            if (std::ranges::find(new_panes, candidate.get()) != new_panes.end())
            {
                return cxx::unexpected(EPaneError::DUPLICATE_PANE);
            }
            if (auto relation = validateRelation(*candidate, this); !relation)
            {
                return cxx::unexpected(
                    relation.error() == object::EObjectTreeError::CLOSED ? EPaneError::CLOSED : EPaneError::BUSY
                );
            }
            new_panes.push_back(candidate.get());
        }
        const auto retained = impl_->panes.size() - old_panes.size();
        const auto available = impl_->pane_capacity - std::min(retained, impl_->pane_capacity);
        const bool exceeds_capacity = add.size() > available;
        if (exceeds_capacity)
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        if (!impl_->panes.prepareInsert(add.size()))
        {
            return cxx::unexpected(EPaneError::CAPACITY);
        }
        std::vector<std::unique_ptr<Pane>> removed(old_panes.size());
        std::vector<PaneHandle> added;
        added.reserve(add.size());
        for (auto* pane : new_panes)
        {
            pane->imgui_label_.reserve(pane->title_.size() + 64);
        }

        Mutation commit{impl_->committing_structure};
        for (std::size_t index{}; index < old_panes.size(); ++index)
        {
            auto& pane = *old_panes[index];
            const auto id = pane.id_;
            releasePane(pane);
            commitRelation(pane, nullptr);
            pane.root_ = nullptr;
            pane.id_ = {};
            impl_->panes.extract(id, removed[index]);
        }
        for (std::size_t index{}; index < add.size(); ++index)
        {
            auto* pane = new_panes[index];
            pane->id_ = impl_->panes.insert(std::move(add[index]));
            pane->root_ = this;
            pane->rebuildImGuiLabel();
            commitRelation(*pane, this);
            added.push_back(paneHandle(*pane));
        }
        beginCallbackBorrow(*this);
        on_commit(added);
        endCallbackBorrow(*this);
        for (const auto& pane : removed)
        {
            notifyRemoved(*pane);
            static_cast<void>(emit(paneChanged, PaneChanged{pane.get(), false}));
        }
        for (auto* pane : new_panes)
        {
            pane->beginTreeVisit();
            static_cast<void>(emit(paneChanged, PaneChanged{pane, true}));
            pane->endTreeVisit();
        }
        return removed;
    }
} // namespace lux::ui
