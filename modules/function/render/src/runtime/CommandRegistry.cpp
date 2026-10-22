#include <algorithm>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
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
        SelectCommandBinding::Response& response,
        ResourceRegistry& resources
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
        activateHandler(input.handler, resources);
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

    RenderResult<HandlerId> CommandRegistry::findHandler(cxx::TypeToken type, std::string_view instance) const noexcept
    {
        const auto found = handlers_by_name_.find({type.name(), instance});
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

    HandlerId CommandRegistry::handlerId(cxx::TypeToken type, std::string_view instance) const noexcept
    {
        const auto found = handlers_by_name_.find({type.name(), instance});
        return found == handlers_by_name_.end() ? HandlerId{} : found->second;
    }

    RenderResult<void> CommandRegistry::validateHandler(
        const HandlerRecord& handler,
        std::span<const CommandBinding> bindings
    ) const noexcept
    {
        const bool is_invalid_factory = handler.name.empty() || !handler.create || !handler.object.get_deleter();
        if (is_invalid_factory)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto existing = findHandler(handler.type, handler.instance_name);
        if (!existing)
        {
            return cxx::unexpected(existing.error());
        }
        if (!existing->isNull() && (*handlers_.find(*existing))->name != handler.name)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        bool has_methods = false;
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];
            if (binding.handlerType() != handler.type)
            {
                continue;
            }
            has_methods = true;
            const auto& description = *binding.descriptor();
            if (auto valid = validateCommand(description); !valid)
            {
                return valid;
            }
            // Qualify collisions within the incoming closure as well as the published directory.
            for (std::size_t j = 0; j < i; ++j)
            {
                if (bindings[j].descriptor()->name != description.name)
                {
                    continue;
                }
                if (auto valid = compatibleCommands(*bindings[j].descriptor(), description); !valid)
                {
                    return valid;
                }
                // Identical CommandMethod types were deduplicated by the template. Two remaining
                // methods for one Handler/Command are ambiguous, even if the linker folds their code.
                if (bindings[j].handlerType() == handler.type)
                {
                    return cxx::unexpected(RenderError{kCommandContract});
                }
            }
            const auto found = commands_by_name_.find(description.name.name());
            if (found == commands_by_name_.end())
            {
                continue;
            }
            const auto& stored = *commands_.find(found->second);
            if (auto valid = compatibleCommands(*stored.description, description); !valid)
            {
                return valid;
            }
            if (stored.bindings.front().handler.isNull())
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            // Existing Handler/type + Command contracts are canonical. Callback addresses can
            // differ between compatible DLLs; repeated registration preserves the published call.
        }
        return has_methods ? RenderResult<void>{} : cxx::unexpected(RenderError{kCommandContract});
    }

    HandlerId CommandRegistry::publishHandler(HandlerRecord handler) noexcept
    {
        const auto existing = handlerId(handler.type, handler.instance_name);
        if (!existing.isNull())
        {
            return existing;
        }
        const auto id = handlers_.emplace(std::make_unique<HandlerRecord>(std::move(handler)));
        const auto& record = **handlers_.find(id);
        handlers_by_name_.emplace(HandlerName{record.type.name(), record.instance_name}, id);
        return id;
    }

    void CommandRegistry::publishBinding(HandlerId handler, const CommandBinding& binding) noexcept
    {
        auto call = binding.call();
        call.object = (*handlers_.find(handler))->object.get();
        const auto found = commands_by_name_.find(binding.descriptor()->name.name());
        if (found == commands_by_name_.end())
        {
            const auto id =
                commands_.emplace(CommandRecord{binding.descriptor_, binding.make_error_, {{handler, call}}, 0});
            commands_by_name_.emplace(binding.descriptor()->name.name(), id);
            return;
        }
        auto& entry = *commands_.find(found->second);
        const auto position = std::ranges::lower_bound(
            entry.bindings,
            handler.index,
            {},
            [](const HandlerBinding& value) { return value.handler.index; }
        );
        if (position != entry.bindings.end() && position->handler == handler)
        {
            return;
        }
        const auto index = static_cast<std::size_t>(position - entry.bindings.begin());
        entry.bindings.insert(position, {handler, call});
        // An older Handler can gain a new method. Keep the selected identity when inserting before it.
        if (entry.selected_binding != CommandRecord::kUnbound && index <= entry.selected_binding)
        {
            ++entry.selected_binding;
        }
    }

    void CommandRegistry::activateHandler(HandlerId id, ResourceRegistry& resources) noexcept
    {
        auto& record = **handlers_.find(id);
        if (record.object)
        {
            return;
        }
        record.object.reset(record.create(resources));
        for (auto& command : commands_)
        {
            for (auto& binding : command.bindings)
            {
                if (binding.handler == id)
                {
                    binding.call.object = record.object.get();
                }
            }
        }
    }

    void CommandRegistry::activateSelectedHandlers(ResourceRegistry& resources) noexcept
    {
        // Setup/control path only. Ordinary dispatch never resolves or constructs a Handler.
        for (auto& command : commands_)
        {
            const auto* selected = command.selectedBinding();
            if (selected && !selected->handler.isNull())
            {
                activateHandler(selected->handler, resources);
            }
        }
    }
} // namespace lux::render::runtime_detail
