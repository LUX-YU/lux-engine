from pathlib import Path
import json
s=Path('E:/SyncForder/CodeRepos/lux-engine-p11')
def edit(p,a,b):
 p=s/p;t=p.read_text();assert a in t,(p,a);p.write_text(t.replace(a,b),newline='\n')
p='editor/workbench/desktop/include/lux/engine/editor/desktop/DesktopShell.hpp'
edit(p,'#include <optional>','#include <optional>\n#include <lux/engine/editor/desktop/CommandMenu.hpp>')
edit(p,'        [[nodiscard]] lux::ui::Root& root() noexcept;','''        [[nodiscard]] commands::CommandResult<void> installCommands(
            commands::CommandRegistry&, commands::CommandDispatcher&, CommandMenu::Capture);
        [[nodiscard]] CommandMenu* commands() noexcept;
        [[nodiscard]] lux::ui::Root& root() noexcept;''')
p='editor/workbench/desktop/src/DesktopShell.cpp'
edit(p,'            Presentation* presentation{};','''            Presentation* presentation{};
            CommandMenu* menu{};
            void event(object::EventView& event) noexcept override
            {
                if (auto* request = event.getIf<lux::ui::MenuRequest>(); request && menu)
                {
                    menu->receive(*request);
                    event.accept();
                }
            }''')
edit(p,'        ViewHost host_;','        ViewHost host_;\n        std::unique_ptr<CommandMenu> menu_;')
edit(p,'            root_.closeInput();','            root_.menu = nullptr;\n            root_.closeInput();')
edit(p,'    lux::ui::Root& DesktopShell::root() noexcept','''    commands::CommandResult<void> DesktopShell::installCommands(
        commands::CommandRegistry& registry, commands::CommandDispatcher& dispatcher, CommandMenu::Capture capture)
    {
        if (impl_->menu_ || !capture)
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "desktop.commands"});
        auto menu = std::make_unique<CommandMenu>(impl_->root_, registry, dispatcher, std::move(capture));
        auto installed = menu->update();
        if (!installed) return installed;
        impl_->menu_ = std::move(menu);
        impl_->root_.menu = impl_->menu_.get();
        return {};
    }
    CommandMenu* DesktopShell::commands() noexcept { return impl_->menu_.get(); }
    lux::ui::Root& DesktopShell::root() noexcept''')
edit(p,'        auto drained = impl_->host_.drain();','''        if (impl_->menu_)
        {
            const auto commands = impl_->menu_->update();
            if (!commands) return cxx::unexpected(DesktopFailure{"desktop.commands", commands.error()});
        }
        auto drained = impl_->host_.drain();''')
p='editor/workbench/desktop/src/CommandMenu.cpp'
edit(p,'if (!handle) continue; // Entries in a validated immutable snapshot always resolve.','if (!handle) std::terminate(); // A validated snapshot must resolve every own entry.')
p='editor/workbench/desktop/CMakeLists.txt'
edit(p,'SOURCE_FILES src/DesktopShell.cpp','SOURCE_FILES src/CommandMenu.cpp src/DesktopShell.cpp')
edit(p,'target_link_libraries(desktop_shell PUBLIC lux::engine::scene::scene_render','target_link_libraries(desktop_shell PUBLIC lux::engine::editor::editor_commands lux::engine::scene::scene_render')
edit(p,'component_add_transitive_commands(desktop_shell','component_add_transitive_commands(desktop_shell\n    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"')
edit(p,'install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp','install(FILES\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/CommandMenu.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/desktop/DesktopError.hpp')
f=s/'editor/tests/architecture/rules.json';r=json.loads(f.read_text());pr=r['editor_layering']['providers']
for p in ['editor/workbench/desktop/src/CommandMenu.cpp','editor/workbench/desktop/include/lux/engine/editor/desktop/CommandMenu.hpp']:pr[p]=['desktop_shell']
f.write_text(json.dumps(r,indent=2)+'\n')
