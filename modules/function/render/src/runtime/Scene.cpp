#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    RenderView::RenderView(RenderScene& parent) noexcept
        : scene(parent), resources(parent.resources, EResourceDomain::VIEW)
    {
    }

    RenderScene::RenderScene(detail::DeviceEntry& entry) noexcept
        : device(entry), resources(entry.resources(), EResourceDomain::SCENE)
    {
    }
} // namespace lux::render::runtime_detail

namespace lux::render
{
    void RenderServer::Impl::createScene(
        const CreateScene::Request&,
        CreateScene::Response& output_response
    ) noexcept
    {
        auto* device = runtime_->execution_device_;
        if (!device)
        {
            output_response.error = {kRuntimeState};
            return;
        }
        output_response = SceneCreated{{}, runtime_->scenes_.emplace(*device)};
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
        if (scene->resources.hasCommands())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        for (auto& view : scene->views)
        {
            if (view.resources.hasCommands() || !view.jobs.empty())
            {
                output_response.error = RenderError{kDeviceBusy};
                return;
            }
        }
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
        output_response = ViewCreated{{}, scene->views.emplace(*scene)};
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
        if (view->resources.hasCommands() || !view->jobs.empty())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        scene->views.erase(input->view);
        output_response = {};
    }
} // namespace lux::render
