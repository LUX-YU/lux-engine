#include <lux/engine/editor/desktop/CommandMenu.hpp>
#include <lux/engine/ui/Root.hpp>
#include <algorithm>
#include <deque>
namespace lux::editor::desktop
{
    using namespace commands;
    namespace
    {
        struct ParsedOverride final
        {
            ShortcutOverride value;
            std::uint64_t hash;
            lux::ui::Shortcut shortcut;
        };
        using Overrides = std::vector<ParsedOverride>;
    } // namespace
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
            std::shared_ptr<const Overrides> overrides;
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
        std::shared_ptr<const Overrides> overrides;
        CommandResult<void> rebuild(std::shared_ptr<const Overrides> requested)
        {
            auto snapshot = registry.snapshot();
            auto candidate = std::make_shared<Source>();
            candidate->handles.reserve(snapshot.entries().size());
            candidate->overrides = std::move(requested);
            std::vector<lux::ui::Shortcut> effective;
            effective.reserve(snapshot.entries().size());
            std::vector<lux::ui::MenuItem> menu;
            for (const auto& entry : snapshot.entries())
            {
                const auto index = candidate->handles.size();
                candidate->handles.push_back(*snapshot.at(index));
                const auto& descriptor = entry->descriptor();
                std::string_view label = descriptor.shortcut;
                auto shortcut = *entry->shortcut();
                if (candidate->overrides)
                {
                    const auto& values = *candidate->overrides;
                    const auto found =
                        std::ranges::lower_bound(values, descriptor.id.hash(), {}, &ParsedOverride::hash);
                    if (found != values.end() && found->hash == descriptor.id.hash())
                    {
                        const bool is_identity_mismatch = found->value.command != descriptor.id.name();
                        if (is_identity_mismatch)
                            return cxx::unexpected(CommandFailure{ECommandError::HASH_COLLISION, "shortcut.identity"});
                        const bool is_incompatible = found->value.scope != descriptor.scope ||
                                                     found->value.input_version != descriptor.input_version;
                        if (is_incompatible)
                            return cxx::unexpected(CommandFailure{
                                ECommandError::INCOMPATIBLE_REGISTRATION,
                                "shortcut.version",
                                0,
                                found->value.command
                            });
                        label = found->value.binding;
                        shortcut = found->shortcut;
                    }
                }
                if (shortcut.key != lux::ui::EKey::NONE)
                {
                    if (std::ranges::find(effective, shortcut) != effective.end())
                        return cxx::unexpected(CommandFailure{
                            ECommandError::SHORTCUT_CONFLICT,
                            "shortcut.effective",
                            0,
                            std::string{descriptor.id.name()}
                        });
                }
                effective.push_back(shortcut);
                if (descriptor.group.empty())
                    continue;
                auto* children = &menu;
                std::string_view group{descriptor.group};
                while (!group.empty())
                {
                    const auto slash = group.find('/');
                    auto group_label = group.substr(0, slash);
                    auto found = std::ranges::find(*children, group_label, &lux::ui::MenuItem::label);
                    if (found == children->end())
                    {
                        candidate->groups.emplace_back(group_label);
                        children->push_back({{}, candidate->groups.back()});
                        found = children->end() - 1;
                    }
                    children = &found->children;
                    if (slash == std::string_view::npos)
                        break;
                    group.remove_prefix(slash + 1);
                }
                children->push_back({descriptor.id, descriptor.label, label, shortcut, {}, index});
            }
            root.setMenu(std::move(menu), candidate);
            source = std::move(candidate);
            revision = registry.revision();
            return {};
        }
        void refresh()
        {
            if (open || revision == registry.revision())
                return;
            auto prepared = rebuild(overrides);
            // Rejected new registration/settings keep the last published menu. Do not reparse or
            // retry an unchanged failure every frame; explicit settings edits can prepare again.
            revision = registry.revision();
            status = std::move(prepared);
        }
    };
    CommandMenu::CommandMenu(
        lux::ui::Root& root,
        CommandRegistry& registry,
        CommandDispatcher& dispatcher,
        Capture capture
    )
        : impl_(std::make_unique<Impl>(nullptr, root, registry, dispatcher, std::move(capture)))
    {
    }
    CommandMenu::~CommandMenu() = default;
    namespace
    {
        CommandResult<std::shared_ptr<const Overrides>> prepareShortcuts(std::span<const ShortcutOverride> input)
        {
            if (input.size() > 256)
                return cxx::unexpected(CommandFailure{ECommandError::CAPACITY, "shortcut.capacity"});
            auto candidate = std::make_shared<Overrides>();
            candidate->reserve(input.size());
            for (const auto& value : input)
            {
                const bool is_invalid_scope = value.scope != ECommandScope::APPLICATION &&
                                              value.scope != ECommandScope::SESSION &&
                                              value.scope != ECommandScope::VIEW;
                const bool is_invalid_identity = value.command.empty() || value.command.size() > 512 ||
                                                 value.command.find('\0') != std::string::npos ||
                                                 value.input_version == 0;
                if (is_invalid_scope || is_invalid_identity)
                    return cxx::unexpected(CommandFailure{ECommandError::INVALID_ARGUMENT, "shortcut.identity"});
                auto parsed = lux::ui::parseShortcut(value.binding);
                if (!parsed)
                    return cxx::unexpected(CommandFailure{
                        ECommandError::INVALID_ARGUMENT,
                        "shortcut.syntax",
                        static_cast<std::uint64_t>(parsed.error()),
                        value.command
                    });
                candidate->push_back({value, CommandIdView{value.command}.hash(), *parsed});
            }
            std::ranges::sort(*candidate, {}, &ParsedOverride::hash);
            for (std::size_t i = 1; i < candidate->size(); ++i)
            {
                const auto& a = (*candidate)[i - 1];
                const auto& b = (*candidate)[i];
                if (a.hash != b.hash)
                    continue;
                const auto code =
                    a.value.command == b.value.command ? ECommandError::DUPLICATE : ECommandError::HASH_COLLISION;
                return cxx::unexpected(CommandFailure{code, "shortcut.identity"});
            }
            return candidate;
        }
    } // namespace
    CommandResult<void> validateShortcutOverrides(std::span<const ShortcutOverride> input)
    {
        auto prepared = prepareShortcuts(input);
        if (!prepared)
            return cxx::unexpected(prepared.error());
        return {};
    }
    CommandResult<void> CommandMenu::setShortcuts(std::span<const ShortcutOverride> input)
    {
        if (impl_->open)
            return cxx::unexpected(CommandFailure{ECommandError::BUSY, "shortcut.open-menu"});
        auto parsed = prepareShortcuts(input);
        if (!parsed)
            return cxx::unexpected(parsed.error());
        auto candidate = std::move(*parsed);
        auto prepared = impl_->rebuild(candidate);
        if (!prepared)
            return prepared;
        impl_->overrides = std::move(candidate);
        impl_->status = {};
        return {};
    }
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
} // namespace lux::editor::desktop
