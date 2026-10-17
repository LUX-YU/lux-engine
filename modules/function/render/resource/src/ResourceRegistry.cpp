#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include <limits>
#include <exception>

namespace lux::render
{
    ResourceRegistry::ResourceRegistry(void* native, std::uint32_t capabilities) noexcept
        : native_(native), capabilities_(capabilities), identity_(std::make_shared<const std::byte>())
    {
        root_ = domains_.emplace(Domain{{}, {}, EResourceScope::GLOBAL});
        domains_.find(root_)->id = root_;
    }

    ResourceRegistry::~ResourceRegistry() noexcept
    {
        for (auto& entry : entries_) entry.closing = true;
        collectClosed();
        if (!entries_.empty()) std::terminate(); // external users must end before the DeviceSession
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

    bool ResourceRegistry::valid(const ResourceDomain& domain) const noexcept
    {
        if (domain.owner_ != identity_) return false;
        auto* value = domains_.find(domain.id_);
        return value && !value->closing;
    }

    RenderResult<ResourceDomain> ResourceRegistry::createDomain(
        const ResourceDomain& parent, EResourceScope scope
    ) noexcept
    {
        if (!valid(parent)) return cxx::unexpected(RenderError{kStorageDomain});
        const auto parent_scope = domains_.find(parent.id_)->scope;
        const bool allowed = (scope == EResourceScope::SCENE && parent_scope == EResourceScope::GLOBAL) ||
            (scope == EResourceScope::VIEW && parent_scope == EResourceScope::SCENE) ||
            (scope == EResourceScope::ATTACHMENT && parent_scope == EResourceScope::VIEW);
        if (!allowed) return cxx::unexpected(RenderError{kStorageDomain});
        const auto id = domains_.emplace(Domain{{}, parent.id_, scope});
        domains_.find(id)->id = id;
        return ResourceDomain{id, identity_};
    }

    RenderResult<ResourceDomain> ResourceRegistry::ancestor(
        const ResourceDomain& domain, EResourceScope scope
    ) const noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        auto id = domain.id_;
        while (!id.isNull())
        {
            const auto& value = *domains_.find(id);
            if (value.scope == scope) return ResourceDomain{id, identity_};
            id = value.parent;
        }
        return cxx::unexpected(RenderError{kStorageDomain});
    }

    StorageId ResourceRegistry::local(DomainId domain, StorageTypeId type, std::string_view key) const noexcept
    {
        for (const auto& entry : entries_)
            if (entry.native && !entry.closing && entry.domain == domain && entry.type == type && entry.key == key) return entry.id;
        return {};
    }

    RenderResult<StorageId> ResourceRegistry::find(
        const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, bool ancestors
    ) const noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        const auto index = typeId(type.type);
        if (index.isNull()) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto compatible = compatibleStorage(*types_.find(index)->factory.description, type); !compatible)
            return cxx::unexpected(compatible.error());
        auto current = domain.id_;
        while (!current.isNull())
        {
            auto id = local(current, index, key);
            if (!id.isNull()) return id;
            if (!ancestors) break;
            current = domains_.find(current)->parent;
        }
        return cxx::unexpected(RenderError{kStorageDomain});
    }

    RenderResult<StorageId> ResourceRegistry::getOrCreateImpl(
        const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, const void* config,
        StorageCppType actual, StorageCppType required_ops
    ) noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        if (key.empty()) return cxx::unexpected(RenderError{kStorageContract});
        const auto index = typeId(type.type);
        if (index.isNull()) return cxx::unexpected(RenderError{kStorageFactory});
        const auto& installed = *types_.find(index);
        if (auto compatible = compatibleStorage(*installed.factory.description, type); !compatible)
            return cxx::unexpected(compatible.error());
        if ((type.required_capabilities & ~capabilities_) != 0)
            return cxx::unexpected(RenderError{kStorageCapability});
        // Typed callers qualify their Config and Ops before any new-key Native creation. Untyped
        // prepared Native consumers use the already-proven installed Ops and pass no requested Ops.
        const bool is_wrong_config = actual.size != 0 && installed.factory.config_type != actual;
        const bool is_wrong_ops = required_ops.size != 0 && installed.factory.ops_type != required_ops;
        if (is_wrong_config || is_wrong_ops) return cxx::unexpected(RenderError{kStorageMismatch});
        if (!config) config = installed.configuration.data();
        if (!config) return cxx::unexpected(RenderError{kStorageContract});
        auto id = local(domain.id_, index, key);
        if (!id.isNull())
        {
            if (!installed.factory.compatible_config(entries_.find(id)->native.get(), config))
                return cxx::unexpected(RenderError{kStorageMismatch});
            return id;
        }
        // The sole instance index allocates the identity before construction. The reserved entry has
        // no Native address and cannot be found by key until the Factory succeeds. Failure revokes it.
        id = entries_.emplace(Entry{{}, domain.id_, index, std::string(key),
            NativeStorage{nullptr, [](void*) noexcept {}}});
        entries_.find(id)->id = id;
        auto candidate = installed.factory.create(native_, id, config);
        if (!candidate || !*candidate)
        {
            const auto error = candidate ? RenderError{kStorageFactory} : candidate.error();
            entries_.erase(id);
            return cxx::unexpected(error);
        }
        entries_.find(id)->native = std::move(*candidate);
        ++creations_;
        return id;
    }

    void* ResourceRegistry::native(StorageId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->native.get() : nullptr;
    }

    void ResourceRegistry::retain(StorageId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || entry->closing) std::terminate(); // prepared-owner invariant, not an external identity API
        ++entry->references;
    }

    void ResourceRegistry::release(StorageId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->references) std::terminate();
        if (--entry->references == 0 && entry->closing) destroy(id);
    }

    void ResourceRegistry::rollback(StorageId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || entry->references) std::terminate();
        destroy(id);
    }

    void ResourceRegistry::destroy(StorageId id) noexcept
    {
        // A professional Storage may release another instance (e.g. a material's texture reference).
        // Run its deleter only after SlotMap mutation is complete, while the factory code pin is live.
        auto owner = std::move(entries_.find(id)->native);
        entries_.erase(id);
    }

    void ResourceRegistry::collectClosed() noexcept
    {
        // Dense erase swaps the last record into this index; inspect it without restarting the scan.
        std::size_t index = 0;
        while (index < entries_.size())
        {
            const auto& entry = *(entries_.begin() + index);
            if (entry.closing && entry.references == 0) destroy(entry.id);
            else ++index;
        }
    }

    void ResourceRegistry::collect() noexcept
    {
        for (auto& entry : entries_)
            if (const auto collect = types_.find(entry.type)->factory.collect) collect(entry.native.get());
    }

    RenderResult<void> ResourceRegistry::closeDomain(const ResourceDomain& domain) noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        // Depth is fixed by the four legal scopes. Keep parents until descendants have been classified.
        std::vector<DomainId> closing;
        for (const auto& value : domains_)
        {
            const auto key = value.id;
            auto current = key;
            while (!current.isNull())
            {
                if (current == domain.id_) { closing.push_back(key); break; }
                current = domains_.find(current)->parent;
            }
        }
        for (auto key : closing)
        {
            domains_.find(key)->closing = true;
            for (auto& entry : entries_) if (entry.domain == key) entry.closing = true;
        }
        collectClosed();
        for (auto key : closing) domains_.erase(key);
        return {};
    }
}
