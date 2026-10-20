#include <exception>
#include <lux/engine/render/resource/ResourceRegistry.hpp>

namespace lux::render
{
    ResourceRegistry::ResourceRegistry(void* native, std::uint32_t capabilities) noexcept
        : native_(native), capabilities_(capabilities)
    {
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

    RenderResult<void> ResourceRegistry::validateFactory(
        const StorageFactory& factory,
        const ConfigValue& configuration
    ) const noexcept
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
        const auto actual = configuration.typeInfo();
        if (configuration && factory.config_type != actual)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (!typeId(factory.descriptor->type).isNull())
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        return {};
    }

    StorageTypeId ResourceRegistry::publishFactory(StorageFactory factory, ConfigValue configuration) noexcept
    {
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

    RenderResult<std::vector<StorageTypeId>> ResourceRegistry::installFactories(
        std::span<StorageFactoryInstall> candidates
    ) noexcept
    {
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            const auto& candidate = candidates[i];
            if (auto valid = validateFactory(candidate.factory, candidate.configuration); !valid)
            {
                return cxx::unexpected(valid.error());
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (candidates[j].factory.descriptor->type == candidate.factory.descriptor->type)
                {
                    return cxx::unexpected(RenderError{kStorageFactory});
                }
            }
        }
        types_.reserve(types_.size() + candidates.size());
        std::vector<StorageTypeId> ids;
        ids.reserve(candidates.size());
        for (auto& candidate : candidates)
        {
            ids.push_back(publishFactory(std::move(candidate.factory), std::move(candidate.configuration)));
        }
        return ids;
    }

    StorageId ResourceRegistry::reserve(StorageSet& set, StorageTypeId type) noexcept
    {
        return entries_.emplace(Entry{&set, 0, type});
    }

    RenderResult<StorageInstance> ResourceRegistry::create(StorageId id, const void* configuration) noexcept
    {
        const auto type = entries_.find(id)->type;
        return types_.find(type)->factory.create(native_, id, configuration);
    }

    void ResourceRegistry::publish(StorageId id, std::size_t position, void* instance) noexcept
    {
        // A factory may have reentered another Set and grown the index. Reacquire after it returns.
        auto& entry = *entries_.find(id);
        entry.position = position;
        entry.native = instance;
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
        return entry && entry->native && entry->storage_set == &set ? std::optional{entry->position} : std::nullopt;
    }

    void* ResourceRegistry::native(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->native : nullptr;
    }

    StorageSet* ResourceRegistry::storageSet(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->native ? entry->storage_set : nullptr;
    }

} // namespace lux::render
