from pathlib import Path
import json
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/activities/commands/src/CommandRegistry.cpp'
s=p.read_text()
a=s.index('        bool validShortcut(')
b=s.index('        CommandResult<void> validate(',a)
s=s[:a]+s[b:]
s=s.replace('''        : code_(std::move(code)), descriptor_(&descriptor), query_(std::move(query)), execute_(std::move(execute))''','''        : code_(std::move(code)), descriptor_(&descriptor), shortcut_(lux::ui::parseShortcut(descriptor.shortcut)),
          query_(std::move(query)), execute_(std::move(execute))''')
a=s.index('    const CommandDescriptor& CommandHandle::descriptor()')
s=s[:a]+'''    const lux::ui::ShortcutResult& CommandEntry::shortcut() const noexcept
    {
        return shortcut_;
    }
'''+s[a:]
s=s.replace('''            const bool is_invalid = is_invalid_identity || is_invalid_description || is_invalid_binding ||
                !validShortcut(descriptor.shortcut);''','''            const bool is_invalid = is_invalid_identity || is_invalid_description || is_invalid_binding;''')
s=s.replace('''        CommandRegistrySnapshot result;
        result.data_ =''','''        for (std::size_t i{}; i < entries.size(); ++i)
        {
            const auto& binding = entries[i]->shortcut();
            if (!binding)
                return cxx::unexpected(CommandFailure{
                    ECommandError::INVALID_ARGUMENT, "command.shortcut", static_cast<std::uint64_t>(binding.error())
                });
            if (binding->key == lux::ui::EKey::NONE)
                continue;
            // Bounded cold validation of the effective default set, never part of event dispatch.
            for (std::size_t previous{}; previous < i; ++previous)
                if (*binding == *entries[previous]->shortcut())
                    return cxx::unexpected(CommandFailure{
                        ECommandError::SHORTCUT_CONFLICT, "command.shortcut", 0,
                        std::string(entries[previous]->descriptor().id.name()) + " / " +
                            std::string(entries[i]->descriptor().id.name())
                    });
        }
        CommandRegistrySnapshot result;
        result.data_ =''')
p.write_text(s)
p=r/'editor/workbench/desktop/src/CommandMenu.cpp'
s=p.read_text()
a=s.index('    namespace\n')
b=s.index('    struct CommandMenu::Impl',a)
s=s[:a]+s[b:]
s=s.replace('shortcut(descriptor.shortcut)', '*entry->shortcut()')
p.write_text(s)
p=r/'editor/tests/architecture/rules.json'
x=json.loads(p.read_text())
if 'ui_input' not in x['editor_commands']['direct']: x['editor_commands']['direct'].append('ui_input')
if 'lux/engine/ui/Shortcut.hpp' not in x['editor_commands']['headers']: x['editor_commands']['headers'].append('lux/engine/ui/Shortcut.hpp')
# Only closures that actually include commands or ui obtain this pure header provider.
for section in x.values():
 if isinstance(section,dict) and isinstance(section.get('closure'),dict):
  if any(k in section['closure'] for k in ('editor_commands','ui')):
   section['closure']['ui_input']={'path':'modules/function/ui'}
x['editor_commands']['closure']['ui_input']={'path':'modules/function/ui'}
layer=x['editor_layering']
print('layer sections:',list(layer))
for k,v in layer.items():
 if isinstance(v,dict) and 'ui' in v:
  print('ui metadata in',k,v['ui'])
  v['ui_input']={'path':'modules/function/ui'} if k=='external_targets' else v['ui']
 # Exact file providers are edited below after inspecting this known inventory schema.
p.write_text(json.dumps(x,indent=2)+'\n')
