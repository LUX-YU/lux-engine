#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>

namespace lux::editor::application
{
    namespace
    {
        // These are old persisted data identifiers, not executable compatibility factories.
        views::ViewTypeId recoveryType(views::ViewTypeId type)
        {
            if (type == views::ViewTypeId{"lux.editor.scene.v1"})
                return views::ViewTypeId{"lux.editor.scene.view"};
            if (type == views::ViewTypeId{"lux.editor.material.v1"})
                return views::ViewTypeId{"lux.editor.material"};
            if (type == views::ViewTypeId{"lux.editor.flowforge.v1"})
                return views::ViewTypeId{"lux.editor.flowforge"};
            return type;
        }
    }
    EditorResult<void> EditorApplication::Impl::captureRecovery()
    {
        auto previous = workspace_.readRecovery();
        if (!previous && previous.error().code != workspace::EWorkspaceError::NOT_FOUND)
            return applicationFailure("recovery.read", previous.error());
        auto value = previous ? previous->value : workspace::RecoveryManifest{};
        const auto version = previous ? previous->target.expected_version : "missing";
        // Preserve unknown records and payload. Only the actual currently bound view entries are replaced.
        auto views = desktop_->views().describeAll();
        if (!views)
            return applicationFailure("recovery.views", views.error());
        const auto catalog = contributions_.snapshot();
        for (const auto& view : *views)
        {
            if (view.content.sessions.empty())
                continue;
            const auto factory = std::ranges::find_if(catalog.views().entries(), [&](const auto& entry) {
                return entry->descriptor().type == view.type &&
                       entry->descriptor().binding_type == cxx::typeToken<views::ContentViewInput>();
            });
            if (factory == catalog.views().entries().end())
                continue; // Contextual auxiliary windows are layout entries, not content creation factories.
            workspace::RecoveryEntry next{view.restore_key, view.type};
            for (const auto id : view.content.sessions)
            {
                auto source = sessions_.describe(id);
                if (!source)
                    return applicationFailure("recovery.source", source.error());
                if (!source->binding)
                    return cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT, "recovery.unbound", 0,
                        "Save unbound content before recording its recovery location."
                    });
                if (view.content.primary == id)
                    next.primary = static_cast<std::uint32_t>(next.contents.size());
                next.contents.push_back({"asset:" + uuids::to_string(source->binding->asset.uuid()), source->dirty});
            }
            auto found = std::ranges::find_if(value.entries, [&](const auto& entry) {
                return entry.restore_key == next.restore_key && entry.type == next.type;
            });
            if (found == value.entries.end())
                value.entries.push_back(std::move(next));
            else
                *found = std::move(next);
        }
        auto written = workspace_.writeRecovery(value, version);
        if (!written)
            return applicationFailure("recovery.capture", written.error());
        workspace_publications_.push_back({"Record recovery locations (not unsaved content)", *written});
        return {};
    }
    EditorResult<void> EditorApplication::Impl::restoreRecovery()
    {
        if (recovery_ &&
            std::ranges::any_of(recovery_->items, [](const auto& item) { return item.opening.has_value(); }))
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recovery.pending"});
        auto stored = workspace_.readRecovery();
        if (!stored)
            return applicationFailure("recovery.read", stored.error());
        RecoveryPresentation next{contributions_.snapshot()};
        next.items.reserve(stored->value.entries.size());
        for (auto entry : stored->value.entries)
            next.items.push_back({std::move(entry)});
        recovery_ = std::move(next);
        return {};
    }
    EditorResult<void> EditorApplication::Impl::settleRecovery()
    {
        if (!recovery_)
            return {};
        for (auto& item : recovery_->items)
        {
            if (item.result)
                continue;
            if (item.opening)
            {
                auto status = opening_.status(*item.opening);
                if (!status)
                    return applicationFailure("recovery.open.status", status.error());
                const bool is_pending = status->stage == sessions::EOpenAssetStage::READING ||
                                        status->stage == sessions::EOpenAssetStage::PREPARING;
                const bool is_reviewing = phase_ == EApplicationPhase::REVIEWING ||
                                          phase_ == EApplicationPhase::COMMITTING_EXIT;
                if (is_pending || (status->stage == sessions::EOpenAssetStage::PUBLISHED && is_reviewing))
                    continue;
                auto acknowledged = opening_.acknowledge(*item.opening);
                if (!acknowledged)
                    return applicationFailure("recovery.acknowledge", acknowledged.error());
                std::erase_if(opens_, [&](const auto& entry) { return entry.operation == *item.opening; });
                item.opening.reset();
                item.sources.push_back(std::move(*status));
                const auto& completed = item.sources.back();
                if (completed.stage != sessions::EOpenAssetStage::PUBLISHED)
                {
                    item.result.emplace(completed.failure
                        ? applicationFailure("recovery.open", *completed.failure)
                        : cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "recovery.open"}));
                    continue;
                }
            }
            if (phase_ != EApplicationPhase::RUNNING)
            {
                if (phase_ == EApplicationPhase::DRAINING)
                    item.result.emplace(cxx::unexpected(EditorFailure{EEditorError::CLOSING, "recovery.presentation"}));
                continue;
            }
            if (item.sources.size() < item.entry.contents.size())
            {
                // One admitted read per entry. Completed facts remain owned even if another source or
                // the final view fails; no partial content publication is rolled back for a UI error.
                const auto& locator = item.entry.contents[item.sources.size()].locator;
                const auto parsed = locator.starts_with("asset:") ? uuids::uuid::from_string(locator.substr(6))
                                                                  : std::optional<uuids::uuid>{};
                if (!parsed)
                {
                    item.result.emplace(cxx::unexpected(EditorFailure{
                        EEditorError::INVALID_ARGUMENT, "recovery.locator", 0, locator
                    }));
                    continue;
                }
                const auto asset_id = asset::AssetId{*parsed};
                const auto* asset = project_->asset(asset_id);
                const auto factory = asset
                    ? recovery_->catalog.sessions().selectSource(asset->source_type, asset->source_version)
                    : sessions::SessionFactoryResult<std::shared_ptr<sessions::SessionFactoryEntry>>{
                        cxx::unexpected(sessions::SessionFactoryFailure{
                            sessions::ESessionFactoryError::NOT_FOUND, "recovery.asset"
                        })
                    };
                const bool matching = factory && recovery_->catalog.views().selectContent(
                    (*factory)->descriptor().kind, recoveryType(item.entry.type)
                ).has_value();
                if (!matching)
                {
                    item.result.emplace(cxx::unexpected(EditorFailure{
                        EEditorError::MISSING_PROVIDER, "recovery.type-or-asset", 0,
                        "Entry retained: its asset or exact content view type is unavailable."
                    }));
                    continue;
                }
                std::optional<EditorResult<sessions::OpenAssetId>> admitted;
                auto prepare = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void> {
                    admitted.emplace(openCaptured(project_->reference(asset_id), recovery_->catalog));
                    return {};
                };
                auto guarded = contributions_.withSnapshot(prepare);
                if (!guarded)
                {
                    if (guarded.error().code != extensions::EContributionError::BUSY)
                        item.result.emplace(applicationFailure("recovery.catalog", guarded.error()));
                    continue;
                }
                if (!*admitted)
                {
                    if (admitted->error().code != EEditorError::BUSY)
                        item.result.emplace(cxx::unexpected(admitted->error()));
                    continue;
                }
                item.opening = **admitted;
                auto presentation = std::ranges::find(opens_, **admitted, &OpenPresentation::operation);
                if (presentation == opens_.end())
                    std::terminate();
                presentation->present = false;
                continue;
            }
            views::ViewContent association;
            for (const auto& content : item.sources)
                association.sessions.push_back(content.session);
            if (item.entry.primary)
                association.primary = association.sessions[*item.entry.primary];
            std::optional<EditorResult<views::ViewId>> displayed;
            auto prepare = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void> {
                displayed.emplace(makeContentView(
                    association, true, recovery_->catalog, item.entry.restore_key, recoveryType(item.entry.type)
                ));
                return {};
            };
            auto guarded = contributions_.withSnapshot(prepare);
            if (!guarded)
            {
                if (guarded.error().code != extensions::EContributionError::BUSY)
                    item.result.emplace(applicationFailure("recovery.view.catalog", guarded.error()));
                continue;
            }
            if (!*displayed && displayed->error().code == EEditorError::BUSY)
                continue;
            item.result = std::move(displayed);
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::settleMigration()
    {
        if (!migration_ || migration_complete_ || migration_failure_)
            return {};
        if (migration_ticket_)
        {
            const auto found =
                std::ranges::find(workspace_publications_, *migration_ticket_, &WorkspacePublication::ticket);
            if (found == workspace_publications_.end() || !found->result)
                return {};
            if (!std::holds_alternative<persistence::CommitReceipt>(*found->result))
            {
                migration_failure_ = applicationFailure("workspace.migration.publication", *found->result).value();
                migration_ticket_.reset();
                return {};
            }
            migration_ticket_.reset();
        }
        if (phase_ != EApplicationPhase::RUNNING || workspace_publications_.size() >= 16)
            return {};
        auto next = workspace_.continueMigration(*migration_);
        if (!next)
        {
            if (next.error().code != workspace::EWorkspaceError::BUSY)
                migration_failure_ = applicationFailure("workspace.migration", next.error()).value();
            return {};
        }
        if (!*next)
            migration_complete_ = true;
        else
        {
            migration_ticket_ = **next;
            workspace_publications_.push_back({"Migrate one legacy record", **next});
        }
        return {};
    }
}
