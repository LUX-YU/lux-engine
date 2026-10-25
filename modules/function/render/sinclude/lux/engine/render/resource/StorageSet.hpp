#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/storage/StorageFactory.hpp>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;

    // The Device/Scene/View/Attachment exclusively owns this set. Accepted operations borrow its instances.
    class LUX_RENDER_PUBLIC StorageSet final
    {
    public:
        explicit StorageSet(ResourceRegistry& registry, bool global = false) noexcept
            : registry_(registry), global_(global)
        {
        }

        [[nodiscard]] bool isGlobal() const noexcept
        {
            return global_;
        }

        ~StorageSet() noexcept;
        StorageSet(const StorageSet&) = delete;
        StorageSet& operator=(const StorageSet&) = delete;
        StorageSet(StorageSet&&) = delete;
        StorageSet& operator=(StorageSet&&) = delete;

        template <class T>
        [[nodiscard]] RenderResult<StorageId> getOrCreate(
            StorageCreateContext& dependencies,
            std::string_view key,
            const ConfigValue& config = {}
        ) noexcept
        {
            return getOrCreateImpl(
                dependencies,
                T::descriptor,
                key,
                config,
                []() noexcept
                {
                    if constexpr (std::is_void_v<typename T::Config>)
                    {
                        return cxx::TypeToken{};
                    }
                    else
                    {
                        return cxx::typeToken<typename T::Config>();
                    }
                }(),
                storageOpsType<T>()
            );
        }

        [[nodiscard]] RenderResult<StorageId> getOrCreate(
            StorageCreateContext&,
            StorageTypeId type,
            std::string_view key,
            const ConfigValue& config = {}
        ) noexcept;

        // Caller finishes dependent operations before removal. The ID is revoked before destruction.
        [[nodiscard]] RenderResult<void> remove(StorageId id) noexcept;
        [[nodiscard]] StorageId find(StorageTypeId type, std::string_view key) const noexcept;

        [[nodiscard]] ResourceRegistry& registry() const noexcept
        {
            return registry_;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

        void clear() noexcept;

    private:
        friend class ResourceRegistry;
        friend class RenderServer;
        // Called after the containing business object's identity has been published.
        void bindScope(VRenderScope) noexcept;
        void bindInput(CapabilitySelection, std::uint32_t input, StorageId) noexcept;
        using InputKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;
        [[nodiscard]] StorageId inputStorage(CapabilitySelection, std::uint32_t input) const noexcept;
        using NameKey = std::tuple<std::uint32_t, std::uint32_t, std::string>;
        using NameIndex = std::map<NameKey, StorageId, std::less<>>;

        struct Record final
        {
            StorageId id;
            NameIndex::iterator name;
            StorageInstance instance;
        };

        [[nodiscard]] RenderResult<StorageId> getOrCreateImpl(
            StorageCreateContext&,
            const StorageDescriptor&,
            std::string_view,
            const ConfigValue&,
            cxx::TypeToken,
            cxx::TypeToken
        ) noexcept;
        [[nodiscard]] RenderResult<StorageId>
        ensure(StorageCreateContext&, StorageTypeId, std::string_view, const ConfigValue&) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        bool global_;
        std::vector<Record> records_;
        NameIndex instances_by_name_;
        std::optional<VRenderScope> scope_;
        std::map<InputKey, StorageId> input_bindings_;
    };
} // namespace lux::render
