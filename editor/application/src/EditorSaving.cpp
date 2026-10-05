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

    void EditorApplication::Impl::installSaveCommands(extensions::ContributionDraft& draft)
    {
        const auto review = [this]() -> EditorResult<std::shared_ptr<project::ContentReview>>
        {
            if (!content_review_)
            {
                auto created = editor_context_.services().get<project::ContentReview>(editor_context_.scope());
                if (!created)
                {
                    return applicationFailure("content-review.service", created.error());
                }
                content_review_ = std::move(*created);
            }
            if (!reloading_)
            {
                // Result/close composition still borrows the same activity until M7/M8.
                auto reloads = editor_context_.services().get<ProjectContentReloading>(editor_context_.scope());
                if (!reloads)
                {
                    return applicationFailure("reload.service", reloads.error());
                }
                reloading_ = std::move(*reloads);
            }
            return content_review_;
        };

        draft.commands.push_back(commands::CommandEntry::bind<kReloadCommand>(
            lux::object::CodeLease::builtin(),
            [this](const commands::CommandQuery&) -> commands::CommandResult<commands::CommandState>
            {
                return commands::CommandState{
                    phase_ == EApplicationPhase::RUNNING && (!content_review_ || !content_review_->question())
                };
            },
            [this, review](const commands::CommandInvocation& invocation
            ) -> commands::CommandResult<commands::DispatchReceipt>
            {
                if (last_view_)
                {
                    return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "reload.question"});
                }
                auto owner = review();
                if (!owner)
                {
                    return cxx::unexpected(saveFailure(owner.error()));
                }
                auto accepted = (*owner)->askReload(std::get<commands::SessionTarget>(invocation.target()));
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
                {
                    return commands::CommandState{
                        phase_ == EApplicationPhase::RUNNING && (!content_review_ || !content_review_->question())
                    };
                },
                [this, mode, review](const commands::CommandInvocation& invocation
                ) -> commands::CommandResult<commands::DispatchReceipt>
                {
                    auto owner = review();
                    if (!owner)
                    {
                        return cxx::unexpected(saveFailure(owner.error()));
                    }
                    const auto target = std::get<commands::SessionTarget>(invocation.target());
                    auto info = sessions_->describe(target.id);
                    if (!info)
                    {
                        return cxx::unexpected(saveFailure(applicationFailure("save.session", info.error()).value()));
                    }
                    if (mode == persistence::ESaveMode::SAVE && info->binding)
                    {
                        auto admitted = (*owner)->save(target, mode);
                        if (!admitted)
                        {
                            return cxx::unexpected(saveFailure(admitted.error()));
                        }
                        return commands::DispatchReceipt{
                            commands::AcceptedOperation{commands::OperationKindId{"save"}, admitted->value}
                        };
                    }
                    auto asked = (*owner)->askSave(
                        target,
                        mode == persistence::ESaveMode::SAVE ? persistence::ESaveMode::SAVE_AS : mode
                    );
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
            [this, review](const commands::CommandInvocation&) -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto owner = review();
                if (!owner)
                {
                    return cxx::unexpected(saveFailure(owner.error()));
                }
                auto operation = (*owner)->saveAll();
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
