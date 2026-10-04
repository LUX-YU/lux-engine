from pathlib import Path
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/application/extensions/src/BuiltinContributions.cpp'
s=p.read_text()
a=s.index('        commands::CommandFailure commandFailure('); b=s.index('        template <class Error> views::ViewFactoryFailure',a)
helpers=s[a:b]
a=s.index('    std::vector<std::shared_ptr<commands::CommandEntry>> builtinSessionCommands(');b=s.index('    std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> builtinSessionFactories(',a)
body=s[a:b]
s=s[:a]+s[b:]
a=s.index('        commands::CommandFailure commandFailure(');b=s.index('        template <class Error> views::ViewFactoryFailure',a);s=s[:a]+s[b:]
a=s.index('    constexpr lux::editor::commands::CommandDescriptor command_lux_editor_save{');b=s.index('\n    };',a)+len('\n    };');s=s[:a]+s[b:];p.write_text(s)
# Preserve the real history/save algorithms, split assembly before any entries are registered.
a=body.index('        entries.push_back(CommandEntry::bind<command_lux_editor_save>('); b=body.index('        for (bool forward',a)
save=body[a:b].replace('        entries.push_back(CommandEntry::bind<command_lux_editor_save>(', '        return CommandEntry::bind<kSaveCommand>(').replace('        ));','        );')
history=body[b:body.rindex('        return entries;')]
a=history.index('            entries.push_back(CommandEntry::create(');b=history.index('                [&store, roles, forward]',a)
history=history[:a]+'''            const auto bind = [&]<const CommandDescriptor& Descriptor>() {
                return CommandEntry::bind<Descriptor>(
                    contracts::CodeLease::builtin(),
'''+history[b:]
history=history.replace('            ));','''                );
            };
            entries.push_back(forward ? bind.template operator()<kRedoCommand>()
                                      : bind.template operator()<kUndoCommand>());''')
header='''#pragma once

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
'''
(r/'editor/activities/sessions/include/lux/engine/editor/sessions/SessionCommands.hpp').write_text(header)
cpp='#include <lux/engine/editor/sessions/SessionCommands.hpp>\n\nnamespace lux::editor::sessions\n{\n    namespace\n    {\n'+helpers+'''    }
    std::shared_ptr<commands::CommandEntry> makeSourceSaveCommand(
        SessionStore& store, persistence::SaveService& saves, HistoryActionLookup lookup
    )
    {
        using namespace commands;
        auto roles = std::make_shared<HistoryActionLookup>(std::move(lookup));
'''+save+'''    }
    std::vector<std::shared_ptr<commands::CommandEntry>> makeHistoryCommands(
        SessionStore& store, HistoryActionLookup lookup
    )
    {
        using namespace commands;
        auto roles = std::make_shared<HistoryActionLookup>(std::move(lookup));
        std::vector<std::shared_ptr<CommandEntry>> entries;
'''+history+'''        return entries;
    }
}
'''
(r/'editor/activities/sessions/src/SessionCommands.cpp').write_text(cpp)
p=r/'editor/application/extensions/include/lux/engine/editor/extensions/BuiltinContributions.hpp';s=p.read_text();s=s.replace('    using HistoryActionLookup = cxx::move_only_function<sessions::InstalledSession*(sessions::SessionId)>;\n','');a=s.index('    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> builtinSessionCommands(');b=s.index('    [[nodiscard]] std::vector<std::shared_ptr<sessions::SessionFactoryEntry>>',a);s=s[:a]+s[b:];p.write_text(s)
p=r/'editor/application/src/EditorCommands.cpp';s=p.read_text().replace('#include <algorithm>','#include <algorithm>\n#include <lux/engine/editor/sessions/SessionCommands.hpp>');s=s.replace('extensions::builtinSessionCommands({sessions_, saves_}, [this](auto id) { return opening_.find(id); })','sessions::makeHistoryCommands(sessions_, [this](auto id) { return opening_.find(id); })');a=s.index('        std::erase_if(draft.commands,');b=s.index('        installSaveCommands',a);s=s[:a]+s[b:];p.write_text(s)
p=r/'editor/tests/integration/session_factories/installation.cpp';s=p.read_text();s='#include <lux/engine/editor/sessions/SessionCommands.hpp>\n'+s;s=s.replace('auto snapshot = take(CommandRegistrySnapshot::create(extensions::builtinSessionCommands({store, saves}, find)));','auto entries = sessions::makeHistoryCommands(store, find);\n        entries.insert(entries.begin(), sessions::makeSourceSaveCommand(store, saves, find));\n        auto snapshot = take(CommandRegistrySnapshot::create(std::move(entries)));');p.write_text(s)
p=r/'editor/activities/sessions/CMakeLists.txt';s=p.read_text().replace('src/SessionOperations.cpp)','src/SessionOperations.cpp src/SessionCommands.cpp)');s=s.replace('target_link_libraries(session_factories PUBLIC','target_link_libraries(session_factories PUBLIC lux::engine::editor::editor_commands');s=s.replace('component_add_transitive_commands(session_factories','component_add_transitive_commands(session_factories\n    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"');p.write_text(s)
