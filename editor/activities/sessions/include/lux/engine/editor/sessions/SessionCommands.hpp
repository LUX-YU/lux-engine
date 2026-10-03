#pragma once

#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/sessions/SessionInstallation.hpp>

namespace lux::editor::sessions
{
    inline constexpr commands::CommandDescriptor kSaveCommand{
        commands::CommandIdView{"lux.editor.save"}, "Save", "File", "Ctrl+S", commands::ECommandScope::SESSION
    };
    inline constexpr commands::CommandDescriptor kUndoCommand{
        commands::CommandIdView{"lux.editor.undo"}, "Undo", "Edit", "Ctrl+Z", commands::ECommandScope::SESSION
    };
    inline constexpr commands::CommandDescriptor kRedoCommand{
        commands::CommandIdView{"lux.editor.redo"}, "Redo", "Edit", "Ctrl+Y", commands::ECommandScope::SESSION
    };
    using HistoryActionLookup = cxx::move_only_function<InstalledSession*(SessionId)>;

    // Product composition selects a source-save or project-save receiver for kSaveCommand, never both.
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
    makeSourceSaveCommand(SessionStore&, persistence::SaveService&, HistoryActionLookup);
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>>
    makeHistoryCommands(SessionStore&, HistoryActionLookup);
}
