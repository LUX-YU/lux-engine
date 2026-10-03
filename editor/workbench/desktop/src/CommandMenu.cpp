#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
namespace lux::editor::desktop
{
    using namespace commands;
    namespace
    {
        lux::ui::Shortcut shortcut(std::string_view text)
        {
            lux::ui::Shortcut result;
            if (text.starts_with("Ctrl+"))
            {
                result.control = true;
                text.remove_prefix(5);
            }
            if (text.starts_with("Shift+"))
            {
                result.shift = true;
                text.remove_prefix(6);
            }
            if (text.starts_with("Alt+"))
            {
                result.alt = true;
                text.remove_prefix(4);
            }
            if (text.size() == 1 && text.front() >= 'A' && text.front() <= 'Z')
                result.key = static_cast<lux::ui::EKey>(static_cast<unsigned>(lux::ui::EKey::A) + text.front() - 'A');
            else if (text == "Delete")
                result.key = lux::ui::EKey::DELETE_KEY;
            else if (text == "Enter")
                result.key = lux::ui::EKey::ENTER;
            else if (text == "Escape")
                result.key = lux::ui::EKey::ESCAPE;
            return result;
        }
    }
    struct CommandMenu::Impl final
    {
        struct Item final
        {
            CommandHandle handle;
            CommandResult<CommandInvocation> input;
        };
        lux::ui::Root& root;
        CommandRegistry& registry;
        CommandDispatcher& dispatcher;
        Capture capture;
        std::uint64_t revision{UINT64_MAX};
        bool open{};
        std::vector<Item> items;
        std::vector<CommandCompletion> completed;
        CommandResult<void> status;
        void refresh()
        {
            if (open || revision == registry.revision())
                return;
            auto snapshot = registry.snapshot();
            std::vector<lux::ui::MenuItem> menu;
            for (const auto& entry : snapshot.entries())
            {
                const auto& descriptor = entry->descriptor();
                if (descriptor.group.empty())
                    continue;
                auto* children = &menu;
                std::string_view group{descriptor.group};
                while (!group.empty())
                {
                    const auto slash = group.find('/');
                    auto label = group.substr(0, slash);
                    auto found = std::ranges::find(*children, label, &lux::ui::MenuItem::label);
                    if (found == children->end())
                    {
                        children->push_back({{}, std::string(label)});
                        found = children->end() - 1;
                    }
                    children = &found->children;
                    if (slash == std::string_view::npos)
                        break;
                    group.remove_prefix(slash + 1);
                }
                children->push_back(
                    {lux::ui::CommandId{descriptor.id.name()}, std::string{descriptor.label},
                     std::string{descriptor.shortcut}, shortcut(descriptor.shortcut)}
                );
            }
            root.setMenu(std::move(menu));
            revision = registry.revision();
        }
    };
    CommandMenu::CommandMenu(
        lux::ui::Root& root,
        CommandRegistry& registry,
        CommandDispatcher& dispatcher,
        Capture capture
    )
        : impl_(std::make_unique<Impl>(root, registry, dispatcher, std::move(capture)))
    {}
    CommandMenu::~CommandMenu() = default;
    void CommandMenu::receive(lux::ui::MenuRequest& request)
    {
        auto& self = *impl_;
        if (request.action == lux::ui::EMenuAction::OPEN)
        {
            self.items.clear();
            auto snapshot = self.registry.snapshot();
            self.items.reserve(snapshot.entries().size());
            for (const auto& entry : snapshot.entries())
            {
                auto handle = snapshot.find(entry->descriptor().id);
                if (!handle)
                    std::terminate(); // A validated snapshot must resolve every own entry.
                auto input = self.capture(entry->descriptor(), request.pane, request.element);
                self.items.push_back({std::move(*handle), std::move(input)});
            }
            self.open = true;
            return;
        }
        if (request.action == lux::ui::EMenuAction::CLOSE)
        {
            self.open = false;
            self.items.clear();
            return;
        }
        const auto found = std::ranges::find_if(self.items, [&](const auto& item) {
            return item.handle.descriptor().id == request.command.id;
        });
        request.command.enabled = false;
        if (found == self.items.end())
            return;
        if (!found->input)
        {
            self.status = cxx::unexpected(found->input.error());
            request.command.result = lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        if (request.command.phase == lux::ui::ECommandPhase::QUERY)
        {
            auto state = self.registry.query(found->handle, found->input->query());
            if (!state)
            {
                self.status = cxx::unexpected(state.error());
                request.command.result = lux::ui::ECommandDispatchResult::FAILED;
                return;
            }
            request.command.enabled = state->enabled;
            request.command.checked = state->checked;
            request.command.result = lux::ui::ECommandDispatchResult::DISABLED;
            return;
        }
        auto input = *found->input; // Owned payload and identity stay tied to the menu-open capture.
        auto admitted = self.dispatcher.enqueue(found->handle, input);
        if (!admitted)
        {
            self.status = cxx::unexpected(admitted.error());
            request.command.result = lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        request.command.enabled = true;
        request.command.result = lux::ui::ECommandDispatchResult::EXECUTED; // Queue admission only.
        self.open = false; // Shortcuts also use OPEN/COMMAND, without a popup CLOSE event.
        self.status = {};
    }
    CommandResult<void> CommandMenu::update()
    {
        impl_->refresh();
        if (!impl_->completed.empty())
            return {};
        auto completed = impl_->dispatcher.drain();
        if (!completed)
            return cxx::unexpected(completed.error());
        impl_->completed = std::move(*completed);
        return {};
    }
    std::vector<CommandCompletion> CommandMenu::takeCompletions()
    {
        return std::exchange(impl_->completed, {});
    }
    const CommandResult<void>& CommandMenu::status() const noexcept
    {
        return impl_->status;
    }
}
