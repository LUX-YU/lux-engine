#include <lux/engine/editor/rendering/detail/ViewResources.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#endif

namespace lux::editor::rendering
{
    struct RenderView::Impl final
    {
        std::unique_ptr<detail::ViewResources> resources;
    };
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
    std::uint64_t detail::RendererTestAccess::inFlightResize(const RenderView &view) noexcept
    {
        const auto &resources = *view.impl_->resources;
        return resources.resize.valid() ? resources.in_flight_sequence : 0;
    }
#endif
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
    RenderResult<void> RenderView::setCamera(const CameraFrame &camera) noexcept
    {
        auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
            return checked;
        if (state.close_requested)
            return lux::cxx::unexpected(RendererFailure{ERendererError::STOPPING, {}, id()});
        const auto finite = [](double value) { return std::isfinite(value) && std::abs(value) <= FLT_MAX; };
        const bool valid_values = std::all_of(camera.view.begin(), camera.view.end(), finite) &&
                                  std::all_of(camera.projection.begin(), camera.projection.end(), finite) &&
                                  std::all_of(camera.origin.begin(), camera.origin.end(),
                                              [](double value)
                                              {
                                                  return std::isfinite(value) &&
                                                         std::floor(value / 1024.0) >= INT32_MIN &&
                                                         std::floor(value / 1024.0) <= INT32_MAX;
                                              });
        if (!valid_values || !camera.desired.session)
            return lux::cxx::unexpected(RendererFailure{ERendererError::INVALID_ARGUMENT, {}, id()});
        state.camera = camera;
        state.camera_valid = true;
        return {};
    }
    RenderResult<ViewImage> RenderView::acquireImage() noexcept
    {
        auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        if (state.status.state == EViewState::FAILED && state.status.failure)
            return lux::cxx::unexpected(*state.status.failure);
        if (state.status.state != EViewState::READY || !state.camera_valid)
            return lux::cxx::unexpected(RendererFailure{ERendererError::NOT_READY, {}, id()});
        try
        {
            auto record = std::make_shared<ViewImageLease::Record>();
            record->version = state.version;
            record->camera = state.camera;
            record->content = {state.camera.desired, 0, EImageEvidence::REQUESTED};
            record->content.source.surface_generation = state.version->generation;
            record->wire_camera.scene_id = state.scene;
            record->wire_camera.view = state.view;
            for (std::size_t index = 0; index < 16; ++index)
            {
                record->wire_camera.view_matrix[index] = static_cast<float>(state.camera.view[index]);
                record->wire_camera.proj_matrix[index] = static_cast<float>(state.camera.projection[index]);
            }
            for (std::size_t index = 0; index < 3; ++index)
            {
                const auto page = std::floor(state.camera.origin[index] / 1024.0);
                record->wire_camera.render_origin.page_delta[index] = static_cast<std::int32_t>(page);
                record->wire_camera.render_origin.local[index] =
                    static_cast<float>(state.camera.origin[index] - page * 1024.0);
            }
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
