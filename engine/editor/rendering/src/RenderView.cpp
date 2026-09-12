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
    namespace
    {
        thread_local std::optional<lux::render::RenderSceneId> next_test_scene;
    }
    void detail::RendererTestAccess::useSceneForNextView(lux::render::RenderSceneId scene) noexcept
    {
        next_test_scene = scene;
    }
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
#if defined(LUX_EDITOR_RENDERER_TEST_DIAGNOSTICS)
        if (const auto replacement = std::exchange(next_test_scene, std::nullopt))
            scene = *replacement;
#endif
        {
            auto impl = std::make_unique<Impl>();
            // Allocate the owner before the stateful resource record; no request is sent from this factory.
            auto result = std::unique_ptr<RenderView>(new RenderView(std::move(impl)));
            result->impl_->resources = std::make_unique<detail::ViewResources>(renderer, control, id, scene, config);
            return result;
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
    RenderResult<RenderViewCloseStatus> RenderView::closeStatus() const noexcept
    {
        const auto &state = *impl_->resources;
        if (auto checked = state.check(); !checked)
            return lux::cxx::unexpected(checked.error());
        RenderViewCloseStatus result;
        result.status = state.status;
        result.close_requested = state.close_requested;
        result.layer_attached = state.linked;
        result.target_owned = state.target.isValid();
        result.view_owned = state.view.isValid();
        result.image_version_owners = state.version.use_count();
        if (state.version)
            result.last_submission = state.version->last_submission.load(std::memory_order_acquire);
        result.gpu_completed = state.renderer.statistics().gpu_completed;
        result.create_view = state.create_view.requestId();
        result.create_target = state.create_target.requestId();
        result.resize = state.resize.requestId();
        result.release_view = state.release_view.requestId();
        result.release_target = state.release_target.requestId();
        return result;
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
        if (state.status.state == EViewState::FAILED && state.status.failure)
            return lux::cxx::unexpected(*state.status.failure);
        const auto finite = [](double value) { return std::isfinite(value) && std::abs(value) <= FLT_MAX; };
        const bool valid_values = std::all_of(camera.view.begin(), camera.view.end(), finite) &&
                                  std::all_of(camera.projection.begin(), camera.projection.end(), finite) &&
                                  std::all_of(camera.origin.begin(), camera.origin.end(),
                                              [&state](double value)
                                              {
                                                  return std::isfinite(value) &&
                                                         std::floor(value / state.coordinate_page_size) >= INT32_MIN &&
                                                         std::floor(value / state.coordinate_page_size) <= INT32_MAX;
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
        {
            auto record = std::make_shared<ViewImageLease::Record>();
            record->version = state.version;
            record->camera = state.camera;
            record->content = {state.camera.desired, 0, EImageEvidence::REQUESTED};
            record->content.source.surface_generation = state.version->generation;
            record->wire_camera.scene_id = state.scene;
            record->wire_camera.view = state.view;
            record->wire_camera.coordinate_page_size = static_cast<float>(state.coordinate_page_size);
            for (std::size_t index = 0; index < 16; ++index)
            {
                record->wire_camera.view_matrix[index] = static_cast<float>(state.camera.view[index]);
                record->wire_camera.proj_matrix[index] = static_cast<float>(state.camera.projection[index]);
            }
            for (std::size_t index = 0; index < 3; ++index)
            {
                const auto page = std::floor(state.camera.origin[index] / state.coordinate_page_size);
                record->wire_camera.render_origin.page_delta[index] = static_cast<std::int32_t>(page);
                record->wire_camera.render_origin.local[index] =
                    static_cast<float>(state.camera.origin[index] - page * state.coordinate_page_size);
            }
            ViewImage image{state.version->texture, state.version->extent, id(), record->content, {}};
            image.lease.record_ = std::move(record);
            return image;
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
