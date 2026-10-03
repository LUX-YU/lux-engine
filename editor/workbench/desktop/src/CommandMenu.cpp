#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
#include <deque>
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
        struct Source final
        {
            // Handles pin each original defining code owner; nodes borrow only their immutable text.
            std::vector<CommandHandle> handles;
            std::deque<std::string> groups;
        };
        std::shared_ptr<Source> source;
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
            auto candidate = std::make_shared<Source>();
            candidate->handles.reserve(snapshot.entries().size());
            std::vector<lux::ui::MenuItem> menu;
            for (const auto& entry : snapshot.entries())
            {
                const auto index = candidate->handles.size();
                candidate->handles.push_back(*snapshot.at(index));
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
                        candidate->groups.emplace_back(label);
                        children->push_back({{}, candidate->groups.back()});
                        found = children->end() - 1;
                    }
                    children = &found->children;
                    if (slash == std::string_view::npos)
                        break;
                    group.remove_prefix(slash + 1);
                }
                children->push_back(
                    {descriptor.id, descriptor.label, descriptor.shortcut, shortcut(descriptor.shortcut), {}, index}
                );
            }
            root.setMenu(std::move(menu), candidate);
            source = std::move(candidate);
            revision = registry.revision();
        }
    };
    CommandMenu::CommandMenu(
        lux::ui::Root& root,
        CommandRegistry& registry,
        CommandDispatcher& dispatcher,
        Capture capture
    )
        : impl_(std::make_unique<Impl>(nullptr, root, registry, dispatcher, std::move(capture)))
    {}
    CommandMenu::~CommandMenu() = default;
    void CommandMenu::receive(lux::ui::MenuRequest& request)
    {
        auto& self = *impl_;
        if (request.action == lux::ui::EMenuAction::OPEN)
        {
            // The displayed menu is authoritative even if a new catalog has since been published.
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
            return;
        }
        if (request.action == lux::ui::EMenuAction::CLOSE)
        {
            self.open = false;
            self.items.clear();
            return;
        }
        request.command.enabled = false;
        const bool is_stale_source = request.source != self.source.get();
        const bool is_invalid_index = request.index >= self.items.size();
        if (is_stale_source || is_invalid_index)
        {
            self.status = cxx::unexpected(CommandFailure{ECommandError::STALE_TARGET, "menu.source"});
            request.command.result = lux::ui::ECommandDispatchResult::FAILED;
            return;
        }
        const auto* found = &self.items[request.index];
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
