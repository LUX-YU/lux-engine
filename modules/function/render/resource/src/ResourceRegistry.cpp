#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <exception>

namespace lux::render
{
    ResourceRegistry::ResourceRegistry(void* native, std::uint32_t capabilities) noexcept
        : native_(native), capabilities_(capabilities) {}

    ResourceRegistry::~ResourceRegistry() noexcept
    {
        if (!entries_.empty()) std::terminate(); // actual Sets must close before their borrowed index
    }

    StorageTypeId ResourceRegistry::typeId(StorageNameView type) const noexcept
    {
        for (const auto& entry : types_)
            if (entry.factory.description->type == type) return entry.id;
        return {};
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

    const void* ResourceRegistry::configuration(StorageNameView type) const noexcept
    {
        const auto index = typeId(type);
        return index.isNull() ? nullptr : types_.find(index)->configuration.data();
    }

    RenderResult<StorageTypeId> ResourceRegistry::installFactory(
        StorageFactory factory, ConfigValue configuration
    ) noexcept
    {
        if (!factory.description) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto valid = validateStorageFactory(*factory.description, factory); !valid)
            return cxx::unexpected(valid.error());
        const auto& type = *factory.description;
        if ((type.required_capabilities & ~capabilities_) != 0)
            return cxx::unexpected(RenderError{kStorageCapability});
        const StorageCppType actual{configuration.type(), configuration.objectSize(), configuration.objectAlignment()};
        if (configuration && factory.config_type != actual)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (!typeId(factory.description->type).isNull())
            return cxx::unexpected(RenderError{kStorageFactory});
        const auto id = types_.emplace(Type{{}, std::move(factory), std::move(configuration)});
        types_.find(id)->id = id;
        return id;
    }

    void* ResourceRegistry::native(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->native : nullptr;
    }

    StorageSet* ResourceRegistry::owner(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry && entry->native ? entry->owner : nullptr;
    }

    RenderResult<StorageLease> ResourceRegistry::lease(StorageId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->native || entry->owner->closing_)
            return cxx::unexpected(RenderError{kStorageIdentity});
        ++entry->owner->records_[entry->position].leases;
        return StorageLease{*entry->owner, id};
    }
}
