#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/resource/StorageSet.hpp>

namespace lux::render
{
    // Temporary cold installation values; no Native instance or second type catalog.
    struct StorageFactoryInstall final
    {
        StorageFactory factory;
        ConfigValue configuration;
    };

    class LUX_RENDER_PUBLIC ResourceRegistry final
    {
    public:
        ResourceRegistry(void* native, std::uint32_t capabilities) noexcept;
        ~ResourceRegistry() noexcept;
        ResourceRegistry(const ResourceRegistry&) = delete;
        ResourceRegistry& operator=(const ResourceRegistry&) = delete;

        // Configuration/device-thread cold transaction; no instance creation or GPU side effect.
        [[nodiscard]] RenderResult<StorageTypeId> installFactory(
            StorageFactory factory,
            ConfigValue configuration = {}
        ) noexcept;

        template <class Config>
        [[nodiscard]] RenderResult<StorageTypeId> installFactory(StorageFactory factory, Config configuration) noexcept
        {
            auto value = ConfigValue::make(std::move(configuration));
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            return installFactory(std::move(factory), std::move(*value));
        }

        // All candidates are qualified before any type is visible. Failure preserves the input values.
        [[nodiscard]] RenderResult<std::vector<StorageTypeId>> installFactories(
            std::span<StorageFactoryInstall> candidates
        ) noexcept;

        // Cold observation of the sole type directory; the visitor must not mutate it.
        template <class Visitor> void forEachType(Visitor&& visitor) const noexcept
        {
            for (const auto& entry : types_)
            {
                const auto& type = *entry.factory.descriptor;
                visitor(entry.id, type, capabilities_ & (type.required_capabilities | type.optional_capabilities));
            }
        }

        [[nodiscard]] StorageTypeId typeId(StorageNameView name) const noexcept;
        [[nodiscard]] const StorageFactory* factory(StorageTypeId type) const noexcept;
        [[nodiscard]] const StorageFactory* factory(StorageNameView type) const noexcept;
        [[nodiscard]] const ConfigValue* configuration(StorageTypeId type) const noexcept;

        // Boundary lookup; the instance index borrows, never owns a Native object.
        template <class T>
            requires(!std::is_void_v<typename T::Ops>)
        [[nodiscard]] RenderResult<typename T::Access> tryGet(StorageId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->native)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const auto& factory = types_.find(entry->type)->factory;
            const bool is_wrong_interface =
                factory.descriptor->type != T::descriptor.type || factory.ops_type != cppType<typename T::Ops>();
            if (is_wrong_interface)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return typename T::Access(entry->native, *static_cast<const typename T::Ops*>(factory.ops));
        }

        template <class Native> [[nodiscard]] RenderResult<Native*> borrow(StorageId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->native)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (types_.find(entry->type)->factory.native_type != cppType<Native>())
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return static_cast<Native*>(entry->native);
        }

        [[nodiscard]] void* native(StorageId id) const noexcept;
        [[nodiscard]] StorageSet* storageSet(StorageId id) const noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return entries_.size();
        }

        [[nodiscard]] std::uint64_t creations() const noexcept
        {
            return creations_;
        }

        // StorageSet owns instances; this index alone mutates their identities and borrowed locations.
        [[nodiscard]] StorageId reserve(StorageSet&, StorageTypeId) noexcept;
        [[nodiscard]] RenderResult<StorageInstance> create(StorageId, const void* configuration) noexcept;
        void publish(StorageId, std::size_t position, void* instance) noexcept;
        void relocate(StorageId, std::size_t position) noexcept;
        void revoke(StorageId) noexcept;
        [[nodiscard]] std::optional<std::size_t> position(StorageId, const StorageSet&) const noexcept;

    private:
        struct Type final
        {
            StorageTypeId id;
            StorageFactory factory;
            ConfigValue configuration;
        };

        struct Entry final
        {
            StorageSet* storage_set;
            std::size_t position;
            StorageTypeId type;
            void* native = nullptr;
        };

        [[nodiscard]] RenderResult<void> validateFactory(const StorageFactory&, const ConfigValue&) const noexcept;
        StorageTypeId publishFactory(StorageFactory, ConfigValue) noexcept;
        void* native_;
        std::uint32_t capabilities_;
        cxx::StableSlotMap<Type, StorageTypeTag> types_;
        cxx::SlotMap<Entry, StorageTag> entries_;
        std::map<std::string, StorageTypeId, std::less<>> types_by_name_;
        std::uint64_t creations_ = 0;
    };
} // namespace lux::render
