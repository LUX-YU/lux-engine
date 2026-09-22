#include <lux/engine/render/RenderView.hpp>
#include <lux/engine/render/detail/ViewResources.hpp>

namespace lux::render
{
RenderView::RenderView(std::shared_ptr<detail::ViewResources> resources) noexcept : resources_(std::move(resources))
{
}

RenderView::~RenderView() noexcept
{
    // Already-admitted storage owns creation replies and release intent.
    // Destruction does not allocate, publish, wait or pump the owner.
    resources_->close_requested = true;
}

RenderViewId RenderView::id() const noexcept
{
    return resources_->status.view;
}
ViewStatus RenderView::status() const noexcept
{
    return resources_->status;
}
ViewHandle RenderView::handle() const noexcept
{
    return resources_->view;
}

RenderResult<void> RenderView::requestExtent(PixelExtent extent) noexcept
{
    return resources_->requestExtent(extent);
}

RenderResult<void> RenderView::setOutput(ViewStamp desired, bool enabled) noexcept
{
    auto &state = *resources_;
    if (auto checked = state.check(); !checked)
    {
        return checked;
    }
    if (state.close_requested)
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::STOPPING, {}, id()});
    }
    if (state.status.failure)
    {
        return lux::cxx::unexpected(*state.status.failure);
    }
    state.desired = desired;
    state.scene_enabled = enabled;
    return {};
}

RenderResult<ViewImage> RenderView::acquireImage()
{
    auto &state = *resources_;
    if (!std::holds_alternative<SampledOutput>(state.output))
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::INVALID_ARGUMENT, {}, id()});
    }
    if (auto checked = state.check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (state.status.failure)
    {
        return lux::cxx::unexpected(*state.status.failure);
    }
    if (state.close_requested || state.status.state != EViewState::READY)
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::NOT_READY, {}, id()});
    }
    auto record = std::make_shared<ViewImageLease::Record>();
    record->version = state.version;
    record->scene_enabled = state.scene_enabled;
    record->content = {state.desired, 0, EImageEvidence::REQUESTED};
    record->content.source.surface_generation = state.version->generation;
    ViewImage image{state.version->texture, state.version->extent, id(), record->content, {}};
    image.lease.record_ = std::move(record);
    return image;
}

RenderResult<void> RenderView::beginClose() noexcept
{
    return resources_->beginClose();
}

RenderResult<ERenderClose> RenderView::advanceClose() const noexcept
{
    if (auto checked = resources_->check(); !checked)
    {
        return lux::cxx::unexpected(checked.error());
    }
    if (!resources_->close_requested)
    {
        return lux::cxx::unexpected(RendererFailure{ERendererError::BUSY, {}, id()});
    }
    return resources_->status.state == EViewState::CLOSED ? ERenderClose::COMPLETE : ERenderClose::PENDING;
}
} // namespace lux::render
