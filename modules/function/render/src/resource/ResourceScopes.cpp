#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    RenderResult<ResourceSet*> RenderServer::Impl::jobResourceScope(
        detail::DeviceEntry& device, JobExecution& execution, ViewScope scope, ResourceCreateContext* creation
    ) noexcept
    {
        if (!creation)
        {
            if (auto* existing = execution.privateResources()) return existing;
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto* scene = runtime_->scenes_.find(scope.scene);
        auto* view = scene ? scene->findView(scope.view) : nullptr;
        if (!view) return cxx::unexpected(RenderError{kRuntimeIdentity});
        creation->view_row_ = &view->parameters;
        auto* parent = device.scopedResources(scope.scene, scope.view, creation);
        if (!parent) return cxx::unexpected(RenderError{kStorageIdentity});
        return &execution.privateResources(*parent);
    }

    RenderResult<ResourceSet*>
    RenderServer::Impl::resourceScope(
        detail::DeviceEntry& device, const VResourceScope& scope, ResourceTypeId type, ResourceCreateContext* creation
    ) noexcept
    {
        const auto* factory = runtime_->resources_.factory(type);
        if (!factory) return cxx::unexpected(RenderError{kStorageFactory});

        const auto domain = factory->descriptor->domain;
        if (const auto* id = std::get_if<RenderJobId>(&scope))
        {
            auto* job = runtime_->jobs_.find(*id);
            if (!job) return cxx::unexpected(RenderError{kJobIdentity});
            if (!job->execution || !job->running()) return cxx::unexpected(RenderError{kJobStopped});
            if (job->execution->device != device.id) return cxx::unexpected(RenderError{kStorageMismatch});
            const ViewScope parent{job->content.scene, job->content.view};
            if (domain == EResourceDomain::JOB) return jobResourceScope(device, *job->execution, parent, creation);
            return resourceScope(device, parent, type, creation);
        }
        if (domain == EResourceDomain::JOB) return cxx::unexpected(RenderError{kStorageMismatch});

        SceneId scene;
        ViewId view;
        if (const auto* scene_scope = std::get_if<SceneId>(&scope))
        {
            scene = *scene_scope;
            if (!runtime_->scenes_.find(scene)) return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        if (const auto* view_scope = std::get_if<ViewScope>(&scope))
        {
            scene = view_scope->scene;
            view = view_scope->view;
            auto* logical = runtime_->scenes_.find(scene);
            auto* row = logical ? logical->findView(view) : nullptr;
            if (!row) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (creation) creation->view_row_ = &row->parameters;
        }
        const bool missing_owner = (domain == EResourceDomain::SCENE && scene.isNull()) ||
            (domain == EResourceDomain::VIEW && view.isNull());
        if (missing_owner) return cxx::unexpected(RenderError{kStorageMismatch});

        if (domain == EResourceDomain::DEVICE) scene = {};
        if (domain != EResourceDomain::VIEW) view = {};
        if (auto* owner = device.scopedResources(scene, view, creation)) return owner;
        if (auto valid = runtime_->resources_.qualify(type, domain, device.session().resourceFeatures()); !valid)
            return cxx::unexpected(valid.error());
        return cxx::unexpected(RenderError{kStorageIdentity});
    }

    void RenderServer::Impl::removeResource(const ResourceAddress& resource, AckResponse& reply) noexcept
    {
        auto* entry = runtime_->resources_.entries_.find(resource.instance);
        if (resource.server != instance_id_ || !entry || !entry->instance)
        {
            reply.error = {kStorageIdentity};
            return;
        }
        const auto removed = entry->storage_set->remove(resource.instance);
        reply.error = removed ? RenderError{} : removed.error();
    }

    void RenderServer::Impl::listResourceJobs(const ResourceAddress& resource, ListResourceJobs::Response& reply) noexcept
    {
        if (resource.server != instance_id_ || !runtime_->resources_.resourceInstance(resource.instance))
        {
            reply.error = {kStorageIdentity};
            return;
        }
        for (const auto& job : runtime_->jobs_)
            if (job.usesResource(resource.instance)) reply.jobs.push_back(job.id());
        reply.error = {};
    }

    void RenderServer::Impl::queryResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        auto* device = runtime_->device(request.device);
        if (!device)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        if (device->retiring)
        {
            response.error = {kDeviceBusy};
            return;
        }
        const auto type = runtime_->resources_.typeId(request.declaration);
        auto owner = resourceScope(*device, request.scope, type, nullptr);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        const auto context = device->resourceCreateContext();
        const auto id = (*owner)->query(context, type, request.binding);
        response = id ? ResourceResponse{{}, type, *id, instance_id_} : ResourceResponse{id.error()};
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
        auto context = device->resourceCreateContext();
        auto owner = resourceScope(*device, request.scope, type, &context);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto instance = (*owner)->getOrCreate(context, type, request.binding);
        if (!instance)
        {
            response.error = instance.error();
            return;
        }
        storages_created_.store(runtime_->resources_.creations(), std::memory_order_relaxed);
        response = {{}, type, *instance, instance_id_};
    }
} // namespace lux::render
