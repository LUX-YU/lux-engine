#include <algorithm>
#include <lux/engine/editor/application/RestoreWorkbench.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/storage/ProjectContentOpening.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <thread>

namespace lux::editor::application
{
    namespace
    {
        // These are old persisted data identifiers, not executable compatibility factories.
        views::ViewTypeId recoveryType(views::ViewTypeId type)
        {
            if (type == views::ViewTypeId{"lux.editor.scene.v1"})
            {
                return views::ViewTypeId{"lux.editor.scene.view"};
            }
            if (type == views::ViewTypeId{"lux.editor.material.v1"})
            {
                return views::ViewTypeId{"lux.editor.material"};
            }
            if (type == views::ViewTypeId{"lux.editor.flowforge.v1"})
            {
                return views::ViewTypeId{"lux.editor.flowforge"};
            }
            return type;
        }

        template <class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                {
                    code = EEditorError::BUSY;
                }
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                {
                    code = EEditorError::BUSY;
                }
            }
            if constexpr (requires { cause.session == sessions::ESessionError::BUSY; })
            {
                if (cause.session == sessions::ESessionError::BUSY)
                {
                    code = EEditorError::BUSY;
                }
            }
            if constexpr (requires { cause.retryable; })
            {
                if (cause.retryable)
                {
                    code = EEditorError::BUSY;
                }
            }
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    } // namespace
    struct RestoreWorkbench::Impl final
    {
        struct RecoveryPresentation final
        {
            extensions::ContributionSnapshot catalog;
            std::vector<RestoredView> items;
        };
        ProjectStorage& project_;
        persistence::IArtifactStore& files_;
        sessions::SessionStore& sessions_;
        sessions::SessionOpening& opening_;
        workspace::WorkspaceStore& workspace_;
        workspace::WorkspaceChanges& workspace_changes_;
        extensions::ContributionRegistry& contributions_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        std::optional<RecoveryPresentation> recovery_;
        Impl(
            ProjectStorage& project,
            persistence::IArtifactStore& files,
            sessions::SessionStore& sessions,
            sessions::SessionOpening& opening,
            workspace::WorkspaceStore& workspace,
            workspace::WorkspaceChanges& changes,
            extensions::ContributionRegistry& contributions
        )
            : project_(project), files_(files), sessions_(sessions), opening_(opening), workspace_(workspace),
              workspace_changes_(changes), contributions_(contributions)
        {
        }
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission() const
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "recovery.owner-thread"});
            }
            if (dispatching_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recovery.dispatch"});
            }
            return {};
        }
        EditorResult<void> captureCore(
            std::span<const desktop::WindowInfo> views,
            const extensions::ContributionSnapshot& catalog
        )
        {
            if (workspace_changes_.migrationPending())
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.migration"});
            }
            auto previous = workspace_.readRecovery();
            if (!previous && previous.error().code != workspace::EWorkspaceError::NOT_FOUND)
            {
                return failure("recovery.read", previous.error());
            }
            auto value = previous ? previous->value : workspace::RecoveryManifest{};
            const auto version = previous ? previous->target.expected_version : "missing";
            // Preserve unknown records and payload. Only the actual currently bound view entries are replaced.
            for (const auto& view : views)
            {
                if (view.content.sessions.empty())
                {
                    continue;
                }
                const auto factory = std::ranges::find_if(
                    catalog.ui().entries(),
                    [&](const auto& entry)
                    { return entry->descriptor().type == view.type.view() && entry->descriptor().restore_content; }
                );
                if (factory == catalog.ui().entries().end())
                {
                    continue; // Contextual auxiliary windows are layout entries, not content creation factories.
                }
                workspace::RecoveryEntry next{view.restore_key, view.type};
                for (const auto id : view.content.sessions)
                {
                    auto source = sessions_.describe(id);
                    if (!source)
                    {
                        return failure("recovery.source", source.error());
                    }
                    if (!source->binding)
                    {
                        return cxx::unexpected(EditorFailure{
                            EEditorError::INVALID_ARGUMENT,
                            "recovery.unbound",
                            0,
                            "Save unbound content before recording its recovery location."
                        });
                    }
                    if (view.content.primary == id)
                    {
                        next.primary = static_cast<std::uint32_t>(next.contents.size());
                    }
                    next.contents.push_back({"asset:" + uuids::to_string(source->binding->asset.uuid()), source->dirty}
                    );
                }
                auto found = std::ranges::find_if(
                    value.entries,
                    [&](const auto& entry) { return entry.restore_key == next.restore_key && entry.type == next.type; }
                );
                if (found == value.entries.end())
                {
                    value.entries.push_back(std::move(next));
                }
                else
                {
                    *found = std::move(next);
                }
            }
            return workspace_changes_.recordRecovery(value, version);
        }

        EditorResult<void> startCore()
        {
            if (workspace_changes_.migrationPending())
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.migration"});
            }
            if (recovery_ &&
                std::ranges::any_of(recovery_->items, [](const auto& item) { return item.opening.has_value(); }))
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "recovery.pending"});
            }
            auto stored = workspace_.readRecovery();
            if (!stored)
            {
                return failure("recovery.read", stored.error());
            }
            RecoveryPresentation next{contributions_.snapshot()};
            next.items.reserve(stored->value.entries.size());
            for (auto entry : stored->value.entries)
            {
                next.items.push_back({std::move(entry)});
            }
            recovery_ = std::move(next);
            return {};
        }
        EditorResult<void> updateCore(ERestorationProgress progress, Present present)
        {
            if (!recovery_)
            {
                return {};
            }
            for (auto& item : recovery_->items)
            {
                if (item.result)
                {
                    continue;
                }
                if (item.opening)
                {
                    auto status = opening_.status(*item.opening);
                    if (!status)
                    {
                        return failure("recovery.open.status", status.error());
                    }
                    const bool is_pending = status->stage == sessions::EOpenAssetStage::READING ||
                                            status->stage == sessions::EOpenAssetStage::PREPARING;
                    const bool is_reviewing = progress == ERestorationProgress::SUSPENDED;
                    if (is_pending || (status->stage == sessions::EOpenAssetStage::PUBLISHED && is_reviewing))
                    {
                        continue;
                    }
                    auto acknowledged = opening_.acknowledge(*item.opening);
                    if (!acknowledged)
                    {
                        return failure("recovery.acknowledge", acknowledged.error());
                    }
                    item.opening.reset();
                    item.sources.push_back(std::move(*status));
                    const auto& completed = item.sources.back();
                    if (completed.stage != sessions::EOpenAssetStage::PUBLISHED)
                    {
                        item.result.emplace(
                            completed.failure ? failure("recovery.open", *completed.failure)
                                              : cxx::unexpected(EditorFailure{EEditorError::CANCELLED, "recovery.open"})
                        );
                        continue;
                    }
                }
                if (progress != ERestorationProgress::ACTIVE)
                {
                    if (progress == ERestorationProgress::CLOSING)
                    {
                        item.result.emplace(
                            cxx::unexpected(EditorFailure{EEditorError::CLOSING, "recovery.presentation"})
                        );
                    }
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
                        item.result.emplace(cxx::unexpected(
                            EditorFailure{EEditorError::INVALID_ARGUMENT, "recovery.locator", 0, locator}
                        ));
                        continue;
                    }
                    const auto asset_id = asset::AssetId{*parsed};
                    const auto* asset = project_.asset(asset_id);
                    const auto factory =
                        asset ? recovery_->catalog.sessions().selectSource(asset->source_type, asset->source_version)
                              : sessions::SessionFactoryResult<std::shared_ptr<sessions::SessionFactoryEntry>>{
                                    cxx::unexpected(sessions::SessionFactoryFailure{
                                        sessions::ESessionFactoryError::NOT_FOUND,
                                        "recovery.asset"
                                    })
                                };
                    const bool matching =
                        factory && recovery_->catalog.ui()
                                       .selectContent(
                                           sessions::SessionKindId{std::string{(*factory)->descriptor().kind.name()}},
                                           recoveryType(item.entry.type)
                                       )
                                       .has_value();
                    if (!matching)
                    {
                        item.result.emplace(cxx::unexpected(EditorFailure{
                            EEditorError::MISSING_PROVIDER,
                            "recovery.type-or-asset",
                            0,
                            "Entry retained: its asset or exact content view type is unavailable."
                        }));
                        continue;
                    }
                    std::optional<EditorResult<sessions::OpenAssetId>> admitted;
                    auto prepare = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void>
                    {
                        admitted.emplace(openProjectContent(
                            project_,
                            files_,
                            opening_,
                            project_.reference(asset_id),
                            recovery_->catalog.sessions()
                        ));
                        return {};
                    };
                    auto guarded = contributions_.withSnapshot(prepare);
                    if (!guarded)
                    {
                        if (guarded.error().code != extensions::EContributionError::BUSY)
                        {
                            item.result.emplace(failure("recovery.catalog", guarded.error()));
                        }
                        continue;
                    }
                    if (!*admitted)
                    {
                        if (admitted->error().code != EEditorError::BUSY)
                        {
                            item.result.emplace(cxx::unexpected(admitted->error()));
                        }
                        continue;
                    }
                    item.opening = **admitted;
                    continue;
                }
                views::ViewContent association;
                for (const auto& content : item.sources)
                {
                    association.sessions.push_back(content.session);
                }
                if (item.entry.primary)
                {
                    association.primary = association.sessions[*item.entry.primary];
                }
                std::optional<EditorResult<lux::ui::PaneHandle>> displayed;
                auto prepare = [&](const extensions::ContributionSnapshot&) -> extensions::ContributionResult<void>
                {
                    displayed.emplace(
                        present(association, recovery_->catalog, item.entry.restore_key, recoveryType(item.entry.type))
                    );
                    return {};
                };
                auto guarded = contributions_.withSnapshot(prepare);
                if (!guarded)
                {
                    if (guarded.error().code != extensions::EContributionError::BUSY)
                    {
                        item.result.emplace(failure("recovery.view.catalog", guarded.error()));
                    }
                    continue;
                }
                if (!*displayed && displayed->error().code == EEditorError::BUSY)
                {
                    continue;
                }
                item.result = std::move(displayed);
            }
            return {};
        }
    };
    RestoreWorkbench::RestoreWorkbench(
        ProjectStorage& project,
        persistence::IArtifactStore& files,
        sessions::SessionStore& sessions,
        sessions::SessionOpening& opening,
        workspace::WorkspaceStore& workspace,
        workspace::WorkspaceChanges& changes,
        extensions::ContributionRegistry& contributions
    )
        : impl_(std::make_unique<Impl>(project, files, sessions, opening, workspace, changes, contributions))
    {
    }
    RestoreWorkbench::~RestoreWorkbench() = default;
    EditorResult<void> RestoreWorkbench::capture(std::span<const desktop::WindowInfo> views)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        const Impl::Dispatch scope{impl_->dispatching_};
        EditorResult<void> result;
        auto capture = [&](const extensions::ContributionSnapshot& catalog) -> extensions::ContributionResult<void>
        {
            result = impl_->captureCore(views, catalog);
            return {};
        };
        auto guarded = impl_->contributions_.withSnapshot(capture);
        return guarded ? std::move(result) : failure("recovery.catalog", guarded.error());
    }
    EditorResult<void> RestoreWorkbench::start()
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        const Impl::Dispatch scope{impl_->dispatching_};
        return impl_->startCore();
    }
    EditorResult<void> RestoreWorkbench::update(ERestorationProgress progress, Present present)
    {
        if (auto ready = impl_->admission(); !ready)
        {
            return ready;
        }
        const Impl::Dispatch scope{impl_->dispatching_};
        return impl_->updateCore(progress, present);
    }
    std::span<const RestoredView> RestoreWorkbench::items() const noexcept
    {
        return impl_->recovery_ ? std::span<const RestoredView>{impl_->recovery_->items}
                                : std::span<const RestoredView>{};
    }
} // namespace lux::editor::application
