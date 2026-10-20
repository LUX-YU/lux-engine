#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/StorageFactory.hpp>
#include <map>
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
                        return CppType{};
                    }
                    else
                    {
                        return cppType<typename T::Config>();
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
        using NameKey = std::tuple<std::uint32_t, std::uint32_t, std::string>;
        using NameIndex = std::map<NameKey, StorageId, std::less<>>;

        struct Record final
        {
            StorageId id;
            NameIndex::iterator name;
            StorageInstance native;
        };

        [[nodiscard]] RenderResult<StorageId> getOrCreateImpl(
            StorageCreateContext&,
            const StorageDescriptor&,
            std::string_view,
            const ConfigValue&,
            CppType,
            CppType
        ) noexcept;
        [[nodiscard]] RenderResult<StorageId> ensure(
            StorageCreateContext&,
            StorageTypeId,
            std::string_view,
            const ConfigValue&,
            CppType = {},
            CppType = {}
        ) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        bool global_;
        std::vector<Record> records_;
        NameIndex instances_by_name_;
    };
} // namespace lux::render
