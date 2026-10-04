#include <lux/engine/editor/sessions/SessionCommands.hpp>

namespace lux::editor::sessions
{
    namespace
    {
        commands::CommandFailure commandFailure(const sessions::SessionFactoryFailure& error)
        {
            using enum commands::ECommandError;
            auto code = DOMAIN_FAILURE;
            if (error.code == sessions::ESessionFactoryError::BUSY)
                code = BUSY;
            if (error.code == sessions::ESessionFactoryError::STALE_SESSION)
                code = STALE_TARGET;
            if (error.code == sessions::ESessionFactoryError::STALE_CONTENT)
                code = STALE_CONTENT;
            return {code, error.domain, error.domain_code, error.detail};
        }
        commands::CommandResult<sessions::SessionInfo> targetInfo(
            sessions::SessionStore& store,
            const commands::VCommandTarget& target
        )
        {
            const auto* selected = std::get_if<commands::SessionTarget>(&target);
            if (!selected)
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "session.target"}
                );
            auto info = store.describe(selected->id);
            if (!info)
                return cxx::unexpected(commandFailure(sessions::factoryFailure(info.error())));
            if (info->admission != sessions::EEditAdmission::AVAILABLE)
                return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "session.gate"});
            if (selected->based_on && *selected->based_on != info->current)
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::STALE_CONTENT, "session.content"}
                );
            return std::move(*info);
        }
    } // namespace
    std::shared_ptr<commands::CommandEntry> makeSourceSaveCommand(
        SessionStore& store,
        persistence::SaveService& saves,
        HistoryActionLookup lookup
    )
    {
        using namespace commands;
        auto roles = std::make_shared<HistoryActionLookup>(std::move(lookup));
        return CommandEntry::bind<kSaveCommand>(
            lux::object::CodeLease::builtin(),
            [&store, roles](const CommandQuery& input) -> CommandResult<CommandState>
            {
                auto info = targetInfo(store, input.target);
                if (!info)
                    return cxx::unexpected(info.error());
                const bool available = bool(*roles) && (*roles)(info->id);
                return CommandState{available, false, available ? "" : "No save role"};
            },
            [&saves](const CommandInvocation& input) -> CommandResult<DispatchReceipt>
            {
                // The menu fixes the session; requestSave freezes the content current at actual admission.
                auto admitted = saves.requestSave({std::get<SessionTarget>(input.target()).id});
                if (!admitted)
                    return cxx::unexpected(CommandFailure{
                        admitted.error().code == persistence::EPersistenceError::BUSY ? ECommandError::BUSY
                                                                                      : ECommandError::DOMAIN_FAILURE,
                        "persistence",
                        static_cast<std::uint64_t>(admitted.error().code),
                        admitted.error().detail
                    });
                return DispatchReceipt{AcceptedOperation{OperationKindId{"save"}, admitted->value}};
            }
        );
    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeHistoryCommands(
        SessionStore& store,
        HistoryActionLookup lookup
    )
    {
        using namespace commands;
        auto roles = std::make_shared<HistoryActionLookup>(std::move(lookup));
        std::vector<std::shared_ptr<CommandEntry>> entries;
        for (bool forward : {false, true})
        {
            const auto bind = [&]<const CommandDescriptor & Descriptor>()
            {
                return CommandEntry::bind<Descriptor>(
                    lux::object::CodeLease::builtin(),
                    [&store, roles, forward](const CommandQuery& input) -> CommandResult<CommandState>
                    {
                        auto info = targetInfo(store, input.target);
                        if (!info)
                            return cxx::unexpected(info.error());
                        auto* role = (*roles) ? (*roles)(info->id) : nullptr;
                        if (!role)
                            return CommandState{false, false, "No history role"};
                        auto history = role->queryHistory();
                        if (!history)
                            return cxx::unexpected(commandFailure(history.error()));
                        return CommandState{forward ? history->can_redo : history->can_undo};
                    },
                    [roles, forward](const CommandInvocation& input) -> CommandResult<DispatchReceipt>
                    {
                        auto* role = (*roles) ? (*roles)(std::get<SessionTarget>(input.target()).id) : nullptr;
                        if (!role)
                            return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "session.role"});
                        auto result = forward ? role->redo() : role->undo();
                        if (!result)
                            return cxx::unexpected(commandFailure(result.error()));
                        return DispatchReceipt{ImmediateCompletion{}};
                    }
                );
            };
            entries.push_back(
                forward ? bind.template operator()<kRedoCommand>() : bind.template operator()<kUndoCommand>()
            );
        }
        return entries;
    }
} // namespace lux::editor::sessions
