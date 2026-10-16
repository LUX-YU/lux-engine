#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    struct ResourceDomainTag;
    struct StorageInstanceTag;
    using DomainId = cxx::SlotKey<ResourceDomainTag>;
    using StorageInstanceId = cxx::SlotKey<StorageInstanceTag>;

    class ResourceRegistry;

    // The token proves the registry epoch even after its address is reused. It owns no GPU object.
    // Generation remains exclusively in SlotKey; no second counter is allocated here.
    class ResourceDomain final
    {
    public:
        ResourceDomain() noexcept = default;
        [[nodiscard]] DomainId id() const noexcept { return id_; }
        bool operator==(const ResourceDomain&) const noexcept = default;

    private:
        friend class ResourceRegistry;
        ResourceDomain(DomainId id, std::shared_ptr<const std::byte> owner) noexcept
            : id_(id), owner_(std::move(owner)) {}
        DomainId id_;
        std::shared_ptr<const std::byte> owner_;
    };

    class ResourceRegistry final
    {
    public:
        ResourceRegistry(void* native, std::uint32_t capabilities) noexcept;
        ~ResourceRegistry() noexcept;
        ResourceRegistry(const ResourceRegistry&) = delete;
        ResourceRegistry& operator=(const ResourceRegistry&) = delete;

        [[nodiscard]] RenderResult<void> installFactory(StorageFactory factory) noexcept;
        [[nodiscard]] RenderResult<void>
        configureType(const StorageDescriptor& type, std::shared_ptr<const void> configuration, StorageCppType actual) noexcept;

        template <class Config>
        [[nodiscard]] RenderResult<void>
        configureType(const StorageDescriptor& type, std::shared_ptr<const Config> configuration) noexcept
        {
            return configureType(type, std::move(configuration), storageCppType<Config>());
        }
        void freeze() noexcept { frozen_ = true; }
        [[nodiscard]] bool frozen() const noexcept { return frozen_; }
        [[nodiscard]] const StorageFactory* factory(StorageTypeIdView type) const noexcept;
        [[nodiscard]] const void* configuration(StorageTypeIdView type) const noexcept;

        [[nodiscard]] ResourceDomain root() const noexcept { return {root_, identity_}; }
        [[nodiscard]] RenderResult<ResourceDomain> createDomain(const ResourceDomain& parent, EResourceScope scope) noexcept;
        [[nodiscard]] RenderResult<void> closeDomain(const ResourceDomain& domain) noexcept;
        [[nodiscard]] bool valid(const ResourceDomain& domain) const noexcept;
        [[nodiscard]] RenderResult<ResourceDomain> ancestor(const ResourceDomain& domain, EResourceScope scope) const noexcept;

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        getOrCreate(const ResourceDomain& domain, std::string_view key, const typename T::Config& config) noexcept
        {
            auto id = getOrCreate(domain, key, T::description, &config);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_[entry.type].factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_[entry.type].factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        getOrCreate(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = getOrCreate(domain, key, T::description, nullptr);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_[entry.type].factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_[entry.type].factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        tryGet(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = find(domain, key, T::description, false);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_[entry.type].factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_[entry.type].factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        findVisible(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = find(domain, key, T::description, true);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_[entry.type].factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_[entry.type].factory.ops), false);
        }

        // Cold Native preparation uses the same instance table. These local keys never enter the wire.
        template <class Config>
            requires (!std::is_void_v<Config>)
        [[nodiscard]] RenderResult<StorageInstanceId> getOrCreate(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, const Config* config
        ) noexcept
        {
            return getOrCreateImpl(domain, key, type, config, storageCppType<Config>());
        }

        [[nodiscard]] RenderResult<StorageInstanceId> getOrCreate(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, std::nullptr_t
        ) noexcept
        {
            return getOrCreateImpl(domain, key, type, nullptr, {});
        }

        [[nodiscard]] RenderResult<StorageInstanceId> find(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, bool ancestors = false
        ) const noexcept;
        [[nodiscard]] void* native(StorageInstanceId id) const noexcept;
        [[nodiscard]] StorageInstanceId identityOf(const void* instance) const noexcept;
        void retain(StorageInstanceId id) noexcept;
        void release(StorageInstanceId id) noexcept;
        void rollback(StorageInstanceId id) noexcept;
        void collect() noexcept;
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] std::uint64_t creations() const noexcept { return creations_; }

    private:
        [[nodiscard]] RenderResult<StorageInstanceId> getOrCreateImpl(
            const ResourceDomain&, std::string_view, const StorageDescriptor&, const void*, StorageCppType
        ) noexcept;

        struct Domain final { DomainId id; DomainId parent; EResourceScope scope; bool closing = false; };
        struct Type final { StorageFactory factory; std::shared_ptr<const void> configuration; };
        struct Entry final
        {
            StorageInstanceId id;
            DomainId domain;
            std::size_t type;
            std::string key;
            NativeStorage native;
            std::size_t references = 0;
            bool closing = false;
        };

        [[nodiscard]] std::size_t typeIndex(StorageTypeIdView type) const noexcept;
        [[nodiscard]] StorageInstanceId local(DomainId domain, std::size_t type, std::string_view key) const noexcept;
        void collectClosed() noexcept;
        void destroy(StorageInstanceId id) noexcept;
        void* native_;
        std::uint32_t capabilities_;
        std::shared_ptr<const std::byte> identity_;
        cxx::SlotMap<Domain, ResourceDomainTag> domains_;
        DomainId root_;
        std::vector<Type> types_;
        cxx::SlotMap<Entry, StorageInstanceTag> entries_;
        bool frozen_ = false;
        std::uint64_t creations_ = 0;
    };
}
