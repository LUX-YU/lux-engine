#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <algorithm>
#include <random>

namespace lux::editor::application
{
    namespace
    {
        std::optional<EProjectAssetKind> assetKind(const sessions::SessionKindId& kind)
        {
            if (kind.name == "lux.editor.scene")
                return EProjectAssetKind::SCENE;
            if (kind.name == "lux.editor.material")
                return EProjectAssetKind::MATERIAL_GRAPH;
            if (kind.name == "lux.editor.flowforge")
                return EProjectAssetKind::FLOW_GRAPH;
            return {};
        }
        commands::CommandFailure saveFailure(const EditorFailure& error)
        {
            return {commands::ECommandError::DOMAIN_FAILURE, error.domain, error.reason, error.message};
        }
    }
    EditorResult<EditorApplication::Impl::PreparedSave> EditorApplication::Impl::prepareSave(
        commands::SessionTarget target,
        persistence::ESaveMode mode,
        std::string destination
    )
    {
        auto info = sessions_.describe(target.id);
        if (!info)
            return applicationFailure("save.session", info.error());
        if (!target.based_on || *target.based_on != info->current)
            return applicationFailure("save.source", sessions::ESessionError::STALE_CONTENT);
        if (save_reports_.size() >= 128)
            return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "save.reports"});
        const auto kind = assetKind(info->kind);
        if (!kind)
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "save.project.kind"});
        persistence::SaveRequest request{target.id, mode};
        ProjectAssetEntry entry;
        if (mode == persistence::ESaveMode::SAVE)
        {
            if (!info->binding)
                return applicationFailure("save.unbound", persistence::EPersistenceError::UNBOUND);
            const auto* existing = project_->asset(info->binding->asset);
            if (!existing)
                return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "save.catalog"});
            entry = *existing;
        }
        else
        {
            if (!validProjectPath(destination))
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_ARGUMENT, "save.destination"});
            auto resolved = files_.resolve(destination);
            if (!resolved)
                return applicationFailure("save.destination", resolved.error());
            // Naming another registered source is not permission to overwrite its identity.
            for (const auto& other : project_->manifest().assets)
            {
                auto physical = files_.resolve(other.source_path);
                if (!physical)
                    return applicationFailure("save.catalog.target", physical.error());
                if (physical->key == resolved->key)
                    return applicationFailure("save.destination.owned", persistence::EPersistenceError::CONFLICT);
            }
            std::mt19937 random{std::random_device{}()};
            request.asset = asset::AssetId{uuids::uuid_random_generator{random}()};
            request.destination = std::move(*resolved);
            entry = {request.asset, *kind, std::move(destination)};
            entry.mount_path = std::filesystem::u8path(entry.source_path).parent_path().generic_string();
        }
        return PreparedSave{std::move(request), std::move(entry)};
    }
    EditorResult<persistence::SaveId> EditorApplication::Impl::save(
        commands::SessionTarget target,
        persistence::ESaveMode mode,
        std::string destination
    )
    {
        if (auto ended = cancelContentPreview(target.id); !ended)
            return cxx::unexpected(ended.error());
        auto prepared = prepareSave(target, mode, std::move(destination));
        if (!prepared)
            return cxx::unexpected(prepared.error());
        auto accepted = saves_.requestSave(std::move(prepared->request));
        if (!accepted)
            return applicationFailure("save.admission", accepted.error());
        save_reports_.push_back({*accepted, std::move(prepared->asset)});
        if (std::ranges::find(pending_saves_, *accepted) == pending_saves_.end())
            pending_saves_.push_back(*accepted);
        return *accepted;
    }
    EditorResult<void> EditorApplication::Impl::cancelContentPreview(sessions::SessionId id)
    {
        for (auto& view : content_views_)
        {
            if (view.session != id)
                continue;
            if (view.scene)
            {
                auto ended = view.scene->cancel();
                if (!ended)
                    return applicationFailure("save.scene.preview", ended.error());
            }
            if (view.material)
            {
                auto ended = view.material->cancel();
                if (!ended)
                    return applicationFailure("save.material.preview", ended.error());
            }
            if (view.flow)
            {
                auto ended = view.flow->cancel();
                if (!ended)
                    return applicationFailure("save.flow.preview", ended.error());
            }
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::askSave(commands::SessionTarget target, persistence::ESaveMode mode)
    {
        if (save_question_ || phase_ != EApplicationPhase::RUNNING)
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.question"});
        auto info = sessions_.describe(target.id);
        if (!info)
            return applicationFailure("save.question.source", info.error());
        if (!target.based_on || *target.based_on != info->current)
            return applicationFailure("save.question.source", sessions::ESessionError::STALE_CONTENT);
        const char* suffix = info->kind.name == "lux.editor.scene"      ? ".scene"
                             : info->kind.name == "lux.editor.material" ? ".material"
                                                                        : ".flow";
        auto question = desktop::ReviewView::create(
            messages_.dispatcherRef(),
            lux::ui::PaneId{"save-destination"},
            {next_review_++,
             mode == persistence::ESaveMode::EXPORT_COPY ? "Export Copy" : "Save As",
             "Choose a project-relative source path. The captured target is checked again before saving.",
             {desktop::EReviewChoice::SAVE, desktop::EReviewChoice::CANCEL},
             "Source path",
             std::string("Content/Untitled") + suffix}
        );
        if (!question)
            return applicationFailure("save.question.create", question.error());
        views::DetachedView candidate{contracts::CodeLease::builtin(), std::move(*question)};
        auto shown = adopt(candidate, "save-destination");
        if (!shown)
            return cxx::unexpected(shown.error());
        save_question_ = SaveQuestion{target, mode, *shown};
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveSaveAnswer()
    {
        if (!save_question_)
            return {};
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = desktop_->views().withView(save_question_->view, read);
        if (!borrowed)
            return applicationFailure("save.question.read", borrowed.error());
        if (!answer)
            return {};
        auto prepared = desktop_->views().prepareClose(std::span{&save_question_->view, 1});
        if (!prepared)
            return applicationFailure("save.question.close", prepared.error());
        if (answer->choice == desktop::EReviewChoice::SAVE)
        {
            auto admitted = save(save_question_->target, save_question_->mode, answer->text);
            if (!admitted)
            {
                const auto* persistence = std::any_cast<persistence::PersistenceFailure>(&admitted.error().cause);
                const auto* session = std::any_cast<sessions::ESessionError>(&admitted.error().cause);
                const bool temporary =
                    admitted.error().code == EEditorError::BUSY ||
                    (persistence && (persistence->code == persistence::EPersistenceError::BUSY ||
                                     persistence->code == persistence::EPersistenceError::WRITER_ACTIVE)) ||
                    (session && *session == sessions::ESessionError::BUSY);
                if (temporary)
                    return {}; // Retain the answered draft and exact source until admission is available.
                auto reject = [&](lux::ui::Pane& pane) {
                    static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                        admitted.error().domain + ": " + admitted.error().message +
                        "\nThe original content and target were retained. Correct the path, or Cancel and start again."
                    );
                };
                auto displayed = desktop_->views().withView(save_question_->view, reject);
                if (!displayed)
                    return applicationFailure("save.question.error", displayed.error());
                return {};
            }
        }
        auto closed = desktop_->views().commit(*prepared);
        if (!closed)
            return applicationFailure("save.question.commit", closed.error());
        save_question_.reset();
        return {};
    }
    EditorResult<void> EditorApplication::Impl::rememberSave(persistence::SaveId id)
    {
        if (std::ranges::find(save_reports_, id, &SavePresentation::id) != save_reports_.end())
            return {};
        auto status = saves_.status(id);
        if (!status)
            return applicationFailure("save.status", status.error());
        // Resolve from the immutable physical destination (including a reviewed unbound close),
        // so closing or rebinding the Session cannot relabel a late disk fact.
        auto remember = [&](const auto& entries) -> EditorResult<bool> {
            for (const auto& entry : entries)
            {
                auto target = files_.resolve(entry.source_path);
                if (!target)
                    return applicationFailure("save.catalog.target", target.error());
                if (target->key == status->target.key)
                {
                    save_reports_.push_back({id, entry});
                    return true;
                }
            }
            return false;
        };
        auto existing = remember(project_->manifest().assets);
        if (!existing)
            return cxx::unexpected(existing.error());
        if (*existing)
            return {};
        auto closing = remember(close_destinations_);
        if (!closing)
            return cxx::unexpected(closing.error());
        if (*closing)
            return {};
        return cxx::unexpected(EditorFailure{EEditorError::SOURCE_FAILURE, "save.catalog.source"});
    }
    EditorResult<void> EditorApplication::Impl::settleSaves()
    {
        for (auto iterator = pending_saves_.begin(); iterator != pending_saves_.end();)
        {
            const auto id = *iterator;
            auto remembered = rememberSave(id);
            if (!remembered)
                return remembered;
            auto& report = *std::ranges::find(save_reports_, id, &SavePresentation::id);
            auto status = saves_.status(id);
            if (!status)
                return applicationFailure("save.status", status.error());
            if (status->stage != persistence::ESaveStage::TERMINAL)
            {
                ++iterator;
                continue;
            }
            const auto* published =
                status->outcome ? std::get_if<persistence::CommitReceipt>(&status->outcome->publication) : nullptr;
            if (published && !report.failure)
            {
                if (!report.catalog)
                {
                    // Preserve any newer compiled package information while applying this source publication.
                    if (const auto* existing = project_->asset(report.asset.id))
                        report.asset = *existing;
                    report.asset.source_digest = published->version;
                    ProjectUpdate update;
                    update.assets.push_back(report.asset);
                    auto candidate = project_->preparePublication(update);
                    if (!candidate)
                    {
                        if (candidate.error().code == EEditorError::BUSY)
                        {
                            ++iterator;
                            continue;
                        }
                        report.failure = candidate.error();
                    }
                    else
                    {
                        auto encoded = encodeProjectManifest(candidate->manifest);
                        if (!encoded)
                            report.failure = applicationFailure("catalog.encode", encoded.error()).value();
                        else
                        {
                            auto target = files_.resolve(candidate->manifest_path);
                            if (!target)
                                report.failure = applicationFailure("catalog.target", target.error()).value();
                            else
                            {
                                target->expected_version = candidate->before_manifest_digest;
                                const auto bytes = std::as_bytes(std::span{encoded->data(), encoded->size()});
                                auto ticket = persistence::publishEncodedArtifact(
                                    writes_,
                                    std::move(*target),
                                    persistence::EncodedArtifact{std::vector<std::byte>{bytes.begin(), bytes.end()}}
                                );
                                if (!ticket)
                                    report.failure = applicationFailure("catalog.publish", ticket.error()).value();
                                else
                                {
                                    report.catalog = std::move(*candidate);
                                    report.catalog_ticket = *ticket;
                                }
                            }
                        }
                    }
                }
                if (report.catalog_ticket)
                {
                    auto written = writes_.status(*report.catalog_ticket);
                    if (!written)
                        return applicationFailure("catalog.status", written.error());
                    // Unknown is still a live lane and still owns the Project reservation.
                    if (written->stage != persistence::EWriteStage::TERMINAL)
                    {
                        ++iterator;
                        continue;
                    }
                    if (auto* receipt = std::get_if<persistence::CommitReceipt>(&*written->outcome))
                    {
                        ProjectPublicationReceipt adopted{
                            report.catalog->manifest,
                            receipt->version,
                            1,
                            {},
                            {{report.asset.source_path, published->version}},
                            {}
                        };
                        auto result = project_->adoptPublication(*report.catalog, adopted);
                        if (!result)
                            report.failure = std::move(result.error());
                    }
                    else
                        report.failure = applicationFailure("catalog.publication", *written->outcome).value();
                    auto acknowledged = writes_.acknowledge(*report.catalog_ticket);
                    if (!acknowledged)
                        return applicationFailure("catalog.acknowledge", acknowledged.error());
                    report.catalog_ticket.reset();
                    report.catalog.reset();
                }
            }
            report.result = status->outcome;
            auto acknowledged = saves_.acknowledge(id);
            if (!acknowledged)
                return applicationFailure("save.acknowledge", acknowledged.error());
            iterator = pending_saves_.erase(iterator);
        }
        return {};
    }
    void EditorApplication::Impl::installSaveCommands(extensions::ContributionDraft& draft)
    {
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandId{"lux.editor.reload"},
                "Reload",
                "File",
                "",
                commands::ECommandScope::SESSION
            },
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !reload_question_};
            },
            [this](const commands::CommandInvocation& invocation
            ) -> commands::CommandResult<commands::DispatchReceipt> {
                auto accepted = askReload(std::get<commands::SessionTarget>(invocation.target()));
                if (!accepted)
                    return cxx::unexpected(saveFailure(accepted.error()));
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        for (auto mode :
             {persistence::ESaveMode::SAVE, persistence::ESaveMode::SAVE_AS, persistence::ESaveMode::EXPORT_COPY})
        {
            const bool ordinary = mode == persistence::ESaveMode::SAVE;
            draft.commands.push_back(std::make_shared<commands::CommandEntry>(
                contracts::CodeLease::builtin(),
                commands::CommandDescriptor{
                    commands::CommandId{
                        ordinary                                  ? "lux.editor.save"
                        : mode == persistence::ESaveMode::SAVE_AS ? "lux.editor.save-as"
                                                                  : "lux.editor.export-copy"
                    },
                    ordinary                                  ? "Save"
                    : mode == persistence::ESaveMode::SAVE_AS ? "Save As"
                                                              : "Export Copy",
                    "File",
                    ordinary ? "Ctrl+S" : "",
                    commands::ECommandScope::SESSION
                },
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                    return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !save_question_};
                },
                [this, mode](const commands::CommandInvocation& invocation
                ) -> commands::CommandResult<commands::DispatchReceipt> {
                    const auto target = std::get<commands::SessionTarget>(invocation.target());
                    auto info = sessions_.describe(target.id);
                    if (!info)
                        return cxx::unexpected(saveFailure(applicationFailure("save.session", info.error()).value()));
                    if (mode == persistence::ESaveMode::SAVE && info->binding)
                    {
                        auto admitted = save(target, mode);
                        if (!admitted)
                            return cxx::unexpected(saveFailure(admitted.error()));
                        return commands::DispatchReceipt{commands::AcceptedOperation{"save", admitted->value}};
                    }
                    auto asked =
                        askSave(target, mode == persistence::ESaveMode::SAVE ? persistence::ESaveMode::SAVE_AS : mode);
                    if (!asked)
                        return cxx::unexpected(saveFailure(asked.error()));
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        }
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{commands::CommandId{"lux.editor.save-all"}, "Save All", "File"},
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto ids = sessions_.snapshotIds();
                if (!ids)
                    return cxx::unexpected(saveFailure(applicationFailure("save-all.contents", ids.error()).value()));
                if (save_reports_.size() + ids->size() > 128)
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "save-all.results"});
                for (auto id : *ids)
                    if (auto ended = cancelContentPreview(id); !ended)
                        return cxx::unexpected(saveFailure(ended.error()));
                auto operation = sessions::SaveAllOperation::begin(sessions_, saves_);
                if (!operation)
                    return cxx::unexpected(saveFailure(applicationFailure("save-all", operation.error()).value()));
                for (const auto& entry : operation->entries())
                    if (entry.save)
                        pending_saves_.push_back(*entry.save);
                save_all_ = std::move(*operation);
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }
}
