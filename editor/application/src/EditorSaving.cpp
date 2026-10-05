#include <algorithm>
#include <lux/engine/editor/application/EditorApplicationImpl.hpp>
#include <lux/engine/editor/sessions/SessionCommands.hpp>
#include <lux/engine/editor/storage/ProjectCommands.hpp>

namespace lux::editor::application
{
    namespace
    {
        commands::CommandFailure saveFailure(const EditorFailure& error)
        {
            return {commands::ECommandError::DOMAIN_FAILURE, error.domain, error.reason, error.message};
        }
    } // namespace

    EditorResult<persistence::SaveId> EditorApplication::Impl::save(
        commands::SessionTarget target,
        persistence::ESaveMode mode,
        std::string destination
    )
    {
        if (auto ended = cancelContentPreview(target.id); !ended)
        {
            return cxx::unexpected(ended.error());
        }
        if (!target.based_on)
        {
            return applicationFailure("save.source", sessions::ESessionError::STALE_CONTENT);
        }
        return content_saving_->request(*target.based_on, mode, std::move(destination));
    }
    EditorResult<void> EditorApplication::Impl::cancelContentPreview(sessions::SessionId id)
    {
        auto views = editor_context_.ui().describe(desktop_->root());
        if (!views)
        {
            return applicationFailure("save.views", views.error());
        }
        for (const auto& view : *views)
        {
            if (std::ranges::find(view.content.sessions, id) == view.content.sessions.end())
            {
                continue;
            }
            auto ended = editor_context_.ui().cancelPreview(desktop_->root(), view.handle);
            if (!ended)
            {
                return applicationFailure("save.preview", ended.error());
            }
        }
        return {};
    }
    EditorResult<void> EditorApplication::Impl::askSave(commands::SessionTarget target, persistence::ESaveMode mode)
    {
        if (save_question_ || phase_ != EApplicationPhase::RUNNING)
        {
            return cxx::unexpected(EditorFailure{EEditorError::BUSY, "save.question"});
        }
        auto info = sessions_->describe(target.id);
        if (!info)
        {
            return applicationFailure("save.question.source", info.error());
        }
        if (!target.based_on || *target.based_on != info->current)
        {
            return applicationFailure("save.question.source", sessions::ESessionError::STALE_CONTENT);
        }
        const auto factory = opening_->factory(target.id);
        if (!factory || !(*factory)->descriptor().source)
        {
            return cxx::unexpected(EditorFailure{EEditorError::MISSING_PROVIDER, "save.naming"});
        }
        const auto& suffix = (*factory)->descriptor().source->save_extension;
        auto question = desktop::ReviewView::create(
            messages_.dispatcherRef(),
            lux::ui::PaneId{"save-destination"},
            {next_review_++,
             mode == persistence::ESaveMode::EXPORT_COPY ? "Export Copy" : "Save As",
             "Choose a project-relative source path. The captured target is checked again before saving.",
             {desktop::EReviewChoice::SAVE, desktop::EReviewChoice::CANCEL},
             "Source path",
             std::string("Content/Untitled").append(suffix)}
        );
        if (!question)
        {
            return applicationFailure("save.question.create", question.error());
        }
        auto& root = desktop_->root();
        auto* pane = question->get();
        auto mounted = root.addSubPane(std::move(*question));
        if (!mounted)
        {
            return applicationFailure("save-destination.mount", mounted.error());
        }
        auto shown = root.identify(*pane);
        if (!shown)
        {
            return applicationFailure("save-destination.identity", shown.error());
        }
        save_question_ = SaveQuestion{target, mode, *shown};
        return {};
    }
    EditorResult<void> EditorApplication::Impl::receiveSaveAnswer()
    {
        if (!save_question_)
        {
            return {};
        }
        std::optional<desktop::ReviewAnswer> answer;
        auto read = [&](lux::ui::Pane& pane) { answer = static_cast<desktop::ReviewView&>(pane).response(); };
        auto borrowed = desktop_->root().withPane(save_question_->view, read);
        if (!borrowed)
        {
            return applicationFailure("save.question.read", borrowed.error());
        }
        if (!answer)
        {
            return {};
        }
        auto pane = desktop_->root().findPane(save_question_->view);
        if (!pane)
        {
            return applicationFailure("save-destination.target", pane.error());
        }
        auto prepared = desktop_->root().prepareDetach(**pane);
        if (!prepared)
        {
            return applicationFailure("save.question.close", prepared.error());
        }
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
                {
                    return {}; // Retain the answered draft and exact source until admission is available.
                }
                auto reject = [&](lux::ui::Pane& pane)
                {
                    static_cast<desktop::ReviewView&>(pane).rejectAnswer(
                        admitted.error().domain + ": " + admitted.error().message +
                        "\nThe original content and target were retained. Correct the path, or Cancel and start again."
                    );
                };
                auto displayed = desktop_->root().withPane(save_question_->view, reject);
                if (!displayed)
                {
                    return applicationFailure("save.question.error", displayed.error());
                }
                return {};
            }
        }
        auto closed = desktop_->root().commit(*prepared);
        if (!closed)
        {
            return applicationFailure("save.question.commit", closed.error());
        }
        save_question_.reset();
        return {};
    }

    void EditorApplication::Impl::installSaveCommands(extensions::ContributionDraft& draft)
    {
        draft.commands.push_back(commands::CommandEntry::bind<kReloadCommand>(
            lux::object::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !reload_question_}; },
            [this](const commands::CommandInvocation& invocation) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto accepted = askReload(std::get<commands::SessionTarget>(invocation.target()));
                if (!accepted)
                {
                    return cxx::unexpected(saveFailure(accepted.error()));
                }
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
        const auto bindSave = [&] < const commands::CommandDescriptor & Descriptor > (persistence::ESaveMode mode)
        {
            return commands::CommandEntry::bind<Descriptor>(
                lux::object::CodeLease::builtin(),
                [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
                { return commands::CommandState{phase_ == EApplicationPhase::RUNNING && !save_question_}; },
                [this, mode](const commands::CommandInvocation& invocation
                ) -> commands::CommandResult<commands::DispatchReceipt>
                {
                    const auto target = std::get<commands::SessionTarget>(invocation.target());
                    auto info = sessions_->describe(target.id);
                    if (!info)
                    {
                        return cxx::unexpected(saveFailure(applicationFailure("save.session", info.error()).value()));
                    }
                    if (mode == persistence::ESaveMode::SAVE && info->binding)
                    {
                        auto admitted = save(target, mode);
                        if (!admitted)
                        {
                            return cxx::unexpected(saveFailure(admitted.error()));
                        }
                        return commands::DispatchReceipt{
                            commands::AcceptedOperation{commands::OperationKindId{"save"}, admitted->value}
                        };
                    }
                    auto asked =
                        askSave(target, mode == persistence::ESaveMode::SAVE ? persistence::ESaveMode::SAVE_AS : mode);
                    if (!asked)
                    {
                        return cxx::unexpected(saveFailure(asked.error()));
                    }
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            );
        };
        draft.commands.push_back(bindSave.template operator()<sessions::kSaveCommand>(persistence::ESaveMode::SAVE));
        draft.commands.push_back(bindSave.template operator()<kSaveAsCommand>(persistence::ESaveMode::SAVE_AS));
        draft.commands.push_back(bindSave.template operator()<kExportCopyCommand>(persistence::ESaveMode::EXPORT_COPY));
        draft.commands.push_back(commands::CommandEntry::bind<kSaveAllCommand>(
            lux::object::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            { return commands::CommandState{phase_ == EApplicationPhase::RUNNING}; },
            [this](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto ids = sessions_->snapshotIds();
                if (!ids)
                {
                    return cxx::unexpected(saveFailure(applicationFailure("save-all.contents", ids.error()).value()));
                }
                if (!content_saving_->hasCapacity(ids->size()))
                {
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "save-all.results"});
                }
                for (auto id : *ids)
                {
                    if (auto ended = cancelContentPreview(id); !ended)
                    {
                        return cxx::unexpected(saveFailure(ended.error()));
                    }
                }
                auto operation = content_saving_->saveAll();
                if (!operation)
                {
                    return cxx::unexpected(saveFailure(operation.error()));
                }
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        ));
    }
} // namespace lux::editor::application

namespace lux::editor::application
{
    void EditorApplication::Impl::receiveArtifact(persistence::DerivedArtifact source)
    {
        if (phase_ != EApplicationPhase::RUNNING)
        {
            result_failure_ = EditorFailure{EEditorError::CLOSING, "artifact.admission"};
            return;
        }
        auto requested = content_saving_->requestArtifact(std::move(source));
        if (!requested)
        {
            result_failure_ = EditorFailure{
                requested.error().code == persistence::EPersistenceError::BUSY ? EEditorError::BUSY
                                                                               : EEditorError::SOURCE_FAILURE,
                "artifact.admission",
                0,
                {},
                std::move(requested.error())
            };
        }
    }
} // namespace lux::editor::application
