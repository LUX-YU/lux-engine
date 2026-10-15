#include <lux/engine/render/resource/ResourceRegistry.hpp>

#include <limits>
#include <exception>

namespace lux::render
{
    ResourceRegistry::ResourceRegistry(void* native, EGraphicsApi api, std::uint32_t capabilities) noexcept
        : native_(native), api_(api), capabilities_(capabilities), identity_(std::make_shared<const std::byte>())
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

    std::size_t ResourceRegistry::typeIndex(StorageTypeIdView type) const noexcept
    {
        for (std::size_t i = 0; i < types_.size(); ++i)
            if (types_[i].factory.description->type == type) return i;
        return types_.size();
    }

    const StorageFactory* ResourceRegistry::factory(StorageTypeIdView type) const noexcept
    {
        const auto index = typeIndex(type);
        return index == types_.size() ? nullptr : &types_[index].factory;
    }

    RenderResult<void> ResourceRegistry::installFactory(StorageFactory factory) noexcept
    {
        if (frozen_ || !factory.description) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto valid = validateStorageFactory(*factory.description, factory, api_); !valid) return valid;
        if (typeIndex(factory.description->type) != types_.size())
            return cxx::unexpected(RenderError{kStorageFactory});
        types_.push_back({std::move(factory), {}});
        return {};
    }

    RenderResult<void> ResourceRegistry::configureType(
        const StorageDescriptor& type, std::shared_ptr<const void> configuration
    ) noexcept
    {
        const auto index = typeIndex(type.type);
        if (frozen_ || index == types_.size()) return cxx::unexpected(RenderError{kStorageFactory});
        auto& installed = types_[index];
        if (auto compatible = compatibleStorage(*installed.factory.description, type); !compatible) return compatible;
        if (installed.configuration || (type.config && !configuration))
            return cxx::unexpected(RenderError{kStorageContract});
        installed.configuration = std::move(configuration);
        return {};
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

    StorageInstanceId ResourceRegistry::local(DomainId domain, std::size_t type, std::string_view key) const noexcept
    {
        for (const auto& entry : entries_)
            if (!entry.closing && entry.domain == domain && entry.type == type && entry.key == key) return entry.id;
        return {};
    }

    RenderResult<StorageInstanceId> ResourceRegistry::find(
        const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, bool ancestors
    ) const noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        const auto index = typeIndex(type.type);
        if (index == types_.size()) return cxx::unexpected(RenderError{kStorageFactory});
        if (auto compatible = compatibleStorage(*types_[index].factory.description, type); !compatible)
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

    RenderResult<StorageInstanceId> ResourceRegistry::getOrCreate(
        const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, const void* config
    ) noexcept
    {
        if (!valid(domain)) return cxx::unexpected(RenderError{kStorageDomain});
        if (key.empty()) return cxx::unexpected(RenderError{kStorageContract});
        const auto index = typeIndex(type.type);
        if (index == types_.size()) return cxx::unexpected(RenderError{kStorageFactory});
        const auto& installed = types_[index];
        if (auto compatible = compatibleStorage(*installed.factory.description, type); !compatible)
            return cxx::unexpected(compatible.error());
        const auto scope = domains_.find(domain.id_)->scope;
        if ((type.scopes & scopeMask(scope)) == 0) return cxx::unexpected(RenderError{kStorageDomain});
        if ((type.required_capabilities & ~capabilities_) != 0)
            return cxx::unexpected(RenderError{kStorageCapability});
        if (!config) config = installed.configuration.get();
        if (type.config && !config) return cxx::unexpected(RenderError{kStorageContract});
        auto id = local(domain.id_, index, key);
        if (!id.isNull())
        {
            if (!installed.factory.compatible_config(entries_.find(id)->native.get(), config))
                return cxx::unexpected(RenderError{kStorageMismatch});
            return id;
        }
        auto candidate = installed.factory.create(native_, config);
        if (!candidate) return cxx::unexpected(candidate.error());
        if (!*candidate) return cxx::unexpected(RenderError{kStorageFactory});
        id = entries_.emplace(Entry{{}, domain.id_, index, std::string(key), std::move(*candidate)});
        entries_.find(id)->id = id;
        ++creations_;
        return id;
    }

    void* ResourceRegistry::native(StorageInstanceId id) const noexcept
    {
        const auto* entry = entries_.find(id);
        return entry ? entry->native.get() : nullptr;
    }

    StorageInstanceId ResourceRegistry::identityOf(const void* instance) const noexcept
    {
        for (const auto& entry : entries_) if (entry.native.get() == instance) return entry.id;
        std::terminate(); // invoked by a live factory-created instance, never an untrusted pointer
    }

    void ResourceRegistry::retain(StorageInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || entry->closing) std::terminate(); // prepared-owner invariant, not an external identity API
        ++entry->references;
    }

    void ResourceRegistry::release(StorageInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || !entry->references) std::terminate();
        if (--entry->references == 0 && entry->closing) destroy(id);
    }

    void ResourceRegistry::rollback(StorageInstanceId id) noexcept
    {
        auto* entry = entries_.find(id);
        if (!entry || entry->references) std::terminate();
        destroy(id);
    }

    void ResourceRegistry::destroy(StorageInstanceId id) noexcept
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
            if (const auto collect = types_[entry.type].factory.collect) collect(entry.native.get());
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
