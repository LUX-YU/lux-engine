#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/resource/StorageMeta.hpp>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;

    // One Device, Scene, View or installed Job owns this set. Accepted operations borrow its instances.
    class LUX_RENDER_PUBLIC ResourceSet final
    {
    public:
        ResourceSet(ResourceRegistry& registry, detail::DeviceEntry& device) noexcept;
        ResourceSet(ResourceSet& parent, EResourceDomain domain) noexcept;

        ~ResourceSet() noexcept;
        ResourceSet(const ResourceSet&) = delete;
        ResourceSet& operator=(const ResourceSet&) = delete;
        ResourceSet(ResourceSet&&) = delete;
        ResourceSet& operator=(ResourceSet&&) = delete;

        [[nodiscard]] RenderResult<ResourceInstanceId>
        getOrCreate(ResourceCreateContext&, ResourceTypeId type, std::string_view binding = {}) noexcept;

        // Caller finishes dependent operations before removal. The ID is revoked before destruction.
        [[nodiscard]] RenderResult<void> remove(ResourceInstanceId id) noexcept;
        [[nodiscard]] bool contains(ResourceInstanceId) const noexcept;
        [[nodiscard]] ResourceInstanceId find(ResourceTypeId type, std::string_view binding = {}) const noexcept;
        [[nodiscard]] RenderResult<ResourceInstanceId>
        query(const ResourceCreateContext&, ResourceTypeId, std::string_view binding = {}) const noexcept;

        [[nodiscard]] ResourceRegistry& registry() const noexcept
        {
            return registry_;
        }

        [[nodiscard]] ResourceSet* ownerFor(EResourceDomain domain) noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

        // Stop closes new uses immediately. The View owns a detached Job scope until its last command returns.
        void close() noexcept { closed_ = true; }
        static void retireJob(std::unique_ptr<ResourceSet>) noexcept;
        void clear() noexcept;
        [[nodiscard]] bool hasBorrows() const noexcept;

    private:
        friend class ResourceRegistry;
        using BindingKey = std::tuple<std::uint32_t, std::uint32_t, std::string>;
        using BindingIndex = std::map<BindingKey, ResourceInstanceId, std::less<>>;

        struct Record final
        {
            ResourceInstanceId id;
            BindingIndex::iterator binding;
            StorageOwner instance;
        };

        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        EResourceDomain domain_;
        detail::DeviceEntry& device_;
        ResourceSet* parent_;
        std::vector<Record> records_;
        BindingIndex instances_;
        std::uint32_t constructing_ = 0;
        bool clearing_ = false;
        bool closed_ = false;
        std::vector<std::unique_ptr<ResourceSet>> retiring_jobs_;
    };
} // namespace lux::render
