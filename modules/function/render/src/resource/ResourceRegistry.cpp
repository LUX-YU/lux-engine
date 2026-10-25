#include <exception>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>

namespace lux::render
{
    ResourceRegistry::ResourceRegistry(std::uint32_t capabilities) noexcept : capabilities_(capabilities) {}

    ResourceRegistry::ScopeKey ResourceRegistry::scopeKey(const VRenderScope& scope) noexcept
    {
        const auto key = [](auto id) noexcept { return (static_cast<std::uint64_t>(id.gen) << 32) | id.index; };
        return std::visit(
            [&](const auto& value) noexcept -> ScopeKey
            {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, GlobalRenderScope>)
                {
                    return {0};
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    return {1, key(value)};
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    return {2, key(value.scene), key(value.view)};
                }
                else
                {
                    return {3, key(value)};
                }
            },
            scope
        );
    }

    void ResourceRegistry::bindScope(const VRenderScope& scope, StorageSet& set) noexcept
    {
        scopes_.emplace(scopeKey(scope), &set);
    }

    void ResourceRegistry::unbindScope(const VRenderScope& scope) noexcept
    {
        scopes_.erase(scopeKey(scope));
    }

    StorageId ResourceRegistry::inputStorage(const CapabilityInput& input) const noexcept
    {
        const auto found = scopes_.find(scopeKey(input.scope));
        return found == scopes_.end() ? StorageId{} : found->second->inputStorage(input.implementation, input.index);
    }

    ResourceRegistry::~ResourceRegistry() noexcept
    {
        if (!entries_.empty())
        {
            std::terminate(); // actual Sets must close before their borrowed index
        }
    }

    StorageTypeId ResourceRegistry::typeId(StorageNameView type) const noexcept
    {
        const auto found = types_by_name_.find(type.name());
        return found == types_by_name_.end() ? StorageTypeId{} : found->second;
    }

    const StorageFactory* ResourceRegistry::factory(StorageTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->factory : nullptr;
    }

    const StorageFactory* ResourceRegistry::factory(StorageNameView type) const noexcept
    {
        const auto index = typeId(type);
        return index.isNull() ? nullptr : &types_.find(index)->factory;
    }

    const ConfigValue* ResourceRegistry::configuration(StorageTypeId type) const noexcept
    {
        const auto* entry = types_.find(type);
        return entry ? &entry->configuration : nullptr;
    }

    RenderResult<void> ResourceRegistry::validateFactory(const StorageFactory& factory) const noexcept
    {
        if (!factory.descriptor)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (auto valid = validateStorageFactory(*factory.descriptor, factory); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const auto& type = *factory.descriptor;
        if ((type.required_capabilities & ~capabilities_) != 0)
        {
            return cxx::unexpected(RenderError{kStorageCapability});
        }
        const auto existing = typeId(type.type);
        if (!existing.isNull())
        {
            const auto& previous = *types_.find(existing);
            if (auto valid = compatibleStorage(*previous.factory.descriptor, type); !valid)
            {
                return valid;
            }
            const bool is_factory_mismatch =
                previous.factory.implementation_type != factory.implementation_type ||
                previous.factory.config_type != factory.config_type || previous.factory.ops_type != factory.ops_type ||
                previous.factory.create != factory.create || previous.factory.ops != factory.ops ||
                previous.factory.binding_ops != factory.binding_ops ||
                previous.factory.compatible_config != factory.compatible_config;
            if (is_factory_mismatch)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
        }
        return {};
    }

    RenderResult<void> ResourceRegistry::validateFactory(
        const StorageFactory& factory,
        const ConfigValue& configuration
    ) const noexcept
    {
        if (auto valid = validateFactory(factory); !valid)
        {
            return valid;
        }
        const auto existing = typeId(factory.descriptor->type);
        const bool is_wrong_config_type = configuration && factory.config_type != configuration.type();
        const bool is_default_mismatch = !existing.isNull() && types_.find(existing)->configuration != configuration;
        if (is_wrong_config_type || is_default_mismatch)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        return {};
    }

    StorageTypeId ResourceRegistry::publishFactory(StorageFactory factory, ConfigValue configuration) noexcept
    {
        const auto existing = typeId(factory.descriptor->type);
        if (!existing.isNull())
        {
            return existing;
        }
        const auto id = types_.emplace(Type{{}, std::move(factory), std::move(configuration)});
        types_.find(id)->id = id;
        types_by_name_.emplace(std::string(types_.find(id)->factory.descriptor->type.name()), id);
        return id;
    }

    RenderResult<StorageTypeId> ResourceRegistry::installFactory(
        StorageFactory factory,
        ConfigValue configuration
    ) noexcept
    {
        if (auto valid = validateFactory(factory, configuration); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return publishFactory(std::move(factory), std::move(configuration));
    }

    StorageId ResourceRegistry::reserve(StorageSet& set, StorageTypeId type) noexcept
    {
        return entries_.emplace(Entry{&set, 0, type});
    }

    RenderResult<StorageInstance> ResourceRegistry::create(
        StorageCreateContext& dependencies,
        StorageId id,
        const void* configuration
    ) noexcept
    {
        const auto type = entries_.find(id)->type;
        return types_.find(type)->factory.create(&dependencies, id, configuration);
    }

    void ResourceRegistry::publish(StorageId id, std::size_t position, void* instance) noexcept
    {
        // A factory may have reentered another Set and grown the index. Reacquire after it returns.
        auto& entry = *entries_.find(id);
        entry.position = position;
        entry.instance = instance;
        ++creations_;
    }

    void ResourceRegistry::relocate(StorageId id, std::size_t position) noexcept
    {
        entries_.find(id)->position = position;
    }

    void ResourceRegistry::revoke(StorageId id) noexcept
    {
        entries_.erase(id);
    }

    std::optional<std::size_t> ResourceRegistry::position(StorageId id, const StorageSet& set) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance && entry->storage_set == &set ? std::optional{entry->position} : std::nullopt;
    }

    void* ResourceRegistry::storageInstance(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->instance : nullptr;
    }

    StorageSet* ResourceRegistry::storageSet(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->instance ? entry->storage_set : nullptr;
    }

} // namespace lux::render
