#include <lux/engine/editor/rendering/detail/ViewResources.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace lux::editor::rendering
{
    struct RenderView::Impl final
    {
        std::unique_ptr<detail::ViewResources> resources;
    };
    RenderView::RenderView(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }
    RenderView::~RenderView() noexcept = default;
    RenderResult<std::unique_ptr<RenderView>> RenderView::create(EditorRenderer &renderer,
                                                                 lux::render::RenderControlSession &control,
                                                                 RenderViewId id, lux::render::RenderSceneId scene,
                                                                 ViewConfig config) noexcept
    {
        try
        {
            auto impl = std::make_unique<Impl>();
            // Allocate the owner before the stateful resource record; no request is sent from this factory.
            auto result = std::unique_ptr<RenderView>(new RenderView(std::move(impl)));
            result->impl_->resources = std::make_unique<detail::ViewResources>(renderer, control, id, scene, config);
            return result;
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(RendererFailure{ERendererError::ALLOCATION_FAILURE, {}, id});
        }
    }
    RenderViewId RenderView::id() const noexcept
    {
        return impl_->resources->status.view;
    }
    ViewStatus RenderView::status() const noexcept
    {
        return impl_->resources->status;
    }
    RenderResult<void> RenderView::requestExtent(PixelExtent extent) noexcept
    {
        return impl_->resources->requestExtent(extent);
    }
    lux::render::ViewHandle RenderView::handle() const noexcept
    {
        return impl_->resources->view;
    }

    RenderResult<void> RenderView::setOutput(ViewStamp desired, bool scene_enabled) noexcept
    {
        auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
        {
            return checked;
        }
        if (state.close_requested)
        {
            return lux::cxx::unexpected(RendererFailure{ERendererError::STOPPING, {}, id()});
        }
        if (state.status.state == EViewState::FAILED && state.status.failure)
        {
            return lux::cxx::unexpected(*state.status.failure);
        }
        state.desired = desired;
        state.scene_enabled = scene_enabled;
        return {};
    }
    RenderResult<ViewImage> RenderView::acquireImage() noexcept
    {
        auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (state.status.state == EViewState::FAILED && state.status.failure)
            return lux::cxx::unexpected(*state.status.failure);
        if (state.status.state != EViewState::READY)
            return lux::cxx::unexpected(RendererFailure{ERendererError::NOT_READY, {}, id()});
        try
        {
            auto record = std::make_shared<ViewImageLease::Record>();
            record->version = state.version;
            record->scene_enabled = state.scene_enabled;
            record->content = {state.desired, 0, EImageEvidence::REQUESTED};
            record->content.source.surface_generation = state.version->generation;
            ViewImage image{state.version->texture, state.version->extent, id(), record->content, {}};
            image.lease.record_ = std::move(record);
            return image;
        }
        catch (const std::bad_alloc &)
        {
            return lux::cxx::unexpected(RendererFailure{ERendererError::ALLOCATION_FAILURE, {}, id()});
        }
    }
    RenderResult<void> RenderView::pollResources() noexcept
    {
        return impl_->resources->prepareControlStep();
    }
    std::optional<RendererFailure> RenderView::takeFailure() noexcept
    {
        return std::exchange(impl_->resources->pending_failure, std::nullopt);
    }
    RenderResult<void> RenderView::beginClose() noexcept
    {
        return impl_->resources->beginClose();
    }
    RenderResult<ERenderClose> RenderView::advanceClose() noexcept
    {
        auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (!state.close_requested)
            return lux::cxx::unexpected(RendererFailure{ERendererError::BUSY, {}, id()});
        // Poll belongs to EditorRenderer. The view only observes its owned release protocol here.
        return state.status.state == EViewState::CLOSED ? ERenderClose::COMPLETE : ERenderClose::PENDING;
    }
} // namespace lux::editor::rendering
