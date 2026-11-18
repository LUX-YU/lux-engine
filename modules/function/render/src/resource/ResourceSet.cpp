#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <utility>

namespace lux::render
{
    ResourceSet::ResourceSet(ResourceRegistry& registry, detail::DeviceSession& device) noexcept
        : registry_(registry), domain_(EResourceDomain::DEVICE), device_(device), parent_(nullptr)
    {
    }

    ResourceSet::ResourceSet(ResourceSet& parent, EResourceDomain domain) noexcept
        : registry_(parent.registry_), domain_(domain), device_(parent.device_), parent_(&parent)
    {
        const bool is_direct_child =
            (parent.domain_ == EResourceDomain::DEVICE && domain_ == EResourceDomain::SCENE) ||
            (parent.domain_ == EResourceDomain::SCENE && domain_ == EResourceDomain::VIEW);
        if (!is_direct_child)
        {
            std::terminate();
        }
    }

    ResourceSet* ResourceSet::ownerFor(EResourceDomain domain) noexcept
    {
        for (auto* owner = this; owner; owner = owner->parent_)
        {
            if (owner->domain_ == domain)
            {
                return owner;
            }
        }
        return nullptr;
    }

    ResourceSet::~ResourceSet() noexcept
    {
        clear();
    }

    ResourceInstanceId ResourceSet::find(ResourceTypeId type) const noexcept
    {
        const auto found = instances_by_type_.find(TypeKey{type.index, type.gen});
        if (found == instances_by_type_.end())
        {
            return {};
        }
        return registry_.declarationOf(found->second) == type ? found->second : ResourceInstanceId{};
    }

    RenderResult<ResourceInstanceId>
    ResourceSet::query(const ResourceCreateContext& context, ResourceTypeId type) const noexcept
    {
        if (clearing_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const bool is_foreign_context = &device_ != &context.device || &registry_ != &context.resources;
        if (is_foreign_context)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (const auto id = find(type); !id.isNull())
        {
            return id;
        }
        if (auto qualified = registry_.qualify(type, domain_, context); !qualified)
        {
            return cxx::unexpected(qualified.error());
        }
        return cxx::unexpected(RenderError{kStorageIdentity});
    }

    RenderResult<ResourceInstanceId>
    ResourceSet::ensure(ResourceCreateContext& dependencies, ResourceTypeId type) noexcept
    {
        if (clearing_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const bool is_foreign_context = &device_ != &dependencies.device || &registry_ != &dependencies.resources;
        if (is_foreign_context)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto key = TypeKey{type.index, type.gen};
        if (const auto found = instances_by_type_.find(key); found != instances_by_type_.end())
        {
            auto* instance = registry_.resourceInstance(found->second);
            // A recursive request for this still-unpublished declaration is a construction cycle.
            if (!instance)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            return found->second;
        }
        if (auto qualified = registry_.qualify(type, domain_, dependencies); !qualified)
        {
            return cxx::unexpected(qualified.error());
        }
        const auto& configuration = *registry_.configuration(type);
        const auto id = registry_.reserve(*this, type);
        const auto type_position = instances_by_type_.emplace(key, id).first;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                instances_by_type_.erase(type_position);
                registry_.revoke(id);
            }
        );
        ++constructing_;
        auto instance = registry_.create(dependencies, id, configuration.data());
        --constructing_;
        if (!instance || !*instance)
        {
            return cxx::unexpected(instance ? RenderError{kStorageFactory} : instance.error());
        }
        records_.push_back(Record{id, type_position, std::move(*instance)});
        registry_.publish(id, records_.size() - 1, records_.back().instance.get());
        rollback.release();
        return id;
    }

    RenderResult<ResourceInstanceId>
    ResourceSet::getOrCreate(ResourceCreateContext& dependencies, ResourceTypeId type) noexcept
    {
        return ensure(dependencies, type);
    }

    RenderResult<void> ResourceSet::remove(ResourceInstanceId id) noexcept
    {
        const auto position = registry_.position(id, *this);
        if (!position)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (registry_.hasCommands(id))
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        erase(*position);
        return {};
    }

    void ResourceSet::erase(std::size_t position) noexcept
    {
        const auto& record = records_[position];
        instances_by_type_.erase(record.type);
        registry_.revoke(record.id);
        auto instance = std::move(records_[position].instance);
        if (position + 1 != records_.size())
        {
            records_[position] = std::move(records_.back());
            registry_.relocate(records_[position].id, position);
        }
        records_.pop_back();
        // All indexes are repaired before a Implementation destructor can release another actual instance.
    }

    bool ResourceSet::hasCommands() const noexcept
    {
        return std::ranges::any_of(
            records_,
            [&](const auto& record) noexcept { return registry_.hasCommands(record.id); }
        );
    }

    void ResourceSet::clear() noexcept
    {
        if (constructing_ || clearing_ || hasCommands())
        {
            std::terminate();
        }
        clearing_ = true;
        for (const auto& record : records_)
        {
            registry_.revoke(record.id);
        }
        instances_by_type_.clear();
        // Revoke the entire set before any destructor runs. No temporary container or allocation is needed.
        while (!records_.empty())
        {
            auto instance = std::move(records_.back().instance);
            records_.pop_back();
        }
        clearing_ = false;
    }
} // namespace lux::render
