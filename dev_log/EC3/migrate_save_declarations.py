from pathlib import Path
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/activities/project/include/lux/engine/editor/storage/ProjectCommands.hpp'
p.write_text('''#pragma once

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
''')
p=r/'editor/application/src/EditorSaving.cpp';s=p.read_text();a=s.index('\nnamespace\n{');b=s.index('\nnamespace lux::editor::application',a);s=s[:a]+s[b:];s=s.replace('#include <algorithm>','#include <algorithm>\n#include <lux/engine/editor/storage/ProjectCommands.hpp>\n#include <lux/engine/editor/sessions/SessionCommands.hpp>');s=s.replace('command_lux_editor_reload','kReloadCommand').replace('command_lux_editor_save_all','kSaveAllCommand');a=s.index('        for (auto mode :');b=s.index('                [this](const commands::CommandQuery&)',a);s=s[:a]+'''        const auto bindSave = [&]<const commands::CommandDescriptor& Descriptor>(persistence::ESaveMode mode) {
            return commands::CommandEntry::bind<Descriptor>(
                contracts::CodeLease::builtin(),
'''+s[b:];a=s.index('            ));',a);b=s.index('        draft.commands.push_back',a);s=s[:a]+'''            );
        };
        draft.commands.push_back(bindSave.template operator()<sessions::kSaveCommand>(persistence::ESaveMode::SAVE));
        draft.commands.push_back(bindSave.template operator()<kSaveAsCommand>(persistence::ESaveMode::SAVE_AS));
        draft.commands.push_back(bindSave.template operator()<kExportCopyCommand>(persistence::ESaveMode::EXPORT_COPY));
'''+s[b:];p.write_text(s)
# Exact header/source providers and actual dependency closures, not layer exceptions.
import json
p=r/'editor/tests/architecture/rules.json';d=json.loads(p.read_text())
print(list(d.keys()))
p2=Path('.internal/editor-redesign/EC3/session-command-rules-keys.txt');p2.write_text(str(list(d.keys())))
