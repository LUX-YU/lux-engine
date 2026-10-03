#pragma once

#include <lux/engine/editor/commands/Command.hpp>

namespace lux::editor
{
    inline constexpr commands::CommandDescriptor kReloadCommand{
        commands::CommandIdView{"lux.editor.reload"}, "Reload", "File", "", commands::ECommandScope::SESSION
    };
    inline constexpr commands::CommandDescriptor kSaveAsCommand{
        commands::CommandIdView{"lux.editor.save-as"}, "Save As", "File", "", commands::ECommandScope::SESSION
    };
    inline constexpr commands::CommandDescriptor kExportCopyCommand{
        commands::CommandIdView{"lux.editor.export-copy"}, "Export Copy", "File", "", commands::ECommandScope::SESSION
    };
    inline constexpr commands::CommandDescriptor kSaveAllCommand{
        commands::CommandIdView{"lux.editor.save-all"}, "Save All", "File"
    };
}
