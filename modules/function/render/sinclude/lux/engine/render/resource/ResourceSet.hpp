#pragma once

#include <lux/engine/render/visibility.h>

#include <exception>
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
        explicit ResourceSet(
            ResourceRegistry& registry,
            EResourceDomain domain = EResourceDomain::SCENE,
            const ResourceSet* parent = nullptr
        ) noexcept
            : registry_(registry), domain_(domain), parent_(parent)
        {
            const bool is_foreign_parent = parent_ && &parent_->registry_ != &registry_;
            const bool is_wrong_parent_domain =
                parent_ && static_cast<unsigned>(domain_) != static_cast<unsigned>(parent_->domain_) + 1;
            const bool is_invalid_parent = is_foreign_parent || is_wrong_parent_domain;
            if (is_invalid_parent)
            {
                std::terminate();
            }
        }

        ~ResourceSet() noexcept;
        ResourceSet(const ResourceSet&) = delete;
        ResourceSet& operator=(const ResourceSet&) = delete;
        ResourceSet(ResourceSet&&) = delete;
        ResourceSet& operator=(ResourceSet&&) = delete;

        template <class T>
        [[nodiscard]] RenderResult<ResourceInstanceId> getOrCreate(ResourceCreateContext& dependencies) noexcept
        {
            return getOrCreateImpl(
                dependencies,
                T::descriptor,
                []() noexcept
                {
                    if constexpr (std::is_void_v<typename T::Config>)
                    {
                        return cxx::type_info{};
                    }
                    else
                    {
                        return cxx::make_basic_type_info<typename T::Config>();
                    }
                }(),
                storageOpsType<T>()
            );
        }

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

        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

        void clear() noexcept;
        [[nodiscard]] bool hasCommands() const noexcept;

    private:
        friend class ResourceRegistry;
        using NameKey = std::pair<std::uint32_t, std::uint32_t>;
        using NameIndex = std::map<NameKey, ResourceInstanceId, std::less<>>;

        struct Record final
        {
            ResourceInstanceId id;
            NameIndex::iterator name;
            StorageOwner instance;
        };

        [[nodiscard]] RenderResult<ResourceInstanceId>
        getOrCreateImpl(ResourceCreateContext&, const ResourceDefinition&, cxx::type_info, cxx::type_info) noexcept;
        [[nodiscard]] RenderResult<ResourceInstanceId> ensure(ResourceCreateContext&, ResourceTypeId) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        EResourceDomain domain_;
        const ResourceSet* parent_;
        std::vector<Record> records_;
        NameIndex instances_by_name_;
        const detail::DeviceSession* device_ = nullptr;
        std::uint32_t constructing_ = 0;
        bool clearing_ = false;
    };
} // namespace lux::render
