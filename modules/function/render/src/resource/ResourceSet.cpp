#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <utility>

namespace lux::render
{
    ResourceSet::ResourceSet(ResourceRegistry& registry, detail::DeviceEntry& device) noexcept
        : registry_(registry), domain_(EResourceDomain::DEVICE), device_(device), parent_(nullptr)
    {
    }

    ResourceSet::ResourceSet(ResourceSet& parent, EResourceDomain domain) noexcept
        : registry_(parent.registry_), domain_(domain), device_(parent.device_), parent_(&parent)
    {
        const bool is_direct_child =
            (parent.domain_ == EResourceDomain::DEVICE && domain_ == EResourceDomain::SCENE) ||
            (parent.domain_ == EResourceDomain::SCENE && domain_ == EResourceDomain::VIEW) ||
            (parent.domain_ == EResourceDomain::VIEW && domain_ == EResourceDomain::JOB);
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

    bool ResourceSet::contains(ResourceInstanceId id) const noexcept
    {
        return registry_.position(id, *this).has_value();
    }

    ResourceInstanceId ResourceSet::find(ResourceTypeId type, std::string_view binding) const noexcept
    {
        const auto found = instances_.find(std::tuple{type.index, type.gen, binding});
        if (found == instances_.end())
        {
            return {};
        }
        return registry_.declarationOf(found->second) == type ? found->second : ResourceInstanceId{};
    }

    RenderResult<ResourceInstanceId>
    ResourceSet::query(const ResourceCreateContext& context, ResourceTypeId type, std::string_view binding) const noexcept
    {
        if (closed_ || clearing_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const bool is_foreign_context = &device_ != &context.device_owner_ || &registry_ != &context.resources;
        if (is_foreign_context)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (const auto id = find(type, binding); !id.isNull())
        {
            return id;
        }
        if (auto qualified = registry_.qualify(type, domain_, context.device.resourceFeatures()); !qualified)
        {
            return cxx::unexpected(qualified.error());
        }
        return cxx::unexpected(RenderError{kStorageIdentity});
    }

    RenderResult<ResourceInstanceId>
    ResourceSet::getOrCreate(ResourceCreateContext& dependencies, ResourceTypeId type, std::string_view binding) noexcept
    {
        if (closed_ || clearing_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const bool is_foreign_context = &device_ != &dependencies.device_owner_ || &registry_ != &dependencies.resources;
        if (is_foreign_context)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto key = std::tuple{type.index, type.gen, binding};
        if (const auto found = instances_.find(key); found != instances_.end())
        {
            auto* instance = registry_.resourceInstance(found->second);
            // A recursive request for this still-unpublished declaration is a construction cycle.
            if (!instance)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
            return found->second;
        }
        const auto qualified = registry_.qualifyType(type, domain_, dependencies.device.resourceFeatures());
        if (!qualified) return cxx::unexpected(qualified.error());
        const auto id = registry_.reserve(*this, **qualified);
        const auto position = instances_.emplace(BindingKey{type.index, type.gen, std::string{binding}}, id).first;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                instances_.erase(position);
                registry_.revoke(id);
            }
        );
        ++constructing_;
        auto instance = registry_.create(dependencies, id);
        --constructing_;
        if (!instance || !*instance)
        {
            return cxx::unexpected(instance ? RenderError{kStorageFactory} : instance.error());
        }
        records_.push_back(Record{id, position, std::move(*instance)});
        registry_.publish(id, records_.size() - 1, records_.back().instance.get());
        // The candidate owns its entire Job scope. Only shared scopes need per-instance rollback.
        if (dependencies.created_ && domain_ != EResourceDomain::JOB) dependencies.created_->push_back(id);
        rollback.release();
        return id;
    }

    RenderResult<void> ResourceSet::remove(ResourceInstanceId id) noexcept
    {
        const auto position = registry_.position(id, *this);
        if (!position)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (registry_.hasBorrows(id) || registry_.hasDependents(id))
        {
            return cxx::unexpected(RenderError{kDeviceBusy});
        }
        erase(*position);
        return {};
    }

    void ResourceSet::erase(std::size_t position) noexcept
    {
        const auto id = records_[position].id;
        instances_.erase(records_[position].binding);
        registry_.unpublish(id);
        auto instance = std::move(records_[position].instance);
        records_.erase(records_.begin() + position);
        for (auto i = position; i < records_.size(); ++i) registry_.relocate(records_[i].id, i);

        // Its ID is inaccessible, but its forward dependencies still protect the destructor's inputs.
        instance.reset();
        registry_.revoke(id);
    }

    void ResourceSet::retireJob(std::unique_ptr<ResourceSet> scope) noexcept
    {
        if (!scope) return;
        if (scope->domain_ != EResourceDomain::JOB) std::terminate();
        scope->close();
        if (scope->hasBorrows()) scope->parent_->retiring_jobs_.push_back(std::move(scope));
    }

    bool ResourceSet::hasBorrows() const noexcept
    {
        return constructing_ || clearing_ || !retiring_jobs_.empty() || std::ranges::any_of(
            records_,
            [&](const auto& record) noexcept { return registry_.hasBorrows(record.id); }
        );
    }

    void ResourceSet::clear() noexcept
    {
        if (hasBorrows()) std::terminate();
        if (records_.empty()) return;
        if (registry_.hasSurvivingDependencyOn(*this)) std::terminate();
        clearing_ = true;
        for (const auto& record : records_) registry_.unpublish(record.id);
        instances_.clear();
        while (!records_.empty())
        {
            const auto id = records_.back().id;
            auto instance = std::move(records_.back().instance);
            records_.pop_back();
            instance.reset();
            registry_.revoke(id);
        }
        clearing_ = false;
    }
} // namespace lux::render
