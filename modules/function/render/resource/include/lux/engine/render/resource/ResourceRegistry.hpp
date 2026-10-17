#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/cxx/container/StableSlotMap.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    struct ResourceDomainTag;
    using DomainId = cxx::SlotKey<ResourceDomainTag>;

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

        // Owner-thread cold transaction; no instance creation or Native side effect.
        [[nodiscard]] RenderResult<StorageTypeId>
        installFactory(StorageFactory factory, ConfigValue configuration = {}) noexcept;

        template <class Config>
        [[nodiscard]] RenderResult<StorageTypeId>
        installFactory(StorageFactory factory, Config configuration) noexcept
        {
            auto value = ConfigValue::make(std::move(configuration), factory.code_pin);
            if (!value) return cxx::unexpected(value.error());
            return installFactory(std::move(factory), std::move(*value));
        }
        // Cold observation of the sole type directory; the visitor must not mutate it.
        template <class Visitor>
        void forEachType(Visitor&& visitor) const noexcept
        {
            for (const auto& entry : types_)
            {
                const auto& type = *entry.factory.description;
                visitor(entry.id, type, capabilities_ & (type.required_capabilities | type.optional_capabilities));
            }
        }
        [[nodiscard]] StorageTypeId typeId(StorageNameView name) const noexcept;
        [[nodiscard]] const StorageFactory* factory(StorageTypeId type) const noexcept;
        [[nodiscard]] const StorageFactory* factory(StorageNameView type) const noexcept;
        [[nodiscard]] const void* configuration(StorageNameView type) const noexcept;

        [[nodiscard]] ResourceDomain root() const noexcept { return {root_, identity_}; }
        [[nodiscard]] RenderResult<ResourceDomain> createDomain(const ResourceDomain& parent, EResourceScope scope) noexcept;
        [[nodiscard]] RenderResult<void> closeDomain(const ResourceDomain& domain) noexcept;
        [[nodiscard]] bool valid(const ResourceDomain& domain) const noexcept;
        [[nodiscard]] RenderResult<ResourceDomain> ancestor(const ResourceDomain& domain, EResourceScope scope) const noexcept;

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        getOrCreate(const ResourceDomain& domain, std::string_view key, const typename T::Config& config) noexcept
        {
            auto id = getOrCreateImpl(domain, key, T::description, &config,
                storageCppType<typename T::Config>(), storageCppType<typename T::Ops>());
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_.find(entry.type)->factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        getOrCreate(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = getOrCreateImpl(domain, key, T::description, nullptr,
                storageCppType<typename T::Config>(), storageCppType<typename T::Ops>());
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_.find(entry.type)->factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        tryGet(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = find(domain, key, T::description, false);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_.find(entry.type)->factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_.find(entry.type)->factory.ops), true);
        }

        template <class T>
        [[nodiscard]] RenderResult<typename T::Access>
        findVisible(const ResourceDomain& domain, std::string_view key) noexcept
        {
            auto id = find(domain, key, T::description, true);
            if (!id) return cxx::unexpected(id.error());
            const auto& entry = *entries_.find(*id);
            if (types_.find(entry.type)->factory.ops_type != storageCppType<typename T::Ops>())
                return cxx::unexpected(RenderError{kStorageMismatch});
            return typename T::Access(entry.native.get(), *static_cast<const typename T::Ops*>(
                types_.find(entry.type)->factory.ops), false);
        }

        // Cold Native preparation uses the same instance table. These local keys never enter the wire.
        template <class Config>
            requires (!std::is_void_v<Config>)
        [[nodiscard]] RenderResult<StorageId> getOrCreate(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, const Config* config
        ) noexcept
        {
            return getOrCreateImpl(domain, key, type, config, storageCppType<Config>());
        }

        [[nodiscard]] RenderResult<StorageId> getOrCreate(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, std::nullptr_t
        ) noexcept
        {
            return getOrCreateImpl(domain, key, type, nullptr, {});
        }

        [[nodiscard]] RenderResult<StorageId> find(
            const ResourceDomain& domain, std::string_view key, const StorageDescriptor& type, bool ancestors = false
        ) const noexcept;
        [[nodiscard]] void* native(StorageId id) const noexcept;
        [[nodiscard]] StorageId identityOf(const void* instance) const noexcept;
        void retain(StorageId id) noexcept;
        void release(StorageId id) noexcept;
        void rollback(StorageId id) noexcept;
        void collect() noexcept;
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] std::uint64_t creations() const noexcept { return creations_; }

    private:
        [[nodiscard]] RenderResult<StorageId> getOrCreateImpl(
            const ResourceDomain&, std::string_view, const StorageDescriptor&, const void*, StorageCppType,
            StorageCppType required_ops = {}
        ) noexcept;

        struct Domain final { DomainId id; DomainId parent; EResourceScope scope; bool closing = false; };
        struct Type final { StorageTypeId id; StorageFactory factory; ConfigValue configuration; };
        struct Entry final
        {
            StorageId id;
            DomainId domain;
            StorageTypeId type;
            std::string key;
            NativeStorage native;
            std::size_t references = 0;
            bool closing = false;
        };

        [[nodiscard]] StorageId local(DomainId domain, StorageTypeId type, std::string_view key) const noexcept;
        void collectClosed() noexcept;
        void destroy(StorageId id) noexcept;
        void* native_;
        std::uint32_t capabilities_;
        std::shared_ptr<const std::byte> identity_;
        cxx::SlotMap<Domain, ResourceDomainTag> domains_;
        DomainId root_;
        cxx::StableSlotMap<Type, StorageTypeTag> types_;
        cxx::SlotMap<Entry, StorageTag> entries_;
        std::uint64_t creations_ = 0;
    };
}
