#include <algorithm>
#include <bit>
#include <lux/engine/render/commands/CommandRegistry.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <tuple>

namespace lux::render::runtime_detail
{
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
                if (is_duplicate_call) return cxx::unexpected(RenderError{kCommandContract});
            }
            const auto found = commands_by_name_.find(binding.descriptor()->name.name());
            if (found == commands_by_name_.end()) continue;
            const auto& command = *commands_.find(found->second);
            if (command.description != binding.descriptor())
            {
                if (auto valid = compatibleCommands(*command.description, *binding.descriptor()); !valid) return valid;
            }
            const bool is_route_mismatch = !binding.effect_ || !command.effect || command.storage;
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
    CommandRegistry::validateResourceBindings(std::span<const CommandBinding> bindings) const noexcept
    {
        for (const auto& binding : bindings)
        {
            const bool invalid_route =
                !binding.valid() || !binding.isResourceCommand() || binding.handlerType().isValid();
            if (invalid_route)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
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

    CommandBindingLocation CommandRegistry::publishResourceBinding(const CommandBinding& binding) noexcept
    {
        const auto found = commands_by_name_.find(binding.descriptor()->name.name());
        if (found == commands_by_name_.end())
        {
            const auto id = commands_.emplace(CommandRecord{
                binding.descriptor_,
                binding.make_error_,
                {{{}, binding.call_, binding.method_type_, binding.storage_type_}},
                nullptr,
                binding.storage_
            });
            commands_by_name_.emplace(binding.descriptor()->name.name(), id);
            return {id, 0};
        }
        auto& entry = *commands_.find(found->second);
        const auto index = static_cast<std::uint32_t>(entry.bindings.size());
        entry.bindings.push_back({{}, binding.call_, binding.method_type_, binding.storage_type_});
        return {found->second, index};
    }

    RenderResult<CommandId> CommandRegistry::findCommand(const CommandDescriptor& description) const noexcept
    {
        const auto found = commands_by_name_.find(description.name.name());
        if (found == commands_by_name_.end())
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        const auto& stored = *commands_.find(found->second)->description;
        if (&stored != &description)
        {
            if (auto compatible = compatibleCommands(stored, description); !compatible)
                return cxx::unexpected(compatible.error());
        }
        return found->second;
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
            {{{}, binding.call_, binding.method_type_}},
            binding.effect_,
            binding.storage_
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
            if (!binding.effect_ || !stored.effect || stored.storage)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
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
        const auto found = commands_by_name_.find(binding.descriptor()->name.name());
        if (found == commands_by_name_.end())
        {
            const auto id = commands_.emplace(CommandRecord{
                binding.descriptor_,
                binding.make_error_,
                {{handler, call, binding.method_type_, binding.storage_type_}},
                binding.effect_,
                binding.storage_
            });
            commands_by_name_.emplace(binding.descriptor()->name.name(), id);
            return {id, 0};
        }
        auto& entry = *commands_.find(found->second);
        const auto index = static_cast<std::uint32_t>(entry.bindings.size());
        entry.bindings.push_back({handler, call, binding.method_type_, binding.storage_type_});
        return {found->second, index};
    }

    RenderResult<CommandBindingLocation>
    CommandRegistry::findBinding(CommandId command, cxx::basic_type_info method, HandlerId handler) const noexcept
    {
        const auto* entry = commands_.find(command);
        if (!entry) return cxx::unexpected(RenderError{kRuntimeIdentity});
        const auto found = std::ranges::find_if(entry->bindings, [&](const auto& previous) noexcept
        {
            return previous.handler == handler && previous.method_type == method;
        });
        if (found == entry->bindings.end()) return cxx::unexpected(RenderError{kRuntimeIdentity});
        return CommandBindingLocation{command, static_cast<std::uint32_t>(found - entry->bindings.begin())};
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
