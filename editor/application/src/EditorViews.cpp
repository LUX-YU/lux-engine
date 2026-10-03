#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <algorithm>

namespace lux::editor::application
{
    scene::SceneViewServices EditorApplication::Impl::sceneServices()
    {
        auto& rendering = *engine_->renderContext();
        return {
            sessions_.access<scene::SceneSession>(),
            projections_,
            engine_->sceneRuntime(),
            rendering.resources(),
            rendering.runtime(),
            environment_,
            runs_.inspect()
        };
    }
    EditorResult<views::ViewId> EditorApplication::Impl::adopt(views::DetachedView& candidate, std::string key)
    {
        auto result = desktop_->views().adopt(candidate, views::ViewRestoreKey{key});
        if (!result)
            return applicationFailure("view.adopt", result.error());
        return result->id;
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
            return applicationFailure("open.catalog", guarded.error());
        return std::move(*result);
    }
    EditorResult<sessions::OpenAssetId> EditorApplication::Impl::openCaptured(
        AssetReference reference,
        const extensions::ContributionSnapshot& snapshot
    )
    {
        if (phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::CLOSING, "application.open"});
        if (opens_.size() == 64)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "application.open"});
        auto opened = openProjectContent(*project_, files_, opening_, reference, snapshot.sessions());
        if (!opened)
            return cxx::unexpected(opened.error());
        opens_.push_back({*opened});
        return *opened;
    }

    EditorResult<OpenAndShowResult> EditorApplication::openStatus(sessions::OpenAssetId id) const
    {
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.status"});
        auto status = impl_->opening_.status(id);
        if (!status)
            return applicationFailure("open.status", status.error());
        return OpenAndShowResult{std::move(*status), entry->view, entry->failure};
    }
    EditorResult<void> EditorApplication::cancelOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        const auto entry = std::ranges::find(impl_->opens_, id, &Impl::OpenPresentation::operation);
        if (entry == impl_->opens_.end())
            return cxx::unexpected(EditorFailure{EEditorError::STALE_REQUEST, "open.cancel"});
        auto cancelled = impl_->opening_.cancel(id);
        if (!cancelled)
            return applicationFailure("open.cancel", cancelled.error());
        entry->cancelled = true;
        return {};
    }
    EditorResult<void> EditorApplication::acknowledgeOpen(sessions::OpenAssetId id)
    {
        if (auto ready = impl_->admission(); !ready)
            return cxx::unexpected(ready.error());
        Impl::Dispatch scope{impl_->dispatching_};
        auto acknowledged = impl_->opening_.acknowledge(id);
        if (!acknowledged)
            return applicationFailure("open.acknowledge", acknowledged.error());
        std::erase_if(impl_->opens_, [id](const auto& entry) { return entry.operation == id; });
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveOpenResults()
    {
        auto received = opening_.update();
        if (!received)
            return applicationFailure("open.receive", received.error());
        for (auto& entry : opens_)
        {
            if (entry.view || entry.failure || entry.cancelled)
                continue;
            const auto status = opening_.status(entry.operation);
            if (!status)
                return applicationFailure("open.status", status.error());
            if (status->stage != sessions::EOpenAssetStage::PUBLISHED)
                continue;
            auto shown = show(status->session, false);
            if (shown)
                entry.view = *shown;
            else if (shown.error().code != EEditorError::BUSY)
                entry.failure = std::move(shown.error()); // Content remains owned and queryable without a view.
        }
        return {};
    }
    EditorResult<views::ViewId> EditorApplication::Impl::show(sessions::SessionId id, bool another_view)
    {
        std::optional<EditorResult<views::ViewId>> result;
        auto prepare = [&](const extensions::ContributionSnapshot& snapshot) -> extensions::ContributionResult<void>
        {
            result.emplace(makeContentView({{id}, id}, another_view, snapshot));
            return {};
        };
        auto guarded = contributions_.withSnapshot(prepare);
        if (!guarded)
            return applicationFailure("show.catalog", guarded.error());
        return std::move(*result);
    }
    EditorResult<views::ViewId> EditorApplication::Impl::makeContentView(
        views::ViewContent association,
        bool another_view,
        const extensions::ContributionSnapshot& snapshot,
        std::optional<views::ViewRestoreKey> restore_key,
        std::optional<views::ViewTypeId> preferred
    )
    {
        if (!association.valid())
            return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "show.content"});
        std::optional<views::ViewTypeId> selected = preferred;
        std::string title;
        for (const auto id : association.sessions)
        {
            auto info = sessions_.describe(id);
            if (!info)
                return applicationFailure("show.session", info.error());
            auto candidate = snapshot.views().selectContent(info->kind, selected);
            if (!candidate)
                return applicationFailure("show.provider", candidate.error());
            selected = *candidate;
            if (association.primary == id && info->binding)
                title = info->binding->location;
        }
        if (!selected)
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "show.provider"});
        auto views = desktop_->views().describeAll();
        if (!views)
            return applicationFailure("show.views", views.error());
        std::optional<views::ViewId> existing;
        for (const auto& view : *views)
        {
            if (view.type != *selected)
                continue;
            const bool is_restore_target = restore_key && view.restore_key == *restore_key;
            const bool is_reusable = !another_view && !restore_key && view.content == association;
            if (is_reusable)
            {
                auto focused = desktop_->views().focus(view.id);
                if (!focused)
                    return applicationFailure("show.focus", focused.error());
                return view.id;
            }
            if (is_restore_target)
            {
                if (!view.content.sessions.empty() && view.content != association)
                    return cxx::unexpected(EditorFailure{
                        EEditorError::STALE_REQUEST,
                        "recovery.binding",
                        0,
                        "The matching window already displays different content; original binding retained."
                    });
                if (view.content == association)
                    return view.id;
                existing = view.id;
                break;
            }
        }
        const bool is_full = !existing && views->size() == 64;
        if (is_full || next_view_ == UINT64_MAX)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "show.views"});
        const auto name = "content-" + std::to_string(next_view_++);
        if (existing)
        {
            auto rebound = desktop_->views().rebindContent(*existing, association);
            if (!rebound)
                return applicationFailure("recovery.binding", rebound.error());
            return *existing;
        }
        views::ContentViewInput value{association, title.empty() ? name : std::move(title)};
        const views::ViewFactoryInput input{
            messages_.dispatcherRef(),
            lux::ui::PaneId{name},
            contracts::CodeLease::builtin(),
            cxx::typeToken<views::ContentViewInput>(),
            std::make_shared<const views::ContentViewInput>(std::move(value))
        };
        auto view = snapshot.views().prepare(*selected, input);
        if (!view)
            return applicationFailure("view.factory", view.error());
        auto adopted = adopt(*view, restore_key ? std::string(restore_key->name()) : name);
        if (!adopted)
            return cxx::unexpected(adopted.error());
        return *adopted;
    }
} // namespace lux::editor::application
