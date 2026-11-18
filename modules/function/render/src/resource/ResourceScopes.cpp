#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    RenderResult<ResourceSet*>
    RenderServer::Impl::resourceScope(
        detail::DeviceEntry& device, const VRenderScope& scope, ResourceTypeId type, bool create
    ) noexcept
    {
        const auto* factory = runtime_->resources_.factory(type);
        if (!factory) return cxx::unexpected(RenderError{kStorageFactory});

        SceneId scene;
        ViewId view;
        if (const auto* scene_scope = std::get_if<SceneId>(&scope))
        {
            scene = *scene_scope;
            if (!runtime_->scenes_.find(scene)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        if (const auto* view_scope = std::get_if<ViewRequest>(&scope))
        {
            scene = view_scope->scene;
            view = view_scope->view;
            const auto* logical = runtime_->scenes_.find(scene);
            if (!logical || !logical->views.find(view)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto domain = factory->descriptor->domain;
        const bool missing_owner = (domain == EResourceDomain::SCENE && scene.isNull()) ||
            (domain == EResourceDomain::VIEW && view.isNull());
        if (missing_owner) return cxx::unexpected(RenderError{kStorageMismatch});

        if (domain == EResourceDomain::DEVICE) scene = {};
        if (domain != EResourceDomain::VIEW) view = {};
        if (auto* owner = device.scopedResources(scene, view, create)) return owner;
        const auto context = device.resourceCreateContext();
        if (auto valid = runtime_->resources_.qualify(type, domain, context); !valid)
            return cxx::unexpected(valid.error());
        return cxx::unexpected(RenderError{kStorageIdentity});
    }

    void RenderServer::Impl::queryResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        auto* device = runtime_->device(request.device);
        if (!device)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        const auto type = runtime_->resources_.typeId(request.declaration);
        auto owner = resourceScope(*device, request.scope, type, false);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        const auto context = device->resourceCreateContext();
        const auto id = (*owner)->query(context, type);
        response = id ? ResourceResponse{{}, type, *id} : ResourceResponse{id.error()};
    }

    void RenderServer::Impl::getOrCreateResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        auto* device = runtime_->device(request.device);
        if (!device)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        const auto type = runtime_->resources_.typeId(request.declaration);
        if (const auto failure = device->admissionError(); failure.type)
        {
            response.error = failure;
            return;
        }
        auto owner = resourceScope(*device, request.scope, type, true);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto context = device->resourceCreateContext();
        auto instance = (*owner)->getOrCreate(context, type);
        if (!instance)
        {
            response.error = instance.error();
            return;
        }
        storages_created_.store(runtime_->resources_.creations(), std::memory_order_relaxed);
        response = {{}, type, *instance};
    }
} // namespace lux::render
