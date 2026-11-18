#include <algorithm>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <exception>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>

namespace lux::render
{
    ResourceCommandUse::ResourceCommandUse(
        ResourceRegistry& registry,
        ResourceInstanceId id,
        void* instance,
        CommandBindingLocation binding,
        cxx::basic_type_info implementation
    ) noexcept
        : registry_(&registry), id_(id), instance_(instance), binding_(binding), implementation_(implementation)
    {
    }

    ResourceCommandUse::~ResourceCommandUse() noexcept
    {
        reset();
    }

    ResourceCommandUse::ResourceCommandUse(ResourceCommandUse&& other) noexcept
        : registry_(std::exchange(other.registry_, nullptr)), id_(other.id_),
          instance_(std::exchange(other.instance_, nullptr)), binding_(other.binding_),
          implementation_(other.implementation_)
    {
    }

    ResourceCommandUse& ResourceCommandUse::operator=(ResourceCommandUse&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            registry_ = std::exchange(other.registry_, nullptr);
            id_ = other.id_;
            instance_ = std::exchange(other.instance_, nullptr);
            binding_ = other.binding_;
            implementation_ = other.implementation_;
        }
        return *this;
    }

    void ResourceCommandUse::reset() noexcept
    {
        if (auto* registry = std::exchange(registry_, nullptr))
        {
            registry->releaseCommand(id_);
        }
        instance_ = nullptr;
    }

    RenderResult<ResourceCommandUse>
    ResourceRegistry::acquireCommand(ResourceInstanceId id, CommandId command, bool exclusive) noexcept
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
        return ResourceCommandUse{
            *this, id, entry->instance, *location, definition.factory.storage->implementation_type
        };
    }

    bool ResourceRegistry::hasCommands(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->command_uses != 0;
    }

    bool ResourceRegistry::hasExclusiveCommand(ResourceInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->exclusive_command;
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

    void ResourceRegistry::releaseCommand(ResourceInstanceId id) noexcept
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

    ResourceTypeId ResourceRegistry::publishFactory(ResourceRegistration factory, ConfigValue configuration) noexcept
    {
        const auto id = types_.emplace(Type{std::move(factory), std::move(configuration)});
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
        if (hasCommands(id))
        {
            std::terminate();
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

#include <lux/engine/render/resource/texture/SampledTexture2DStorage.hpp>

namespace lux::render
{
    RenderResult<ResourceRegistry::SampledImage>
    ResourceRegistry::sampledImage(
        const detail::DeviceSession& device, SampledImageReference input, std::uint32_t slot
    ) const noexcept
    {
        if (!input.texture.isNull() && input.target.isNull())
        {
            const auto parent = extension_detail::ResourceIdentity::parent(input.texture);
            const auto* entry = entries_.find(parent);
            if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
            if (&entry->storage_set->device_ != &device)
                return cxx::unexpected(RenderError{kStorageMismatch});
            auto storage = borrow<detail::SampledTexture2DStorage>(parent);
            const auto* value = storage ? (*storage)->find(input.texture) : nullptr;
            if (!value) return cxx::unexpected(RenderError{kStorageIdentity});
            return SampledImage{&value->image(), value->gpuReference().image_index};
        }
        if (input.texture.isNull() && !input.target.isNull() && resolve_output_)
            return resolve_output_(output_owner_, device, input.target, slot);
        return cxx::unexpected(RenderError{kFrameContract});
    }
}
