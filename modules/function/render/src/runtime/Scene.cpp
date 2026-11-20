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
        const RemoveScene::Request& _request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        const auto scene_id = input->scene;
        auto* scene = runtime_->scenes_.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const bool has_jobs = std::ranges::any_of(scene->views, [](const auto& view) noexcept
        {
            return !view.jobs.empty();
        });
        const bool has_resource_borrows = std::ranges::any_of(runtime_->devices_, [scene_id](const auto& device) noexcept
        {
            return device->scopedResourceBorrows(scene_id);
        });
        if (has_jobs || has_resource_borrows)
        {
            output_response.error = {kDeviceBusy};
            return;
        }
        for (auto& device : runtime_->devices_) device->eraseResourceScope(scene_id);
        runtime_->scenes_.erase(scene_id);
        output_response = {};
    }

    void
    RenderServer::Impl::createView(const CreateView::Request& _request, CreateView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = runtime_->scenes_.find(input->scene);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = ViewCreated{{}, scene->views.emplace()};
    }

    void
    RenderServer::Impl::removeView(const RemoveView::Request& _request, RemoveView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = runtime_->scenes_.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        const bool has_resource_borrows = std::ranges::any_of(runtime_->devices_, [&](const auto& device) noexcept
        {
            return device->scopedResourceBorrows(input->scene, input->view);
        });
        if (has_resource_borrows || !view->jobs.empty())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        for (auto& device : runtime_->devices_) device->eraseResourceScope(input->scene, input->view);
        scene->views.erase(input->view);
        output_response = {};
    }
} // namespace lux::render
