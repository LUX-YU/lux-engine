#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render::runtime_detail
{
    RenderView::RenderView(RenderScene& parent) noexcept
        : scene(parent), storages(parent.storages, EResourceDomain::VIEW)
    {
    }

    RenderScene::RenderScene(detail::DeviceEntry& entry) noexcept
        : device(entry), storages(entry.resources(), EResourceDomain::SCENE)
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
        output_response = SceneCreated{{}, domain_->scenes_.emplace(*domain_->device_)};
    }

    void RenderServer::Impl::removeScene(
        const RemoveScene::Request& _request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        const auto scene_id = input->scene;
        auto* scene = domain_->scenes_.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (scene->storages.hasCommands())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        for (auto& view : scene->views)
        {
            if (view.storages.hasCommands() || !view.jobs.empty())
            {
                output_response.error = RenderError{kDeviceBusy};
                return;
            }
        }
        domain_->scenes_.erase(scene_id);
        output_response = {};
    }

    void
    RenderServer::Impl::createView(const CreateView::Request& _request, CreateView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = domain_->scenes_.find(input->scene);
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
        auto* scene = domain_->scenes_.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (view->storages.hasCommands() || !view->jobs.empty())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        scene->views.erase(input->view);
        output_response = {};
    }
} // namespace lux::render
