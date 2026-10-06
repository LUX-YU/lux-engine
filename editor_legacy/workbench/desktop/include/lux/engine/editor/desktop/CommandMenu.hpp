#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/ui/Menu.hpp>
namespace lux::ui
{
    class Root;
}
namespace lux::editor::desktop
{
    [[nodiscard]] commands::CommandResult<void> validateShortcutOverrides(std::span<const commands::ShortcutOverride>);
    // Owns only the open menu's fixed inputs and UI delivery results. Actual operations remain in services.
    class CommandMenu final
    {
    public:
        using Capture = cxx::move_only_function<commands::CommandResult<
            commands::
                CommandInvocation>(const commands::CommandDescriptor&, const lux::ui::Pane*, const lux::ui::Element*)>;
        CommandMenu(lux::ui::Root&, commands::CommandRegistry&, commands::CommandDispatcher&, Capture);
        ~CommandMenu();
        CommandMenu(const CommandMenu&) = delete;
        CommandMenu& operator=(const CommandMenu&) = delete;
        CommandMenu(CommandMenu&&) = delete;
        CommandMenu& operator=(CommandMenu&&) = delete;
        void receive(lux::ui::MenuRequest&);
        // Cold, atomic menu preparation. Empty binding disables a shortcut. Unknown command rows
        // remain owned and become effective when a compatible registration appears. An open popup
        // retains its original source; BUSY means the caller must retain and retry its desired value.
        [[nodiscard]] commands::CommandResult<void> setShortcuts(std::span<const commands::ShortcutOverride>);
        // Outer workbench safe point. Uncollected results exert bounded backpressure, never disappear.
        [[nodiscard]] commands::CommandResult<void> update();
        [[nodiscard]] std::vector<commands::CommandCompletion> takeCompletions();
        [[nodiscard]] const commands::CommandResult<void>& status() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::desktop
