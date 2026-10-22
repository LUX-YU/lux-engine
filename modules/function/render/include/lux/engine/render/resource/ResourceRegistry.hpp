#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/container/StableSlotMap.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <map>
#include <optional>

namespace lux::render
{
    class StorageSet;
    class RenderServer;

    class LUX_RENDER_PUBLIC ResourceRegistry final
    {
    public:
        explicit ResourceRegistry(std::uint32_t capabilities) noexcept;
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

        [[nodiscard]] std::uint32_t capabilities() const noexcept
        {
            return capabilities_;
        }

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

        // Boundary lookup; the instance index borrows, never owns a Implementation object.
        template <class T>
            requires(!std::is_void_v<typename T::Ops>)
        [[nodiscard]] RenderResult<typename T::Access> tryGet(StorageId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->instance)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const auto& factory = types_.find(entry->type)->factory;
            const bool is_wrong_interface =
                factory.descriptor->type != T::descriptor.type || factory.ops_type != cxx::typeToken<typename T::Ops>();
            if (is_wrong_interface)
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return typename T::Access(entry->instance, *static_cast<const typename T::Ops*>(factory.ops));
        }

        template <class Implementation> [[nodiscard]] RenderResult<Implementation*> borrow(StorageId id) noexcept
        {
            auto* entry = entries_.find(id);
            if (!entry || !entry->instance)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (types_.find(entry->type)->factory.implementation_type != cxx::typeToken<Implementation>())
            {
                return cxx::unexpected(RenderError{kStorageMismatch});
            }
            return static_cast<Implementation*>(entry->instance);
        }

        // Non-owning boundary lookup; valid until its StorageSet removes the instance.
        [[nodiscard]] void* storageInstance(StorageId id) const noexcept;
        [[nodiscard]] StorageSet* storageSet(StorageId id) const noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return entries_.size();
        }

        [[nodiscard]] std::uint64_t creations() const noexcept
        {
            return creations_;
        }

    private:
        friend class StorageSet;
        friend class RenderServer;

        // Internal registration primitives: built-ins publish known declarations directly;
        // external registration validates the complete closure before calling publishFactory().
        [[nodiscard]] RenderResult<void> validateFactory(const StorageFactory&) const noexcept;
        [[nodiscard]] RenderResult<void> validateFactory(const StorageFactory&, const ConfigValue&) const noexcept;
        StorageTypeId publishFactory(StorageFactory, ConfigValue = {}) noexcept;

        // StorageSet owns instances; this index alone mutates their identities and borrowed locations.
        [[nodiscard]] StorageId reserve(StorageSet&, StorageTypeId) noexcept;
        [[nodiscard]] RenderResult<StorageInstance> create(
            StorageCreateContext&,
            StorageId,
            const void* configuration
        ) noexcept;
        void publish(StorageId, std::size_t position, void* instance) noexcept;
        void relocate(StorageId, std::size_t position) noexcept;
        void revoke(StorageId) noexcept;
        [[nodiscard]] std::optional<std::size_t> position(StorageId, const StorageSet&) const noexcept;

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
            void* instance = nullptr;
        };

        std::uint32_t capabilities_;
        cxx::StableSlotMap<Type, StorageTypeTag> types_;
        cxx::SlotMap<Entry, StorageTag> entries_;
        std::map<std::string, StorageTypeId, std::less<>> types_by_name_;
        std::uint64_t creations_ = 0;
    };
} // namespace lux::render
