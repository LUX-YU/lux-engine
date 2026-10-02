#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <algorithm>

namespace lux::editor::application
{
    EditorResult<void> EditorApplication::Impl::receiveResultIntent()
    {
        if (!result_intent_)
            return {};
        const auto intent = std::exchange(result_intent_, {});
        return std::visit([&](const auto& action) -> EditorResult<void> {
            using Action = std::decay_t<decltype(action)>;
            if constexpr (std::same_as<Action, AcknowledgeMaintenance>)
            {
                maintenance_failure_.reset();
            }
            else if constexpr (std::same_as<Action, AcknowledgeArtifact>)
            {
                std::erase_if(artifacts_, [&](const auto& entry) {
                    return entry.id == action.target && entry.settled;
                });
            }
            else if constexpr (std::same_as<Action, AcknowledgeSave>)
            {
                std::erase_if(save_reports_, [&](const auto& report) {
                    const bool is_target = report.id == action.target;
                    const bool is_complete = report.result.has_value();
                    const bool is_pending = std::ranges::find(pending_saves_, report.id) != pending_saves_.end();
                    return is_target && is_complete && !is_pending;
                });
            }
            else if constexpr (std::same_as<Action, CancelSave>)
            {
                auto cancelled = saves_.requestCancel(action.target);
                if (!cancelled)
                    return applicationFailure("save.cancel", cancelled.error());
            }
            else if constexpr (std::same_as<Action, ReconcilePublication>)
            {
                auto reconciled = writes_.reconcile(action.target, files_);
                if (!reconciled)
                    return applicationFailure("publication.reconcile", reconciled.error());
            }
            else if constexpr (std::same_as<Action, AcknowledgeReload>)
            {
                std::erase_if(reloads_, [&](const auto& reload) {
                    return reload.source == action.target && reload.result.has_value();
                });
            }
            else if constexpr (std::same_as<Action, AcknowledgeRunFailure>)
            {
                std::erase_if(run_presentations_, [&](const auto& run) {
                    const bool is_target = run.start == action.target;
                    const bool is_failed = run.failure.has_value();
                    const bool is_released = !run.preparing && !run.run;
                    return is_target && is_failed && is_released;
                });
            }
            else if constexpr (std::same_as<Action, AcknowledgeStep>)
            {
                const auto ticket = action.target;
                auto acknowledged = runs_.acknowledgeStep(ticket);
                if (!acknowledged)
                    return applicationFailure("run.step.acknowledge", acknowledged.error());
                for (auto& run : run_presentations_)
                    if (run.run == ticket.run)
                        std::erase(run.steps, ticket);
            }
            else if constexpr (std::same_as<Action, AcknowledgeModel>)
            {
                std::erase_if(model_placements_, [&](const auto& model) {
                    const bool is_target = model.id == action.target;
                    const bool has_result = model.result || model.failure;
                    return is_target && !model.operation && has_result;
                });
            }
            else if constexpr (std::same_as<Action, CancelModel>)
            {
                for (auto& model : model_placements_)
                    if (model.id == action.target)
                        model.cancel_requested = true;
            }
            else if constexpr (std::same_as<Action, ShowContent>)
            {
                const auto target = action.target;
                auto current = sessions_.describe(target.session);
                if (!current)
                    return applicationFailure("content.show", current.error());
                if (current->current != target)
                    return applicationFailure("content.show", sessions::ESessionError::STALE_CONTENT);
                auto shown = show(target.session, false);
                if (!shown)
                    return cxx::unexpected(shown.error());
            }
            else if constexpr (std::same_as<Action, SaveContentAs>)
            {
                const auto target = action.target;
                return askSave({target.session, target}, persistence::ESaveMode::SAVE_AS);
            }
            else if constexpr (std::same_as<Action, AcknowledgeSaveAll>)
            {
                save_all_.reset(); // The accepted SaveIds remain in their original operation/report owners.
            }
            else
                static_assert(sizeof(Action) == 0, "Every result action requires an explicit receiver");
            return {};
        }, *intent);
    }
    void EditorApplication::Impl::installResultView(extensions::ContributionDraft& draft)
    {
        // An application composition view, not another operation owner. It records button intents only;
        // service calls and structural changes run after Root returns from draw/update.
        class ResultsPane final : public lux::ui::Pane
        {
            struct Content final : lux::ui::Element
            {
                Impl& app_;
                Content(ResultsPane& pane, Impl& app) : Element(pane, lux::ui::ElementId{"results"}), app_(app)
                {
                    setStretch({1, 1});
                }
                void draw() noexcept override
                {
                    auto button = [&](const char* label, VResultIntent intent) {
                        ImGui::BeginDisabled(app_.result_intent_.has_value());
                        if (ImGui::Button(label))
                            app_.result_intent_ = std::move(intent);
                        ImGui::EndDisabled();
                    };
                    auto publication = [&](persistence::WriteTicket ticket) {
                        auto status = app_.writes_.status(ticket);
                        if (!status)
                            return;
                        if (status->stage == persistence::EWriteStage::UNKNOWN)
                        {
                            ImGui::TextUnformatted("Publication unknown; this physical target remains reserved.");
                            button("Reconcile disk result", ReconcilePublication{ticket});
                        }
                    };
                    if (app_.result_failure_)
                        ImGui::TextWrapped(
                            "%s: %s",
                            app_.result_failure_->domain.c_str(),
                            app_.result_failure_->message.c_str()
                        );
                    if (app_.maintenance_failure_)
                    {
                        ImGui::TextWrapped(
                            "%s: %s",
                            app_.maintenance_failure_->domain.c_str(),
                            app_.maintenance_failure_->message.c_str()
                        );
                        button("Acknowledge maintenance error", AcknowledgeMaintenance{});
                    }
                    ImGui::SeparatorText("Open content (including content without a window)");
                    auto ids = app_.sessions_.snapshotIds();
                    if (!ids)
                        ImGui::TextUnformatted("Content temporarily unavailable; no empty-list inference.");
                    else
                        for (auto id : *ids)
                        {
                            auto info = app_.sessions_.describe(id);
                            if (!info)
                                continue;
                            ImGui::PushID(static_cast<int>(id.slot));
                            ImGui::Text("%s%s", info->kind.name.c_str(), info->dirty ? " *" : "");
                            if (info->binding)
                                ImGui::TextWrapped("%s", info->binding->location.c_str());
                            button("Show", ShowContent{info->current});
                            if (!info->binding)
                            {
                                ImGui::SameLine();
                                button("Save As", SaveContentAs{info->current});
                            }
                            ImGui::PopID();
                        }
                    ImGui::SeparatorText("Run results");
                    for (const auto& run : app_.run_presentations_)
                    {
                        ImGui::PushID(static_cast<int>(run.start.serial));
                        if (run.failure)
                        {
                            ImGui::TextWrapped("%s: %s", run.failure->domain.c_str(), run.failure->message.c_str());
                            if (!run.preparing && !run.run)
                                button("Acknowledge failed Run", AcknowledgeRunFailure{run.start});
                        }
                        for (const auto& ticket : run.steps)
                        {
                            ImGui::PushID(static_cast<int>(ticket.step.serial));
                            auto step = app_.runs_.stepStatus(ticket);
                            if (step)
                            {
                                ImGui::Text(
                                    "Step %llu: %u",
                                    static_cast<unsigned long long>(ticket.step.serial),
                                    static_cast<unsigned>(step->state)
                                );
                                const bool completed = step->state == lux::scene::ESceneStepState::COMPLETED ||
                                                       step->state == lux::scene::ESceneStepState::FAILED ||
                                                       step->state == lux::scene::ESceneStepState::CANCELLED;
                                if (completed)
                                    button("Acknowledge step", AcknowledgeStep{ticket});
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                    ImGui::SeparatorText("Compiled publications");
                    for (const auto& report : app_.artifacts_)
                    {
                        ImGui::PushID(static_cast<int>(report.id));
                        ImGui::TextWrapped("%s", report.asset.cooked_path.c_str());
                        if (report.failure)
                            ImGui::TextWrapped(
                                "%s: %s",
                                report.failure->domain.c_str(),
                                report.failure->message.c_str()
                            );
                        if (report.settled)
                        {
                            ImGui::TextUnformatted(
                                report.result && std::holds_alternative<persistence::CommitReceipt>(*report.result)
                                    ? "Package published. Author save baseline is unchanged."
                                    : "Publication rejected or failed."
                            );
                            button("Acknowledge publication", AcknowledgeArtifact{report.id});
                        }
                        else
                        {
                            if (report.ticket)
                                publication(*report.ticket);
                            if (report.catalog_ticket)
                                publication(*report.catalog_ticket);
                        }
                        ImGui::PopID();
                    }
                    ImGui::SeparatorText("Save results");
                    for (const auto& report : app_.save_reports_)
                    {
                        ImGui::PushID(static_cast<int>(report.id.value));
                        ImGui::TextWrapped("%s", report.asset.source_path.c_str());
                        if (report.result)
                        {
                            const auto& outcome = report.result->publication;
                            ImGui::Text(
                                "Disk: %s; baseline adoption: %u",
                                std::holds_alternative<persistence::CommitReceipt>(outcome) ? "published"
                                                                                            : "not published",
                                static_cast<unsigned>(report.result->adoption)
                            );
                            if (const auto* failed = std::get_if<persistence::NotPublished>(&outcome))
                                ImGui::TextWrapped("%s", failed->failure.detail.c_str());
                            if (report.failure)
                                ImGui::TextWrapped(
                                    "Catalog: %s: %s",
                                    report.failure->domain.c_str(),
                                    report.failure->message.c_str()
                                );
                            button("Acknowledge result", AcknowledgeSave{report.id});
                        }
                        else
                        {
                            auto status = app_.saves_.status(report.id);
                            if (status)
                            {
                                ImGui::Text("Accepted save, stage %u", static_cast<unsigned>(status->stage));
                                publication(status->ticket);
                                button("Cancel before publication", CancelSave{report.id});
                            }
                            if (report.catalog_ticket)
                                publication(*report.catalog_ticket);
                        }
                        ImGui::PopID();
                    }
                    if (app_.save_all_)
                    {
                        ImGui::SeparatorText("Save All fixed set");
                        for (const auto& entry : app_.save_all_->entries())
                        {
                            ImGui::Text(
                                "Content %u: %s",
                                entry.session.slot,
                                entry.already_clean ? "already clean"
                                : entry.save        ? "accepted (see save result)"
                                                    : "not admitted"
                            );
                            if (entry.failure)
                                ImGui::TextWrapped(
                                    "%s: %s",
                                    entry.failure->domain.c_str(),
                                    entry.failure->detail.c_str()
                                );
                        }
                        ImGui::TextUnformatted(
                            "Unbound content: use Save As above. Other accepted saves continue independently."
                        );
                        button("Acknowledge Save All report", AcknowledgeSaveAll{});
                    }
                    ImGui::SeparatorText("Model insertion");
                    for (const auto& model : app_.model_placements_)
                    {
                        ImGui::PushID(static_cast<int>(model.id));
                        ImGui::Text(
                            "Content %u: %s",
                            model.placement.target.id().slot,
                            model.result && *model.result   ? "inserted"
                            : model.result || model.failure ? "not inserted"
                            : model.cancel_requested        ? "cancelling; waiting for completion"
                                                            : "loading / waiting for the target gate"
                        );
                        if (model.failure)
                            ImGui::TextWrapped("%s: %s", model.failure->domain.c_str(), model.failure->message.c_str());
                        if (model.result && !*model.result)
                            std::visit(
                                [](const auto& error) {
                                    using Error = std::decay_t<decltype(error)>;
                                    if constexpr (std::same_as<Error, scene::SceneEditError>)
                                        ImGui::Text(
                                            "Scene edit rejected (%u); the captured target was not rebased.",
                                            static_cast<unsigned>(error.code)
                                        );
                                    else if constexpr (std::same_as<Error, process::TaskCancelled>)
                                        ImGui::TextUnformatted("Cancelled; no author edit was committed.");
                                    else
                                        ImGui::TextUnformatted(
                                            "Model read or dependency validation failed; source retained."
                                        );
                                },
                                model.result->error().cause
                            );
                        if (model.result || model.failure)
                            button("Acknowledge insertion", AcknowledgeModel{model.id});
                        else
                            button("Cancel insertion", CancelModel{model.id});
                        ImGui::PopID();
                    }
                    ImGui::SeparatorText("Reload results");
                    for (std::size_t index{}; index < app_.reloads_.size(); ++index)
                    {
                        const auto& reload = app_.reloads_[index];
                        ImGui::PushID(static_cast<int>(index));
                        ImGui::Text(
                            "Content %u: %s",
                            reload.source.session.slot,
                            !reload.result   ? "reading / preparing"
                            : *reload.result ? "reloaded"
                                             : "original content retained"
                        );
                        if (reload.result)
                        {
                            if (!*reload.result)
                                ImGui::TextWrapped(
                                    "%s: %s",
                                    reload.result->error().domain.c_str(),
                                    reload.result->error().detail.c_str()
                                );
                            button("Acknowledge reload", AcknowledgeReload{reload.source});
                        }
                        ImGui::PopID();
                    }
                }
            } content_;

        public:
            ResultsPane(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, Impl& app)
                : Pane(
                      dispatcher,
                      std::move(id),
                      lux::ui::PaneTypeId{"lux.editor.content.results"},
                      "Content and Operations"
                  ),
                  content_(*this, app)
            {
                setContent(content_);
            }
        };
        draft.views.push_back(std::make_shared<views::ViewFactoryEntry>(
            contracts::CodeLease::builtin(),
            views::ViewFactoryDescriptor{
                views::ViewTypeId{"lux.editor.content.results"},
                "Content and Operations",
                cxx::typeToken<std::monostate>()
            },
            [this](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView> {
                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<ResultsPane>(input.dispatcher(), input.paneId(), *this)
                };
            }
        ));
        draft.commands.push_back(std::make_shared<commands::CommandEntry>(
            contracts::CodeLease::builtin(),
            commands::CommandDescriptor{
                commands::CommandId{"lux.editor.content.results"},
                "Content and Operations",
                "Window"
            },
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState> {
                return commands::CommandState{phase_ == EApplicationPhase::RUNNING};
            },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt> {
                auto shown = showTool(views::ViewTypeId{"lux.editor.content.results"});
                if (!shown)
                    return cxx::unexpected(commands::CommandFailure{
                        commands::ECommandError::DOMAIN_FAILURE,
                        shown.error().domain,
                        shown.error().reason,
                        shown.error().message
                    });
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }
}
