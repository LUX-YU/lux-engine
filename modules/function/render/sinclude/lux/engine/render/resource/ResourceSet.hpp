#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/engine/render/core/ConfigValue.hpp>
#include <lux/engine/render/extensions/resource/StorageMeta.hpp>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;

    // The Device/Scene/View exclusively owns this set. Accepted operations borrow its instances.
    class LUX_RENDER_PUBLIC ResourceSet final
    {
    public:
        ResourceSet(ResourceRegistry& registry, detail::DeviceSession& device) noexcept;
        ResourceSet(ResourceSet& parent, EResourceDomain domain) noexcept;

        ~ResourceSet() noexcept;
        ResourceSet(const ResourceSet&) = delete;
        ResourceSet& operator=(const ResourceSet&) = delete;
        ResourceSet(ResourceSet&&) = delete;
        ResourceSet& operator=(ResourceSet&&) = delete;

        [[nodiscard]] RenderResult<ResourceInstanceId>
        getOrCreate(ResourceCreateContext&, ResourceTypeId type) noexcept;

        // Caller finishes dependent operations before removal. The ID is revoked before destruction.
        [[nodiscard]] RenderResult<void> remove(ResourceInstanceId id) noexcept;
        [[nodiscard]] ResourceInstanceId find(ResourceTypeId type) const noexcept;
        [[nodiscard]] RenderResult<ResourceInstanceId>
        query(const ResourceCreateContext&, ResourceTypeId) const noexcept;

        [[nodiscard]] ResourceRegistry& registry() const noexcept
        {
            return registry_;
        }

        [[nodiscard]] ResourceSet* ownerFor(EResourceDomain domain) noexcept;

        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

        void clear() noexcept;
        [[nodiscard]] bool hasCommands() const noexcept;

    private:
        friend class ResourceRegistry;
        using TypeKey = std::pair<std::uint32_t, std::uint32_t>;
        using TypeIndex = std::map<TypeKey, ResourceInstanceId, std::less<>>;

        struct Record final
        {
            ResourceInstanceId id;
            TypeIndex::iterator type;
            StorageOwner instance;
        };

        [[nodiscard]] RenderResult<ResourceInstanceId> ensure(ResourceCreateContext&, ResourceTypeId) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        EResourceDomain domain_;
        detail::DeviceSession& device_;
        ResourceSet* parent_;
        std::vector<Record> records_;
        TypeIndex instances_by_type_;
        std::uint32_t constructing_ = 0;
        bool clearing_ = false;
    };
} // namespace lux::render
