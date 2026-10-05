#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/scene/RenderSystem.hpp>

namespace lux::editor::application
{
    EditorResult<lux::ui::PaneHandle> EditorApplication::Impl::adopt(
        std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>& candidate
    )
    {
        auto* pane = candidate.get();
        auto mounted = desktop_->root().addSubPane(std::move(candidate));
        if (!mounted)
        {
            return applicationFailure("view.mount", mounted.error());
        }
        auto id = desktop_->root().identify(*pane);
        if (!id)
        {
            return applicationFailure("view.identity", id.error());
        }
        return *id;
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::open(AssetReference reference)
    {
        std::optional<EditorResult<sessions::OpenAssetId>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            result.emplace(openCaptured(reference, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
        {
            return applicationFailure("open.catalog", guarded.error());
        }
        return std::move(*result);
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::openCaptured(
        AssetReference reference,
        const extensions::ContributionSnapshot& snapshot
    )
    {
        if (phase_ != EApplicationPhase::RUNNING)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "application.open"});
        }
        if (opens_.size() == 64)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "application.open"});
        }
        auto opened = openProjectContent(*project_, files_, opening_, reference, snapshot.sessions());
        if (!opened)
        {
            return cxx::unexpected(opened.error());
        }
        opens_.push_back({*opened});
        return *opened;
    }

    EditorResult<OpenAndShowResult> EditorApplication::openStatus(sessions::OpenAssetId id) const
    {
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.status"});
        }
        auto status = impl_->opening_.status(id);
        if (!status)
        {
            return applicationFailure("open.status", status.error());
        }
        return OpenAndShowResult{std::move(*status), entry->view, entry->failure};
    }
    EditorResult<void> EditorApplication::cancelOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Impl::Dispatch scope{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
        {
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.cancel"});
        }
        auto cancelled = impl_->opening_.cancel(id);
        if (!cancelled)
        {
            return applicationFailure("open.cancel", cancelled.error());
        }
        entry->cancelled = true;
        return {};
    }
    EditorResult<void> EditorApplication::acknowledgeOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        Impl::Dispatch scope{impl_->dispatching_};
        auto acknowledged = impl_->opening_.acknowledge(id);
        if (!acknowledged)
        {
            return applicationFailure("open.acknowledge", acknowledged.error());
        }
        std::erase_if(impl_->opens_, [id](const auto& entry) { return entry.operation == id; });
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveOpenResults()
    {
        auto received = opening_.update();
        if (!received)
        {
            return applicationFailure("open.receive", received.error());
        }
        for (auto& entry : opens_)
        {
            if (entry.view || entry.failure || entry.cancelled)
            {
                continue;
            }
            const auto status = opening_.status(entry.operation);
            if (!status)
            {
                return applicationFailure("open.status", status.error());
            }
            if (status->stage != sessions::EOpenAssetStage::PUBLISHED)
            {
                continue;
            }
            auto shown = show(status->session, false);
            if (shown)
            {
                entry.view = *shown;
            }
            else if (shown.error().code != EEditorError::BUSY)
            {
                entry.failure = std::move(shown.error()); // Content remains owned and queryable without a view.
            }
        }
        return {};
    }
    EditorResult<lux::ui::PaneHandle> EditorApplication::Impl::show(sessions::SessionId id, bool another_view)
    {
        std::optional<EditorResult<lux::ui::PaneHandle>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            result.emplace(makeContentView({{id}, id}, another_view, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
        {
            return applicationFailure("show.catalog", guarded.error());
        }
        return std::move(*result);
    }
    EditorResult<lux::ui::PaneHandle> EditorApplication::Impl::makeContentView(
        views::ViewContent association,
        bool another_view,
        const extensions::ContributionSnapshot& snapshot,
        std::optional<views::ViewRestoreKey> restore_key,
        std::optional<views::ViewTypeId> preferred
    )
    {
        if (!association.valid())
        {
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "show.content"});
        }
        std::optional<views::ViewTypeId> selected = preferred;
        std::string title;
        for (const auto id : association.sessions)
        {
            auto info = sessions_.describe(id);
            if (!info)
            {
                return applicationFailure("show.session", info.error());
            }
            auto candidate = snapshot.ui().selectContent(info->kind, selected);
            if (!candidate)
            {
                return applicationFailure("show.provider", candidate.error());
            }
            selected = views::ViewTypeId{candidate->descriptor().type.name()};
            if (association.primary == id && info->binding)
            {
                title = info->binding->location;
            }
        }
        if (!selected)
        {
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "show.provider"});
        }
        auto views = editor_context_.ui().describe(desktop_->root());
        if (!views)
        {
            return applicationFailure("show.views", views.error());
        }
        std::optional<lux::ui::PaneHandle> existing;
        for (const auto& view : *views)
        {
            if (view.type != *selected)
            {
                continue;
            }
            const bool is_restore_target = restore_key && view.restore_key == *restore_key;
            const bool is_reusable = !another_view && !restore_key && view.content == association;
            if (is_reusable)
            {
                auto pane = desktop_->root().findPane(view.handle);
                if (!pane)
                {
                    return applicationFailure("show.target", pane.error());
                }
                (*pane)->setVisible(true);
                if (!desktop_->root().requestFocus(**pane))
                {
                    return cxx::unexpected(EditorFailure{EEditorError::BUSY, "show.focus"});
                }
                return view.handle;
            }
            if (is_restore_target)
            {
                if (!view.content.sessions.empty() && view.content != association)
                {
                    return cxx::unexpected(EditorFailure{
                        EEditorError::STALE_REQUEST,
                        "recovery.binding",
                        0,
                        "The matching window already displays different content; original binding retained."
                    });
                }
                if (view.content == association)
                {
                    return view.handle;
                }
                existing = view.handle;
                break;
            }
        }
        const bool is_full = !existing && views->size() == 64;
        if (is_full || next_view_ == UINT64_MAX)
        {
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "show.views"});
        }
        const auto name = "content-" + std::to_string(next_view_++);
        if (existing)
        {
            auto rebound = editor_context_.ui().rebind(desktop_->root(), *existing, association);
            if (!rebound)
            {
                return applicationFailure("recovery.binding", rebound.error());
            }
            return *existing;
        }
        auto factory = snapshot.ui().find(selected->view());
        if (!factory)
        {
            return applicationFailure("view.factory", factory.error());
        }
        auto view = editor_context_.ui().create(
            *factory,
            editor_context_.scope(),
            {messages_.dispatcherRef(),
             lux::ui::PaneId{name},
             association,
             {factory->descriptor().schema, {}},
             restore_key ? *restore_key : views::ViewRestoreKey{name}}
        );
        if (!view)
        {
            return applicationFailure("view.create", view.error());
        }
        if (!title.empty())
        {
            (*view)->setTitle(title);
        }
        auto adopted = adopt(*view);
        if (!adopted)
        {
            return cxx::unexpected(adopted.error());
        }
        return *adopted;
    }
} // namespace lux::editor::application
