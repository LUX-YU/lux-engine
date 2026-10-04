from pathlib import Path
r=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=r/'editor/tests/integration/session_factories/installation.cpp'
s=p.read_text()
a=s.index('    asset::AssetId identity(')
s=s[:a]+'''    const ui::MenuItem& menuItem(const ui::Root& root, ui::CommandIdView id)
    {
        const auto find = [&](auto&& self, std::span<const ui::MenuItem> items) -> const ui::MenuItem* {
            for (const auto& item : items)
            {
                if (item.command == id)
                    return &item;
                if (const auto* nested = self(self, item.children))
                    return nested;
            }
            return nullptr;
        };
        const auto* item = find(find, root.menu());
        assert(item);
        return *item;
    }
    void menuDescriptorLifetime()
    {
        using namespace commands;
        unsigned released{}, calls{};
        CommandRegistry registry;
        CommandDispatcher dispatcher{registry, 4};
        auto messages = take(object::ObjectMessageQueue::create(64));
        CommandRoot first{messages.dispatcherRef()}, second{messages.dispatcherRef()};
        const auto capture = [](const CommandDescriptor&, const ui::Pane*, const ui::Element*)
            -> CommandResult<CommandInvocation> { return CommandInvocation{}; };
        desktop::CommandMenu menu_a{first, registry, dispatcher, capture};
        desktop::CommandMenu menu_b{second, registry, dispatcher, capture};
        first.menu = &menu_a;
        second.menu = &menu_b;
        const char* label{};
        const char* shortcut{};
        const char* canonical{};
        {
            // Slice has no terminator at its own end; the dynamic Entry performs the sole text freeze.
            std::string dynamic_label{"Dynamic label unused suffix"};
            auto code = contracts::CodeLease::plugin(std::shared_ptr<const void>(new int{1}, [&](const void* p) {
                ++released;
                delete static_cast<const int*>(p);
            }));
            auto entry = CommandEntry::create(
                code, {CommandIdView{"menu.dynamic"}, std::string_view{dynamic_label}.substr(0, 13), "Tools/Sub", "Alt+M"},
                [](const CommandQuery&) -> CommandResult<CommandState> { return CommandState{true}; },
                [&](const CommandInvocation&) -> CommandResult<DispatchReceipt> {
                    assert(released == 0);
                    ++calls;
                    return DispatchReceipt{ImmediateCompletion{}};
                }
            );
            label = entry->descriptor().label.data();
            shortcut = entry->descriptor().shortcut.data();
            canonical = entry->descriptor().id.name().data();
            assert(label[13] == '\\0');
            assert(registry.publish(take(CommandRegistrySnapshot::create({entry}))));
        }
        assert(menu_a.update() && menu_b.update() && released == 0);
        for (const auto* root : {&first, &second})
        {
            const auto& item = menuItem(*root, ui::CommandIdView{"menu.dynamic"});
            assert(item.label.data() == label && item.shortcut_label.data() == shortcut);
            assert(item.command.name().data() == canonical && item.index == 0);
        }
        ui::MenuRequest opened_a, opened_b;
        assert(object::sendEvent(first, opened_a) && object::sendEvent(second, opened_b));
        assert(opened_a.source && opened_b.source && opened_a.source != opened_b.source);
        ui::MenuRequest wrong_source{ui::EMenuAction::COMMAND, {}, {}, {}, opened_b.source, 0};
        assert(object::sendEvent(first, wrong_source));
        assert(wrong_source.command.result == ui::ECommandDispatchResult::FAILED && calls == 0);
        auto invalid_index = wrong_source;
        invalid_index.source = opened_a.source;
        invalid_index.index = 1;
        assert(object::sendEvent(first, invalid_index));
        assert(invalid_index.command.result == ui::ECommandDispatchResult::FAILED && calls == 0);
        assert(registry.publish({}));
        ui::MenuRequest close_b{ui::EMenuAction::CLOSE, {}, {}, {}, opened_b.source};
        assert(object::sendEvent(second, close_b) && menu_b.update() && released == 0);
        // Dispatch uses the source-local index, not this deliberately unrelated event label.
        ui::MenuRequest invoke{ui::EMenuAction::COMMAND, {}, {},
            {ui::CommandIdView{"not.a.lookup"}, ui::ECommandPhase::EXECUTE}, opened_a.source, 0};
        assert(object::sendEvent(first, invoke));
        assert(invoke.command.result == ui::ECommandDispatchResult::EXECUTED && dispatcher.pending() == 1);
        ui::MenuRequest close_a{ui::EMenuAction::CLOSE, {}, {}, {}, opened_a.source};
        assert(object::sendEvent(first, close_a));
        assert(menu_a.update());
        auto completed = menu_a.takeCompletions();
        assert(completed.size() == 1 && completed[0].result && calls == 1 && released == 1);
        first.menu = nullptr;
        second.menu = nullptr;
        std::cout << "PASS EC3 two menus share original text; source/index rejection and queued code lifetime\\n";
    }
'''+s[a:]
s=s.replace('    assert(argc == 2 || argc == 3);', '    assert(argc == 2 || argc == 3);\n    menuDescriptorLifetime();')
s=s.replace('opened.source, 1};', 'opened.source, menuItem(root, ui::CommandIdView{"lux.editor.undo"}).index};')
s=s.replace('opened.source, 0};', 'opened.source, menuItem(root, ui::CommandIdView{"lux.editor.save"}).index};')
# Check the actual fixed provider's label pointer, not merely value equality.
s=s.replace('        assert(menu.update());\n        const auto scene_before', '''        assert(menu.update());
        const auto original_undo = take(snapshot.find(CommandIdView{"lux.editor.undo"}));
        assert(menuItem(root, ui::CommandIdView{"lux.editor.undo"}).label.data() ==
               original_undo.descriptor().label.data());
        const auto scene_before''',1)
p.write_text(s)
p=r/'editor/activities/commands/test/commands.cpp'
s=p.read_text().replace('        assert(snapshot && snapshot->entries()[0].get() == fixed.get());', '''        assert(snapshot && snapshot->entries()[0].get() == fixed.get());
        assert(!snapshot->at(2) && !CommandRegistrySnapshot{}.at(0));
        assert(&snapshot->at(0)->descriptor() == &literal);
        assert(owned.label.data()[owned.label.size()] == '\\0');
        assert(owned.shortcut.data()[owned.shortcut.size()] == '\\0');''')
p.write_text(s)
