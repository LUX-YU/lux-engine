#include <algorithm>
#include <lux/engine/render/extensions/detail/CommandDispatchContext.hpp>
#include <exception>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>

namespace lux::render
{
    void runtime_detail::finishResourceCommand(ResourceRegistry& registry, ResourceInstanceId id) noexcept
    {
        if (!id.isNull()) registry.finishCommand(id);
    }

    RenderResult<ResourceRegistry::ResolvedCommand>
    ResourceRegistry::beginCommand(ResourceInstanceId id, CommandId command, bool exclusive) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto& definition = *types_.find(entry->type);
        const auto location = std::ranges::find(definition.commands, command, &CommandBindingLocation::command);
        if (location == definition.commands.end())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const bool conflicting_command = entry->exclusive_command || (exclusive && entry->command_uses != 0);
        if (conflicting_command)
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        if (entry->command_uses == UINT32_MAX) std::terminate();
        ++entry->command_uses;
        entry->exclusive_command = exclusive;
        return ResolvedCommand{*location, definition.factory.storage->implementation_type};
    }

    bool ResourceRegistry::hasBorrows(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && (entry->command_uses != 0 || entry->execution_uses != 0);
    }

    bool ResourceRegistry::hasDependents(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->dependent_storages != 0;
    }

    RenderResult<ResourceInstanceId> ResourceRegistry::requireDuringConstruction(
        ResourceCreateContext& context, ResourceInstanceId consumer, ResourceTypeId type
    ) noexcept
    {
        auto* entry = entries_.find(consumer);
        const auto* definition = factory(type);
        if (!entry || entry->instance || !definition) return cxx::unexpected(RenderError{kStorageContract});
        auto* scope = entry->storage_set->ownerFor(definition->descriptor->domain);
        if (!scope) return cxx::unexpected(RenderError{kStorageMismatch});
        auto dependency = scope->getOrCreate(context, type);
        if (!dependency) return cxx::unexpected(dependency.error());

        auto& dependencies = entries_.find(consumer)->dependencies;
        if (std::ranges::find(dependencies, *dependency) == dependencies.end())
        {
            auto& count = entries_.find(*dependency)->dependent_storages;
            if (count == UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
            dependencies.push_back(*dependency);
            ++count;
        }
        return *dependency;
    }

    RenderResult<void> ResourceRegistry::retainExecution(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->instance) return cxx::unexpected(RenderError{kStorageIdentity});
        if (entry->exclusive_command) return cxx::unexpected(RenderError{kDeviceBusy});
        if (entry->execution_uses == UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        ++entry->execution_uses;
        return {};
    }

    void ResourceRegistry::releaseExecution(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->execution_uses) std::terminate();
        --entry->execution_uses;
    }

    bool ResourceRegistry::hasExclusiveCommand(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->exclusive_command;
    }

    const detail::DeviceSession* ResourceRegistry::deviceOf(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? &entry->storage_set->device_ : nullptr;
    }

    bool ResourceRegistry::canReference(ResourceInstanceId owner, ResourceInstanceId dependency) const noexcept
    {
        const auto* target = entries_.find(owner);
        const auto* source = entries_.find(dependency);
        const bool has_instances = target && target->instance && source && source->instance;
        if (!has_instances)
        {
            return false;
        }
        if (&target->storage_set->device_ != &source->storage_set->device_)
        {
            return false;
        }
        for (const auto* domain = target->storage_set; domain; domain = domain->parent_)
        {
            if (domain == source->storage_set)
            {
                return true;
            }
        }
        return false;
    }

    void ResourceRegistry::finishCommand(ResourceInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->command_uses)
        {
            std::terminate();
        }
        if (--entry->command_uses == 0)
        {
            entry->exclusive_command = false;
        }
    }

    ResourceRegistry::~ResourceRegistry() noexcept
    {
        if (!entries_.empty())
        {
            std::terminate(); // actual Sets must close before their borrowed index
        }
    }

    ResourceTypeId ResourceRegistry::declarationOf(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? entry->type : ResourceTypeId{};
    }

    const CommandBindingLocation*
    ResourceRegistry::commandBinding(ResourceTypeId type, CommandId command) const noexcept
    {
        const auto* definition = types_.find(type);
        if (!definition)
        {
            return nullptr;
        }
        const auto found = std::ranges::find(definition->commands, command, &CommandBindingLocation::command);
        return found == definition->commands.end() ? nullptr : &*found;
    }

    RenderResult<void> ResourceRegistry::bindCommand(ResourceTypeId type, CommandBindingLocation location) noexcept
    {
        auto* definition = types_.find(type);
        if (!definition || location.command.isNull())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        if (const auto* previous = commandBinding(type, location.command))
        {
            if (*previous != location)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return {};
        }
        definition->commands.push_back(location);
        return {};
    }

    ResourceTypeId ResourceRegistry::typeId(ResourceNameView type) const noexcept
    {
        const auto found = types_by_name_.find(type.name());
        return found == types_by_name_.end() ? ResourceTypeId{} : found->second;
    }

    const ResourceRegistration* ResourceRegistry::factory(ResourceTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->factory : nullptr;
    }

    const ResourceRegistration* ResourceRegistry::factory(ResourceNameView type) const noexcept
    {
        const auto index = typeId(type);
        return index.isNull() ? nullptr : &types_.find(index)->factory;
    }

    const ConfigValue* ResourceRegistry::configuration(ResourceTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->configuration : nullptr;
    }

    RenderResult<const ResourceRegistration*>
    ResourceRegistry::qualify(ResourceTypeId type, EResourceDomain domain, const ResourceCreateContext& context)
        const noexcept
    {
        const auto* definition = types_.find(type);
        if (!definition)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        const bool is_wrong_registry = &context.resources != this;
        const bool is_wrong_domain = definition->factory.descriptor->domain != domain;
        if (is_wrong_registry || is_wrong_domain)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (!definition->factory.descriptor->supports(context.device.resourceCapabilities()))
        {
            return cxx::unexpected(RenderError{kStorageCapability});
        }
        if (definition->factory.storage->config_type.isValid() && !definition->configuration)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        return &definition->factory;
    }

    RenderResult<void> ResourceRegistry::validateFactory(const ResourceRegistration& factory) const noexcept
    {
        if (!factory.descriptor || !factory.storage)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateResourceRegistration(factory); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        if (!typeId(factory.descriptor->type).isNull())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    RenderResult<void>
    ResourceRegistry::validateFactory(const ResourceRegistration& factory, const ConfigValue& configuration)
        const noexcept
    {
        if (auto valid = validateFactory(factory); !valid)
        {
            return valid;
        }
        const bool is_missing_config = factory.storage->config_type.isValid() && !configuration;
        if (is_missing_config) return cxx::unexpected(RenderError{kStorageContract});
        const bool is_wrong_config_type = configuration && factory.storage->config_type != configuration.type();
        if (is_wrong_config_type)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    ResourceRegistry::Type::Type(ResourceRegistration source, ConfigValue config) noexcept
        : descriptor(*source.descriptor), frame_inputs(descriptor.frame_inputs.begin(), descriptor.frame_inputs.end()),
          frame_channels(source.frame_call.channels.begin(), source.frame_call.channels.end()),
          factory(std::move(source)), configuration(std::move(config))
    {
        const auto own_name = [this](std::string_view text) noexcept -> std::string_view
        {
            return text.empty() ? std::string_view{} : std::string_view{names.emplace_back(text)};
        };
        const auto own_type = [&](cxx::basic_type_info& type) noexcept
        {
            type = {type.hash(), own_name(type.name())};
        };

        descriptor.type = ResourceNameView{own_name(descriptor.type.name())};
        descriptor.description = own_name(descriptor.description);
        own_type(descriptor.binding_type);
        for (auto& type : frame_inputs) own_type(type);
        descriptor.frame_inputs = frame_inputs;
        factory.descriptor = &descriptor;

        command_descriptors.reserve(factory.commands.size());
        command_views.reserve(factory.commands.size());
        command_bindings.reserve(factory.commands.size());
        for (std::size_t i = 0; i < factory.commands.size(); ++i)
        {
            auto& declaration = command_descriptors.emplace_back(*descriptor.commands[i]);
            declaration.name = CommandName{own_name(declaration.name.name())};
            own_type(declaration.request);
            own_type(declaration.response);
            own_type(declaration.data);
            command_views.push_back(&declaration);

            auto& binding = command_bindings.emplace_back(factory.commands[i]);
            binding.descriptor_ = &declaration;
            own_type(binding.storage_type_);
            own_type(binding.method_type_);
        }
        descriptor.commands = command_views;
        factory.commands = command_bindings;

        auto& call = factory.frame_call;
        own_type(call.declaration_type);
        own_type(call.storage_type);
        own_type(call.method_type);
        for (auto& channel : frame_channels)
        {
            own_type(channel.declaration);
            own_type(channel.element);
        }
        call.channels = frame_channels;

        auto& storage = *factory.storage;
        own_type(storage.config_type);
        own_type(storage.ops_type);
        own_type(storage.implementation_type);
        if (storage.snapshot_ops) own_type(storage.snapshot_ops->type);
    }

    ResourceTypeId ResourceRegistry::publishFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        const auto id = types_.emplace(std::move(factory), std::move(configuration));
        types_by_name_.emplace(std::string(types_.find(id)->factory.descriptor->type.name()), id);
        return id;
    }

    RenderResult<ResourceTypeId>
    ResourceRegistry::installFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        if (auto valid = validateFactory(factory, configuration); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return publishFactory(std::move(factory), std::move(configuration));
    }

    ResourceInstanceId ResourceRegistry::reserve(ResourceSet& set, ResourceTypeId type) noexcept
    {
        return entries_.emplace(Entry{&set, 0, type});
    }

    RenderResult<StorageOwner>
    ResourceRegistry::create(
        ResourceCreateContext& dependencies,
        ResourceInstanceId id,
        const void* configuration
    ) noexcept
    {
        const auto type = entries_.find(id)->type;
        return types_.find(type)->factory.storage->create(dependencies, id, configuration);
    }

    void ResourceRegistry::publish(ResourceInstanceId id, std::size_t position, void* instance) noexcept
    {
        // A factory may have reentered another Set and grown the index. Reacquire after it returns.
        auto& entry = *entries_.find(id);
        entry.position = position;
        entry.instance = instance;
        ++creations_;
    }

    void ResourceRegistry::relocate(ResourceInstanceId id, std::size_t position) noexcept
    {
        entries_.find(id)->position = position;
    }

    void ResourceRegistry::revoke(ResourceInstanceId id) noexcept
    {
        if (hasBorrows(id) || hasDependents(id))
        {
            std::terminate();
        }
        for (auto dependency : entries_.find(id)->dependencies)
        {
            auto* owner = entries_.find(dependency);
            if (!owner || !owner->dependent_storages) std::terminate();
            --owner->dependent_storages;
        }
        entries_.erase(id);
    }

    std::optional<std::size_t> ResourceRegistry::position(ResourceInstanceId id, const ResourceSet& set) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance && entry->storage_set == &set ? std::optional{entry->position} : std::nullopt;
    }

    void* ResourceRegistry::resourceInstance(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->instance : nullptr;
    }

} // namespace lux::render
