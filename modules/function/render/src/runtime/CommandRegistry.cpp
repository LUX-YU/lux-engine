#include <algorithm>
#include <lux/engine/render/runtime/CommandRegistry.hpp>
#include <tuple>

namespace lux::render::runtime_detail
{
    RenderResult<CommandId> CommandRegistry::findCommand(const CommandDescriptor& description) const noexcept
    {
        const auto found = commands_by_name_.find(description.name.name());
        if (found == commands_by_name_.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        const auto& stored = *commands_.find(found->second)->description;
        if (auto compatible = compatibleCommands(stored, description); !compatible)
        {
            return cxx::unexpected(compatible.error());
        }
        return found->second;
    }

    CommandList CommandRegistry::listCommands() const noexcept
    {
        CommandList result;
        result.commands.reserve(commands_.size());
        for (const auto& [name, id] : commands_by_name_)
        {
            const auto& entry = *commands_.find(id);
            const auto& description = *entry.description;
            result.commands.push_back(
                {std::string(name), id, description.request, description.response, entry.selectedBinding() != nullptr}
            );
        }
        return result;
    }

    void CommandRegistry::listCommandBindings(
        const ListCommandBindings::Request& command,
        ListCommandBindings::Response& response
    ) noexcept
    {
        const auto* entry = commands_.find(command);
        if (!entry)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const auto* selected = entry->selectedBinding();
        CommandBindings result{{}, selected ? selected->handler : HandlerId{}};
        result.handlers.reserve(entry->bindings.size());
        for (const auto& binding : entry->bindings)
        {
            // Built-in calls borrow Impl, not an installed Handler object.
            if (binding.handler.isNull())
            {
                continue;
            }
            const auto& handler = **handlers_.find(binding.handler);
            result.handlers.push_back({binding.handler, handler.name, handler.instance_name});
        }
        response = std::move(result);
    }

    void CommandRegistry::selectHandler(
        const SelectCommandBinding::Request& input,
        SelectCommandBinding::Response& response
    ) noexcept
    {
        auto* entry = commands_.find(input.command);
        if (!entry)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (entry->bindings.front().handler.isNull())
        {
            response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        if (input.handler.isNull())
        {
            entry->selected_binding = CommandRecord::kUnbound;
            response = {};
            return;
        }
        // There is no per-Handler erase/hot unload. SlotMap assigns monotonically increasing
        // indices until clear(), so installed bindings are ordered without a second sparse index.
        const auto found = std::ranges::lower_bound(
            entry->bindings,
            input.handler.index,
            {},
            [](const HandlerBinding& binding) { return binding.handler.index; }
        );
        const bool is_missing_handler = found == entry->bindings.end() || found->handler != input.handler;
        if (is_missing_handler)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        entry->selected_binding = static_cast<std::size_t>(found - entry->bindings.begin());
        response = {};
    }

    CommandId CommandRegistry::registerCommand(CommandBinding binding) noexcept
    {
        // Built-in installation and prepared Handler publication have already established uniqueness.
        const auto id =
            commands_.emplace(CommandRecord{binding.descriptor_, binding.make_error_, {{{}, binding.call_}}, 0});
        commands_by_name_.emplace(binding.descriptor_->name.name(), id);
        return id;
    }

    RenderResult<HandlerId> CommandRegistry::findHandler(CppType type, std::string_view instance) const noexcept
    {
        const auto found = handlers_by_name_.find({type.token.name(), instance});
        if (found == handlers_by_name_.end())
        {
            return HandlerId{};
        }
        const auto& entry = **handlers_.find(found->second);
        if (entry.type != type)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return found->second;
    }

    RenderResult<std::vector<CommandId>> CommandRegistry::prepareHandler(
        const HandlerRecord& handler,
        std::span<const CommandBinding> calls
    ) const noexcept
    {
        const bool is_invalid_installation = handler.name.empty() || calls.empty();
        if (is_invalid_installation)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        std::vector<CommandId> commands(calls.size());
        std::vector<std::string_view> names;
        names.reserve(calls.size());
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            const auto& description = *calls[i].descriptor();
            if (auto valid = validateCommand(description); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            names.push_back(description.name.name());
            const auto found = commands_by_name_.find(description.name.name());
            if (found == commands_by_name_.end())
            {
                continue;
            }
            const auto& stored = *commands_.find(found->second);
            if (auto valid = compatibleCommands(*stored.description, description); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            if (stored.bindings.front().handler.isNull())
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            commands[i] = found->second;
        }
        std::ranges::sort(names);
        if (std::ranges::adjacent_find(names) != names.end())
        {
            return cxx::unexpected(RenderError{kFeatureDuplicate});
        }
        return commands;
    }

    HandlerId CommandRegistry::registerHandler(
        std::unique_ptr<HandlerRecord> handler,
        std::span<const CommandBinding> calls,
        std::vector<CommandId> commands
    ) noexcept
    {
        // Recoverable validation is complete; ordinary host allocation exhaustion remains fatal.
        auto* record = handler.get();
        const auto id = handlers_.emplace(std::move(handler));
        handlers_by_name_.emplace(HandlerName{record->type.token.name(), record->instance_name}, id);
        for (std::size_t i = 0; i < calls.size(); ++i)
        {
            // An earlier dependency in this validated batch can have published the same declaration.
            if (commands[i].isNull())
            {
                const auto found = commands_by_name_.find(calls[i].descriptor()->name.name());
                if (found != commands_by_name_.end())
                {
                    commands[i] = found->second;
                }
            }
            if (commands[i].isNull())
            {
                const auto& binding = calls[i];
                commands[i] =
                    commands_.emplace(CommandRecord{binding.descriptor_, binding.make_error_, {{id, binding.call_}}, 0}
                    );
                commands_by_name_.emplace(binding.descriptor_->name.name(), commands[i]);
            }
            else
            {
                commands_.find(commands[i])->bindings.push_back({id, calls[i].call()});
                // Preserve the selected index, including deliberate unbinding.
            }
        }
        return id;
    }
} // namespace lux::render::runtime_detail
