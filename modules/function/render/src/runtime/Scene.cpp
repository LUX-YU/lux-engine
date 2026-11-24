#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void RenderServer::Impl::createScene(
        const CreateScene::Request&,
        CreateScene::Response& output_response
    ) noexcept
    {
        output_response = SceneCreated{{}, runtime_->scenes_.emplace()};
    }

    void RenderServer::Impl::removeScene(
        const RemoveScene::Request& request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto scene_id = request.scene;
        auto* scene = runtime_->scenes_.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const bool has_jobs = scene->hasJobs();
        const bool has_resource_borrows = std::ranges::any_of(runtime_->devices_, [scene_id](const auto& device) noexcept
        {
            return device->scopedResourceBorrows(scene_id);
        });
        if (has_jobs || has_resource_borrows)
        {
            output_response.error = {kDeviceBusy};
            return;
        }
        for (auto& device : runtime_->devices_) device->eraseSceneResources(scene_id);
        runtime_->scenes_.erase(scene_id);
        output_response = {};
    }

    void
    RenderServer::Impl::createView(const CreateView::Request& request, CreateView::Response& output_response) noexcept
    {
        auto* scene = runtime_->scenes_.find(request.scene);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const auto view = scene->createView([this](const runtime_detail::DeviceViewUse& use) noexcept
        {
            const auto* device = runtime_->device(use.device);
            return !device || (!device->session().terminalError().type && device->session().reached(use.accepted));
        });
        output_response = {{}, view};
    }

    void RenderServer::Impl::updateView(const UpdateView::Request& request, AckResponse& response) noexcept
    {
        auto* scene = runtime_->scenes_.find(request.scope.scene);
        auto* view = scene ? scene->findView(request.scope.view) : nullptr;
        if (!view)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        const auto result = view->parameters.update(std::span{&request.parameters, 1});
        response.error = result ? RenderError{} : result.error();
    }

    void
    RenderServer::Impl::removeView(const RemoveView::Request& request, RemoveView::Response& output_response) noexcept
    {
        auto* scene = runtime_->scenes_.find(request.scene);
        auto* view = scene ? scene->findView(request.view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const bool has_resource_borrows = std::ranges::any_of(runtime_->devices_, [&](const auto& device) noexcept
        {
            return device->scopedResourceBorrows(request.scene, request.view);
        });
        if (has_resource_borrows || !view->jobs.empty())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        std::vector<runtime_detail::DeviceViewUse> pending;
        for (auto& device : runtime_->devices_)
        {
            auto uses = device->retireViewResources(request.scene, request.view);
            if (uses.size()) pending.push_back({device->id, std::move(uses)});
        }
        scene->removeView(request.view, std::move(pending));
        output_response = {};
    }
} // namespace lux::render
