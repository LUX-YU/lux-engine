from pathlib import Path
s=Path(r'E:/SyncForder/CodeRepos/lux-engine-p11')
p=s/'editor/tests/integration/scene_views/views.cpp';t=p.read_text();t=t.replace('        std::unique_ptr<desktop::DesktopShell> desktop;','        editor::commands::CommandRegistry commands;\n        editor::commands::CommandDispatcher dispatcher{commands};\n        std::unique_ptr<desktop::DesktopShell> desktop;');old='    assert(b->undo());\n    f.wait([&] {';new='''    // The actual DesktopShell menu submits a pinned view command; execution still enters the
    // same SceneView/domain undo path that this dual-viewport regression has always observed.
    using namespace editor::commands;
    auto undo=std::make_shared<CommandEntry>(contracts::CodeLease::builtin(),
        CommandDescriptor{CommandId{"p11.undo"},"Undo","Edit","Ctrl+Z",ECommandScope::VIEW},
        [&](const CommandQuery& input)->CommandResult<CommandState> {
            if(std::get<views::ViewId>(input.target)!=id_b || !f.desktop->views().describe(id_b))
                return cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET,"scene.view"});
            return CommandState{take(f.session->historyView()).can_undo};
        },
        [&](const CommandInvocation&)->CommandResult<DispatchReceipt> {
            assert(b->undo());return DispatchReceipt{ImmediateCompletion{}};
        });
    assert(f.commands.publish(take(CommandRegistrySnapshot::create({undo}))));
    assert(f.desktop->installCommands(f.commands,f.dispatcher,
        [&](const CommandDescriptor&,const ui::Pane*,const ui::Element*)->CommandResult<CommandInvocation> {
            return CommandInvocation{id_b};
        }));
    f.frame();
    ui::MenuRequest open_menu;assert(object::sendEvent(f.desktop->root(),open_menu));
    ui::MenuRequest invoke{ui::EMenuAction::COMMAND,{},{},{ui::CommandIdView{"p11.undo"},ui::ECommandPhase::EXECUTE}};
    assert(object::sendEvent(f.desktop->root(),invoke));assert(f.dispatcher.pending()==1);
    f.wait([&] {''';assert old in t;t=t.replace(old,new,1);needle='    const auto receipt = take(f.resources->viewReceipt(a->viewport()));';t=t.replace(needle,'''    const auto command_results=f.desktop->commands()->takeCompletions();
    assert(command_results.size()==1 && command_results.front().result);
    assert(f.commands.publish({}));
'''+needle,1);p.write_text(t)
