#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/data/ConfigValue.hpp>
#include <string>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;
    class StorageSet;

    // One owner-thread logical use. The actual Set must outlive its leases.
    class StorageLease final
    {
    public:
        StorageLease() noexcept = default;
        ~StorageLease() noexcept;
        StorageLease(StorageLease&& other) noexcept;
        StorageLease& operator=(StorageLease&& other) noexcept;
        StorageLease(const StorageLease&) = delete;
        StorageLease& operator=(const StorageLease&) = delete;
        [[nodiscard]] StorageId id() const noexcept { return id_; }
        [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
        void reset() noexcept;

    private:
        friend class StorageSet;
        friend class ResourceRegistry;
        StorageLease(StorageSet& owner, StorageId id) noexcept : owner_(&owner), id_(id) {}
        StorageSet* owner_ = nullptr;
        StorageId id_;
    };

    // Actual Scene/View/Attachment/Device member. No parent, scope, identity or global instance owner.
    class StorageSet final
    {
    public:
        explicit StorageSet(ResourceRegistry& registry) noexcept : registry_(registry) {}
        ~StorageSet() noexcept;
        StorageSet(const StorageSet&) = delete;
        StorageSet& operator=(const StorageSet&) = delete;
        StorageSet(StorageSet&&) = delete;
        StorageSet& operator=(StorageSet&&) = delete;

        template <class T>
        [[nodiscard]] RenderResult<StorageLease> acquire(std::string_view key, ConfigValue config = {}) noexcept
        {
            return acquireImpl(T::description, key, std::move(config),
                storageCppType<typename T::Config>(), storageCppType<typename T::Ops>());
        }

        [[nodiscard]] RenderResult<StorageLease>
        acquire(StorageTypeId type, std::string_view key, ConfigValue config = {}) noexcept;

        // Explicit preload is distinct from Attach/operation leases. Duplicate release cannot consume a lease.
        [[nodiscard]] RenderResult<StorageId>
        prepare(StorageTypeId type, std::string_view key, ConfigValue config = {}) noexcept;
        [[nodiscard]] RenderResult<void> releasePrepared(StorageId id) noexcept;
        [[nodiscard]] StorageId find(StorageTypeId type, std::string_view key) const noexcept;
        [[nodiscard]] ResourceRegistry& registry() const noexcept { return registry_; }
        [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
        void clear() noexcept;

    private:
        friend class ResourceRegistry;
        friend class StorageLease;
        struct Record final
        {
            StorageId id;
            StorageTypeId type;
            std::string key;
            ConfigValue config;
            NativeStorage native;
            std::size_t leases = 0;
            std::size_t preloads = 0;
        };

        [[nodiscard]] RenderResult<StorageLease> acquireImpl(
            const StorageDescriptor&, std::string_view, ConfigValue, StorageCppType, StorageCppType
        ) noexcept;
        [[nodiscard]] RenderResult<StorageId> ensure(
            StorageTypeId, std::string_view, ConfigValue, StorageCppType = {}, StorageCppType = {}
        ) noexcept;
        void release(StorageId id) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        std::vector<Record> records_;
        bool closing_ = false;
    };
}
