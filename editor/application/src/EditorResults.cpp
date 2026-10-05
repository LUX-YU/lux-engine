#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>

namespace lux::editor::application
{
    using namespace lux::editor::project;
    EditorResult<void> EditorApplication::Impl::receiveResultIntent()
    {
        if (!result_intent_)
        {
            return {};
        }
        const auto intent = std::exchange(result_intent_, {});
        return std::visit(
            [&](const auto& action) -> EditorResult<void>
            {
                using Action = std::decay_t<decltype(action)>;
                if constexpr (std::same_as<Action, AcknowledgeMaintenance>)
                {
                    maintenance_failure_.reset();
                }
                else if constexpr (std::same_as<Action, AcknowledgeArtifact>)
                {
                    return content_saving_->acknowledgeArtifact(action.target);
                }
                else if constexpr (std::same_as<Action, RetryArtifact>)
                {
                    return content_saving_->retryArtifact(action.target);
                }
                else if constexpr (std::same_as<Action, AbandonArtifact>)
                {
                    return content_saving_->abandonArtifact(action.target);
                }
                else if constexpr (std::same_as<Action, AcknowledgeSave>)
                {
                    return content_saving_->acknowledge(action.target);
                }
                else if constexpr (std::same_as<Action, CancelSave>)
                {
                    auto cancelled = saves_->requestCancel(action.target);
                    if (!cancelled)
                    {
                        return applicationFailure("save.cancel", cancelled.error());
                    }
                }
                else if constexpr (std::same_as<Action, ReconcilePublication>)
                {
                    auto reconciled = writes_->reconcile(action.target, *files_);
                    if (!reconciled)
                    {
                        return applicationFailure("publication.reconcile", reconciled.error());
                    }
                }
                else if constexpr (std::same_as<Action, AcknowledgeReload>)
                {
                    std::erase_if(
                        reloads_,
                        [&](const auto& reload) { return reload.source == action.target && reload.result.has_value(); }
                    );
                }
                else if constexpr (std::same_as<Action, AcknowledgeRunFailure>)
                {
                    if (playback_) return playback_->acknowledgeFailure(action.target);
                }
                else if constexpr (std::same_as<Action, AcknowledgeStep>)
                {
                    if (!playback_)
                    {
                        return applicationFailure("run.step.acknowledge", scene::ERunError::INVALID_ID);
                    }
                    return playback_->acknowledgeStep(action.target);
                }
                else if constexpr (std::same_as<Action, AcknowledgeModel>)
                {
                    if (model_placements_)
                    {
                        return model_placements_->acknowledge(action.target);
                    }
                }
                else if constexpr (std::same_as<Action, CancelModel>)
                {
                    if (model_placements_)
                    {
                        return model_placements_->cancel(action.target);
                    }
                }
                else if constexpr (std::same_as<Action, ShowContent>)
                {
                    const auto target = action.target;
                    auto current = sessions_->describe(target.session);
                    if (!current)
                    {
                        return applicationFailure("content.show", current.error());
                    }
                    if (current->current != target)
                    {
                        return applicationFailure("content.show", sessions::ESessionError::STALE_CONTENT);
                    }
                    auto shown = show(target.session, false);
                    if (!shown)
                    {
                        return cxx::unexpected(shown.error());
                    }
                }
                else if constexpr (std::same_as<Action, SaveContentAs>)
                {
                    const auto target = action.target;
                    return askSave({target.session, target}, persistence::ESaveMode::SAVE_AS);
                }
                else if constexpr (std::same_as<Action, AcknowledgeSaveAll>)
                {
                    return content_saving_->acknowledgeSaveAll();
                }
                else
                {
                    static_assert(sizeof(Action) == 0, "Every result action requires an explicit receiver");
                }
                return {};
            },
            *intent
        );
    }
    EditorResult<project::ResultsSnapshot> EditorApplication::Impl::observeResults()
    {
        project::ResultsSnapshot snapshot;
        const auto session_key = [](sessions::SessionId id)
        { return std::to_string(id.domain) + "/" + std::to_string(id.slot) + "/" + std::to_string(id.generation); };
        const auto row = [&](std::string key) -> project::ResultRow&
        { return snapshot.sections.back().rows.emplace_back(project::ResultRow{std::move(key)}); };
        const auto diagnostic = [](project::ResultRow& to, const EditorFailure& error)
        { to.messages.push_back(error.domain + ": " + error.message); };
        const auto publication = [&](project::ResultRow& to, persistence::WriteTicket ticket) -> EditorResult<void>
        {
            auto status = writes_->status(ticket);
            if (!status)
            {
                return applicationFailure("results.publication", status.error());
            }
            if (status->stage == persistence::EWriteStage::UNKNOWN)
            {
                to.messages.emplace_back("Publication unknown; this physical target remains reserved.");
                to.actions.push_back({"Reconcile disk result", ReconcilePublication{ticket}});
            }
            return {};
        };
        snapshot.sections.push_back({"Diagnostics"});
        if (result_failure_)
        {
            diagnostic(row("request"), *result_failure_);
        }
        if (maintenance_failure_)
        {
            auto& to = row("maintenance");
            diagnostic(to, *maintenance_failure_);
            to.actions.push_back({"Acknowledge maintenance error", AcknowledgeMaintenance{}});
        }
        snapshot.sections.push_back({"Open content (including content without a window)"});
        auto ids = sessions_->snapshotIds();
        if (!ids)
        {
            return applicationFailure("results.contents", ids.error());
        }
        for (auto id : *ids)
        {
            auto info = sessions_->describe(id);
            if (!info)
            {
                return applicationFailure("results.content", info.error());
            }
            auto& to = row(session_key(id));
            to.messages.push_back(info->kind.name + (info->dirty ? " *" : ""));
            if (info->binding)
            {
                to.messages.push_back(info->binding->location);
            }
            to.actions.push_back({"Show", ShowContent{info->current}});
            if (!info->binding)
            {
                to.actions.push_back({"Save As", SaveContentAs{info->current}});
            }
        }
        snapshot.sections.push_back({"Run results"});
        auto run_reports = playback_ ? playback_->reports() : EditorResult<std::vector<scene::RunPresentationInfo>>{};
        if (!run_reports)
        {
            return cxx::unexpected(run_reports.error());
        }
        for (const auto& run : *run_reports)
        {
            auto& to = row(std::to_string(run.start.domain) + "/" + std::to_string(run.start.serial));
            if (run.failure)
            {
                diagnostic(to, *run.failure);
                if (!run.preparing && !run.run)
                {
                    to.actions.push_back({"Acknowledge failed Run", AcknowledgeRunFailure{run.start}});
                }
            }
            for (const auto& step : run.steps)
            {
                const auto ticket = step.ticket;
                const auto name = "Step " + std::to_string(ticket.step.serial);
                to.messages.push_back(name + ": " + std::to_string(static_cast<unsigned>(step.status.state)));
                const bool completed = step.status.state == lux::scene::ESceneStepState::COMPLETED ||
                                       step.status.state == lux::scene::ESceneStepState::FAILED ||
                                       step.status.state == lux::scene::ESceneStepState::CANCELLED;
                if (completed)
                {
                    to.actions.push_back({"Acknowledge " + name, AcknowledgeStep{ticket}});
                }
            }
        }
        snapshot.sections.push_back({"Compiled publications"});
        auto artifacts = content_saving_->artifactReports();
        if (!artifacts)
        {
            return cxx::unexpected(std::move(artifacts.error()));
        }
        for (const auto& report : *artifacts)
        {
            auto& to = row(std::to_string(report.id));
            if (report.admitted)
            {
                to.messages.push_back(report.path);
            }
            if (const auto* failed = std::get_if<EditorFailure>(&report.status))
            {
                diagnostic(to, *failed);
                if (report.admitted)
                {
                    to.actions.push_back({"Retry retained publication", RetryArtifact{report.id}});
                    to.actions.push_back({"Abandon remaining publication", AbandonArtifact{report.id}});
                }
            }
            else if (std::holds_alternative<PublicationSucceeded>(report.status))
            {
                to.messages.emplace_back("Package and catalog published. Author save baseline is unchanged.");
            }
            else if (const auto* abandoned = std::get_if<PublicationAbandoned>(&report.status))
            {
                to.messages.push_back(
                    "Publication stopped; " + std::to_string(abandoned->published_files) +
                    " files already published remain on disk."
                );
            }
            if (report.terminal)
            {
                to.actions.push_back({"Acknowledge publication", AcknowledgeArtifact{report.id}});
            }
        }
        snapshot.sections.push_back({"Save results"});
        for (const auto& report : content_saving_->reports())
        {
            auto& to = row(std::to_string(report.id.value));
            to.messages.push_back(report.asset.source_path);
            if (report.result)
            {
                const auto& outcome = report.result->publication;
                to.messages.push_back(
                    std::string("Disk: ") +
                    (std::holds_alternative<persistence::CommitReceipt>(outcome) ? "published" : "not published") +
                    "; baseline adoption: " + std::to_string(static_cast<unsigned>(report.result->adoption))
                );
                if (const auto* failed = std::get_if<persistence::NotPublished>(&outcome))
                {
                    to.messages.push_back(failed->failure.detail);
                }
                if (report.failure)
                {
                    diagnostic(to, *report.failure);
                }
                to.actions.push_back({"Acknowledge result", AcknowledgeSave{report.id}});
            }
            else
            {
                auto status = saves_->status(report.id);
                if (!status)
                {
                    return applicationFailure("results.save", status.error());
                }
                to.messages.push_back("Accepted save, stage " + std::to_string(static_cast<unsigned>(status->stage)));
                if (auto observed = publication(to, status->ticket); !observed)
                {
                    return cxx::unexpected(observed.error());
                }
                to.actions.push_back({"Cancel before publication", CancelSave{report.id}});
                if (report.catalog_ticket)
                {
                    if (auto observed = publication(to, *report.catalog_ticket); !observed)
                    {
                        return cxx::unexpected(observed.error());
                    }
                }
            }
        }
        if (content_saving_->hasSaveAll())
        {
            snapshot.sections.push_back({"Save All fixed set"});
            for (const auto& entry : content_saving_->saveAllEntries())
            {
                auto& to = row(session_key(entry.session));
                to.messages.push_back(
                    "Content " + to.key + ": " +
                    (entry.already_clean ? "already clean"
                     : entry.save        ? "accepted (see save result)"
                                         : "not admitted")
                );
                if (entry.failure)
                {
                    to.messages.push_back(entry.failure->domain + ": " + entry.failure->detail);
                }
            }
            auto& to = row("report");
            to.messages.emplace_back("Unbound content: use Save As above. Other accepted saves continue independently."
            );
            to.actions.push_back({"Acknowledge Save All report", AcknowledgeSaveAll{}});
        }
        snapshot.sections.push_back({"Model insertion"});
        for (const auto& model :
             model_placements_ ? model_placements_->reports() : std::span<const scene::ModelPlacementReport>{})
        {
            auto& to = row(std::to_string(model.id));
            const auto state = model.result && *model.result   ? "inserted"
                               : model.result || model.failure ? "not inserted"
                               : model.cancel_requested        ? "cancelling; waiting for completion"
                                                               : "loading / waiting for the target gate";
            to.messages.push_back("Content " + session_key(model.placement.target.id()) + ": " + state);
            if (model.failure)
            {
                diagnostic(to, *model.failure);
            }
            if (model.result && !*model.result)
            {
                std::visit(
                    [&](const auto& error)
                    {
                        using Error = std::decay_t<decltype(error)>;
                        if constexpr (std::same_as<Error, scene::SceneEditError>)
                        {
                            to.messages.push_back(
                                "Scene edit rejected (" + std::to_string(static_cast<unsigned>(error.code)) +
                                "); the captured target was not rebased."
                            );
                        }
                        else if constexpr (std::same_as<Error, process::TaskCancelled>)
                        {
                            to.messages.emplace_back("Cancelled; no author edit was committed.");
                        }
                        else
                        {
                            to.messages.emplace_back("Model read or dependency validation failed; source retained.");
                        }
                    },
                    model.result->error().cause
                );
            }
            if (model.result || model.failure)
            {
                to.actions.push_back({"Acknowledge insertion", AcknowledgeModel{model.id}});
            }
            else
            {
                to.actions.push_back({"Cancel insertion", CancelModel{model.id}});
            }
        }
        snapshot.sections.push_back({"Reload results"});
        for (std::size_t index{}; index < reloads_.size(); ++index)
        {
            const auto& reload = reloads_[index];
            auto& to = row(session_key(reload.source.session) + "/" + std::to_string(index));
            to.messages.emplace_back(
                !reload.result   ? "reading / preparing"
                : *reload.result ? "reloaded"
                                 : "original content retained"
            );
            if (reload.result)
            {
                if (!*reload.result)
                {
                    to.messages.push_back(reload.result->error().domain + ": " + reload.result->error().detail);
                }
                to.actions.push_back({"Acknowledge reload", AcknowledgeReload{reload.source}});
            }
        }
        return snapshot;
    }

    void EditorApplication::Impl::installResultView(extensions::ContributionDraft& draft)
    {
        // An application composition view, not another operation owner. It records button intents only;
        // service calls and structural changes run after Root returns from draw/update.
        results_observe_ = [this] { return observeResults(); };
        results_request_ = [this](VResultIntent intent) -> EditorResult<void>
        {
            if (result_intent_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "result.intent.capacity"});
            }
            result_intent_ = std::move(intent);
            return {};
        };
        draft.ui.push_back(desktop::UiEntry::bind<project::kResultsView>(object::CodeLease::builtin()));
        draft.commands.push_back(project::makeResultsCommand(
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            toolOpening()
        ));
    }
} // namespace lux::editor::application
