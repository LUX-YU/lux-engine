#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    RenderResult<ResourceSet*>
    RenderServer::Impl::resourceScope(const VRenderScope& scope, ResourceTypeId type) noexcept
    {
        const auto* factory = runtime_->resources_.factory(type);
        if (!factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        auto actual = findScope(scope);
        if (!actual)
        {
            return cxx::unexpected(actual.error());
        }
        if (auto* owner = (*actual)->ownerFor(factory->descriptor->domain))
        {
            return owner;
        }
        return cxx::unexpected(RenderError{kStorageMismatch});
    }

    void RenderServer::Impl::queryResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        const auto type = runtime_->resources_.typeId(request.declaration);
        auto owner = resourceScope(request.scope, type);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto context = runtime_->execution_device_->resourceCreateContext();
        const auto id = (*owner)->query(context, type);
        if (!id)
        {
            response.error = id.error();
            return;
        }
        response = {{}, type, *id};
    }

    void RenderServer::Impl::getOrCreateResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        const auto type = runtime_->resources_.typeId(request.declaration);
        auto owner = resourceScope(request.scope, type);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto context = runtime_->execution_device_->resourceCreateContext();
        auto instance = (*owner)->getOrCreate(context, type);
        if (!instance)
        {
            response.error = instance.error();
            return;
        }
        storages_created_.store(runtime_->resources_.creations(), std::memory_order_relaxed);
        response = {{}, type, *instance};
    }

    RenderResult<ResourceSet*> RenderServer::Impl::findScope(const VRenderScope& scope) noexcept
    {
        return std::visit(
            [&](const auto& location) noexcept -> RenderResult<ResourceSet*>
            {
                using T = std::remove_cvref_t<decltype(location)>;
                if constexpr (std::same_as<T, GlobalRenderScope>)
                {
                    auto* device = runtime_->execution_device_;
                    if (!device) return cxx::unexpected(RenderError{kRuntimeState});
                    return &device->resources();
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    if (auto* scene = runtime_->scenes_.find(location))
                    {
                        return &scene->resources;
                    }
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    auto* scene = runtime_->scenes_.find(location.scene);
                    auto* view = scene ? scene->views.find(location.view) : nullptr;
                    if (view)
                    {
                        return &view->resources;
                    }
                }
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            },
            scope
        );
    }

} // namespace lux::render
