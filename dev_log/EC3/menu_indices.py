from pathlib import Path
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/activities/commands/src/CommandRegistry.cpp'
s=p.read_text()
a=s.index('            text.reserve(id_size')
b=s.index('\n        }\n    };',a)
s=s[:a]+'''            const auto argument_size = input.argument_type.name().size();
            text.reserve(id_size + label_size + group_size + shortcut_size + argument_size + 5);
            const auto append = [&](std::string_view value) {
                const auto offset = text.size();
                text.append(value).push_back('\\0');
                return offset;
            };
            const auto id = append(input.id.name());
            const auto label = append(input.label);
            const auto group = append(input.group);
            const auto shortcut = append(input.shortcut);
            const auto argument = append(input.argument_type.name());
            // Final storage does not move. Display slices also have a terminator for UI backends.
            const std::string_view bytes{text};
            descriptor = {
                CommandIdView{bytes.substr(id, id_size)},
                bytes.substr(label, label_size),
                bytes.substr(group, group_size),
                bytes.substr(shortcut, shortcut_size),
                input.scope,
                input.input_version,
                {input.argument_type.hash(), bytes.substr(argument, argument_size)}
            };'''+s[b:]
a=s.index('    CommandResult<CommandHandle> CommandRegistrySnapshot::resolve')
s=s[:a]+'''    CommandResult<CommandHandle> CommandRegistrySnapshot::at(std::size_t index) const
    {
        if (!data_ || index >= data_->entries.size())
            return failure(ECommandError::INVALID_ARGUMENT);
        return CommandHandle{data_->entries[index]};
    }
'''+s[a:]
p.write_text(s)
p=r/'modules/function/ui/src/Root.cpp'
s=p.read_text().replace('void menuCommand(Root&, Command&) noexcept;', 'void menuCommand(Root&, Command&, std::size_t) noexcept;')
s=s.replace('        std::vector<MenuItem> menu;', '        std::shared_ptr<const void> menu_source;\n        std::vector<MenuItem> menu;')
s=s.replace('void Root::setMenu(std::vector<MenuItem> menu)', 'void Root::setMenu(std::vector<MenuItem> menu, std::shared_ptr<const void> source)')
s=s.replace('        impl_->menu = std::move(menu);', '        const auto old_source = std::exchange(impl_->menu_source, std::move(source));\n        impl_->menu = std::move(menu);')
s=s.replace('void Root::Impl::menuCommand(Root& root, Command& command) noexcept', 'void Root::Impl::menuCommand(Root& root, Command& command, std::size_t index) noexcept')
s=s.replace('MenuRequest request{EMenuAction::COMMAND, menu_pane, menu_element, command};', 'MenuRequest request{EMenuAction::COMMAND, menu_pane, menu_element, command, menu_source.get(), index};')
s=s.replace('item.label.c_str()', '(item.label.empty() ? "" : item.label.data())').replace('item.shortcut_label.c_str()', '(item.shortcut_label.empty() ? "" : item.shortcut_label.data())')
s=s.replace('Command command{item.command.view()}', 'Command command{item.command}').replace('Command command{item->command.view()}', 'Command command{item->command}')
# Draw and shortcut are separate scopes with differently typed local items.
a=s.index('    void Root::Impl::drawMenuItems')
b=s.index('    bool Root::Impl::shortcut',a)
s=s[:a]+s[a:b].replace('menuCommand(root, command);', 'menuCommand(root, command, item.index);')+s[b:]
a=s.index('    bool Root::Impl::shortcut')
b=s.index('    void Root::Impl::queueChange',a)
s=s[:a]+s[a:b].replace('menuCommand(root, command);', 'menuCommand(root, command, item->index);')+s[b:]
s=s.replace('MenuRequest request{EMenuAction::OPEN, menu_pane, menu_element};','MenuRequest request{EMenuAction::OPEN, menu_pane, menu_element, {}, menu_source.get()};')
s=s.replace('MenuRequest request{EMenuAction::CLOSE, menu_pane, menu_element};','MenuRequest request{EMenuAction::CLOSE, menu_pane, menu_element, {}, menu_source.get()};')
s=s.replace('MenuRequest opened{EMenuAction::OPEN, menu_pane, menu_element};','MenuRequest opened{EMenuAction::OPEN, menu_pane, menu_element, {}, menu_source.get()};')
p.write_text(s)
p=r/'editor/workbench/desktop/src/CommandMenu.cpp'
s=p.read_text().replace('#include <algorithm>', '#include <algorithm>\n#include <deque>')
s=s.replace('        lux::ui::Root& root;', '''        struct Source final
        {
            // Handles pin each original defining code owner; nodes borrow only their immutable text.
            std::vector<CommandHandle> handles;
            std::deque<std::string> groups;
        };
        std::shared_ptr<Source> source;
        lux::ui::Root& root;''')
s=s.replace('            std::vector<lux::ui::MenuItem> menu;', '''            auto candidate = std::make_shared<Source>();
            candidate->handles.reserve(snapshot.entries().size());
            std::vector<lux::ui::MenuItem> menu;''')
s=s.replace('                const auto& descriptor = entry->descriptor();', '''                const auto index = candidate->handles.size();
                candidate->handles.push_back(*snapshot.at(index));
                const auto& descriptor = entry->descriptor();''',1)
s=s.replace('                        children->push_back({{}, std::string(label)});', '''                        candidate->groups.emplace_back(label);
                        children->push_back({{}, candidate->groups.back()});''')
s=s.replace('''                    {lux::ui::CommandId{descriptor.id.name()}, std::string{descriptor.label},
                     std::string{descriptor.shortcut}, shortcut(descriptor.shortcut)}''', '''                    {descriptor.id, descriptor.label, descriptor.shortcut, shortcut(descriptor.shortcut), {}, index}''')
s=s.replace('            root.setMenu(std::move(menu));', '''            root.setMenu(std::move(menu), candidate);
            source = std::move(candidate);''')
# Adding source as a member means aggregate construction must account for it.
s=s.replace('std::make_unique<Impl>(root, registry, dispatcher, std::move(capture))', 'std::make_unique<Impl>(nullptr, root, registry, dispatcher, std::move(capture))')
a=s.index('            self.items.clear();',s.index('if (request.action == lux::ui::EMenuAction::OPEN)'))
b=s.index('            return;',a)
s=s[:a]+'''            // The displayed menu is authoritative even if a new catalog has since been published.
            if (!self.source || (request.source && request.source != self.source.get()))
            {
                self.status = cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "menu.source"});
                return;
            }
            self.items.clear();
            self.items.reserve(self.source->handles.size());
            self.open = true;
            request.source = self.source.get();
            for (const auto& handle : self.source->handles)
            {
                auto input = self.capture(handle.descriptor(), request.pane, request.element);
                self.items.push_back({handle, std::move(input)});
            }
'''+s[b:]
a=s.index('        const auto found = std::ranges::find_if(self.items')
b=s.index('        if (!found->input)',a)
s=s[:a]+'''        request.command.enabled = false;
        const bool is_stale_source = request.source != self.source.get();
        const bool is_invalid_index = request.index >= self.items.size();
        if (is_stale_source || is_invalid_index)
        {
            self.status = cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "menu.source"});
            request.command.result = lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        const auto* found = &self.items[request.index];
'''+s[b:]
p.write_text(s)
