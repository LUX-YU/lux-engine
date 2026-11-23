#include <algorithm>
#include <bit>
#include <lux/engine/render/runtime/commands/CommandRegistry.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <tuple>

namespace lux::render::runtime_detail
{
    void CommandRegistry::publishNodeCommands(const EffectNodeDefinition& node, std::vector<CommandId> commands) noexcept
    {
        if (!commands.empty()) node_commands_.emplace(&node, std::move(commands));
    }

    void CommandRegistry::publishImplementationBindings(
        const EffectImplementation& implementation, std::vector<CommandBindingLocation> bindings
    ) noexcept
    {
        if (!bindings.empty()) implementation_bindings_.emplace(&implementation, std::move(bindings));
    }

    std::span<const CommandId> CommandRegistry::nodeCommands(const EffectNodeDefinition& node) const noexcept
    {
        const auto found = node_commands_.find(&node);
        if (found == node_commands_.end()) return {};
        return found->second;
    }

    std::span<const CommandBindingLocation>
    CommandRegistry::effectBindings(const EffectImplementation& implementation) const noexcept
    {
        const auto found = implementation_bindings_.find(&implementation);
        if (found == implementation_bindings_.end()) return {};
        return found->second;
    }

    RenderResult<void>
    CommandRegistry::validateStaticBindings(std::span<const CommandBinding> bindings) const noexcept
    {
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];
            const bool is_invalid_route = !binding.valid() || binding.handlerType().isValid() ||
                binding.storageType().isValid() || binding.call().object || binding.storage_;
            if (is_invalid_route) return cxx::unexpected(RenderError{kCommandContract});
            if (auto valid = validateCommand(*binding.descriptor()); !valid) return valid;
            for (std::size_t previous = 0; previous < i; ++previous)
            {
                const auto& other = bindings[previous];
                if (other.descriptor()->name != binding.descriptor()->name) continue;
                if (auto valid = compatibleCommands(*other.descriptor(), *binding.descriptor()); !valid) return valid;
                const bool is_duplicate_call = !binding.effect_ || other.methodType() == binding.methodType();
                const bool mismatched_address = binding.isNodeEffectCommand() != other.isNodeEffectCommand();
                if (mismatched_address) return cxx::unexpected(RenderError{kCommandContract});
                if (is_duplicate_call) return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto found = commands_by_name_.find(binding.descriptor()->name.name());
            if (found == commands_by_name_.end()) continue;
            const auto& command = *commands_.find(found->second);
            if (command.description != binding.descriptor())
            {
                if (auto valid = compatibleCommands(*command.description, *binding.descriptor()); !valid) return valid;
            }
            const bool mismatched_address = binding.isNodeEffectCommand() != (command.effect_node != nullptr);
            const bool is_route_mismatch = !binding.effect_ || !command.effect || command.storage || mismatched_address;
            if (is_route_mismatch) return cxx::unexpected(RenderError{kCommandContract});
            const bool is_duplicate_call = std::ranges::any_of(command.bindings, [&](const auto& previous) noexcept
            {
                return previous.handler.isNull() && previous.method_type == binding.methodType();
            });
            if (is_duplicate_call) return cxx::unexpected(RenderError{kCommandContract});
        }
        return {};
    }

    void CommandRegistry::publishStaticBindings(std::span<const CommandBinding> bindings) noexcept
    {
        for (const auto& binding : bindings) publishBinding({}, binding);
    }

    RenderResult<void>
    CommandRegistry::validateResourceBindings(cxx::basic_type_info storage, std::span<const CommandBinding> bindings) const noexcept
    {
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto& binding = bindings[i];
            const bool invalid_route =
                !binding.valid() || !binding.isResourceCommand() || binding.handlerType().isValid();
            if (invalid_route || binding.storageType() != storage)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (std::ranges::any_of(bindings.first(i), [&](const CommandBinding& previous) noexcept {
                return previous.descriptor()->name == binding.descriptor()->name;
            })) return cxx::unexpected(RenderError{kCommandContract});
            if (auto valid = validateCommand(*binding.descriptor()); !valid)
            {
                return valid;
            }
            const auto found = commands_by_name_.find(binding.descriptor()->name.name());
            if (found == commands_by_name_.end())
            {
                continue;
            }
            const auto& previous = *commands_.find(found->second);
            if (!previous.storage)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            if (previous.description != binding.descriptor())
            {
                if (auto valid = compatibleCommands(*previous.description, *binding.descriptor()); !valid) return valid;
            }
        }
        return {};
    }

    void CommandRegistry::publishResourceBinding(ResourceTypeId resource, const CommandBinding& binding) noexcept
    {
        const auto own_type = [this](cxx::basic_type_info type) noexcept -> cxx::basic_type_info
        {
            return type.isValid() ? cxx::basic_type_info{type.hash(), resource_names_.emplace_back(type.name())} : type;
        };
        const HandlerBinding method{{}, binding.call_, own_type(binding.method_type_), own_type(binding.storage_type_), {}, resource};
        const auto found = commands_by_name_.find(binding.descriptor()->name.name());
        if (found == commands_by_name_.end())
        {
            auto& descriptor = resource_descriptors_.emplace_back(*binding.descriptor());
            descriptor.name = CommandName{resource_names_.emplace_back(descriptor.name.name())};
            descriptor.request = own_type(descriptor.request);
            descriptor.response = own_type(descriptor.response);
            descriptor.data = own_type(descriptor.data);
            const auto id = commands_.emplace(CommandRecord{
                &descriptor,
                binding.make_error_,
                {method},
                nullptr,
                binding.storage_
            });
            commands_by_name_.emplace(descriptor.name.name(), id);
            return;
        }
        auto& entry = *commands_.find(found->second);
        entry.bindings.push_back(method);
    }

    void CommandRegistry::queryCommandIds(const QueryCommandIds::Request& names, QueryCommandIds::Response& response)
        const noexcept
    {
        // RenderThread only, like listCommands. No per-name cross-thread descriptor lookup is needed.
        response.ids.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            const auto found = commands_by_name_.find(names[i].name());
            if (found == commands_by_name_.end())
            {
                response.error = RenderError{kRuntimeUnsupported, {i}};
                response.ids.clear();
                return;
            }
            response.ids.push_back(found->second);
        }
        response.error = {};
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
                {std::string(name),
                 id,
                 description.request,
                 description.response,
                 !entry.bindings.empty(),
                 description.requires_idle_resource,
                 description.failure_safe,
                 description.data,
                 description.capability_command}
            );
        }
        return result;
    }

    CommandId CommandRegistry::registerCommand(CommandBinding binding) noexcept
    {
        // Built-in installation and prepared Handler publication have already established uniqueness.
        const auto id = commands_.emplace(CommandRecord{
            binding.descriptor_,
            binding.make_error_,
            {{{}, binding.call_, binding.method_type_, binding.storage_type_, binding.effect_state_type_}},
            binding.effect_,
            binding.storage_,
            binding.effect_node_
        });
        commands_by_name_.emplace(binding.descriptor_->name.name(), id);
        return id;
    }

    RenderResult<HandlerId> CommandRegistry::findHandler(cxx::basic_type_info type, std::string_view instance) const noexcept
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
        if (!existing->isNull())
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
            if (!binding.valid() || binding.isResourceCommand())
            {
                return cxx::unexpected(RenderError{kCommandContract});
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
            if (stored.description != &description)
            {
                if (auto valid = compatibleCommands(*stored.description, description); !valid) return valid;
            }
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }

        return has_methods ? RenderResult<void>{} : cxx::unexpected(RenderError{kCommandContract});
    }

    HandlerId CommandRegistry::publishHandler(HandlerRecord handler) noexcept
    {
        const auto id = handlers_.emplace(std::make_unique<HandlerRecord>(std::move(handler)));
        const auto& record = **handlers_.find(id);
        handlers_by_name_.emplace(HandlerName{record.type.name(), record.instance_name}, id);
        return id;
    }

    CommandBindingLocation CommandRegistry::publishBinding(HandlerId handler, const CommandBinding& binding) noexcept
    {
        auto call = binding.call();
        if (!handler.isNull()) call.object = (*handlers_.find(handler))->object.get();
        const HandlerBinding method{
            handler, call, binding.method_type_, binding.storage_type_, binding.effect_state_type_
        };
        const auto found = commands_by_name_.find(binding.descriptor()->name.name());
        if (found == commands_by_name_.end())
        {
            const auto id = commands_.emplace(CommandRecord{
                binding.descriptor_,
                binding.make_error_,
                {method},
                binding.effect_,
                binding.storage_,
                binding.effect_node_
            });
            commands_by_name_.emplace(binding.descriptor()->name.name(), id);
            return {id, 0};
        }
        auto& entry = *commands_.find(found->second);
        const auto index = static_cast<std::uint32_t>(entry.bindings.size());
        entry.bindings.push_back(method);
        return {found->second, index};
    }

    const HandlerBinding* CommandRegistry::binding(CommandBindingLocation location) const noexcept
    {
        const auto* entry = commands_.find(location.command);
        return entry ? entry->binding(location.binding) : nullptr;
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

    void CommandRegistry::activateHandlers(ResourceRegistry& resources) noexcept
    {
        // Activate the sole stateful command object on the Server owner before any command borrows it.
        for (const auto& [name, id] : handlers_by_name_)
        {
            activateHandler(id, resources);
        }
    }
} // namespace lux::render::runtime_detail
