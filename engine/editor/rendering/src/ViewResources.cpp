#include <lux/engine/editor/rendering/detail/ViewResources.hpp>
#include <limits>
#include <new>

namespace lux::editor::rendering::detail
{
    namespace
    {
        auto fail(ERendererError code, RenderViewId view) noexcept
        {
            return lux::cxx::unexpected(RendererFailure{code, {}, view});
        }
    } // namespace
    ViewResources::ViewResources(EditorRenderer &backend, lux::render::RenderControlSession &channel,
                                 RenderViewId identity, lux::render::RenderSceneId source, ViewConfig config)
        : renderer(backend), control(channel), scene(source), version(std::make_shared<ImageVersion>())
    {
        status = {EViewState::CREATING, identity, config.extent, {}, 1, 0};
    }
    ViewResources::~ViewResources() noexcept
    {
        if (status.state != EViewState::CLOSED)
            std::terminate();
    }
    RenderResult<void> ViewResources::check() const noexcept
    {
        if (owner != std::this_thread::get_id())
            return fail(ERendererError::WRONG_THREAD, status.view);
        return {};
    }
    bool ViewResources::drained() const noexcept
    {
        if (!version)
            return true;
        return version.use_count() == 1 &&
               renderer.statistics().gpu_completed >= version->last_submission.load(std::memory_order_acquire);
    }
    RenderResult<void> ViewResources::requestExtent(PixelExtent extent) noexcept
    {
        if (auto checked = check(); !checked)
            return checked;
        if (close_requested || status.state == EViewState::CLOSED)
            return fail(ERendererError::STOPPING, status.view);
        if (extent.width > 16384 || extent.height > 16384)
            return fail(ERendererError::INVALID_ARGUMENT, status.view);
        if (extent == status.requested_extent)
            return {};
        if (status.request_sequence == (std::numeric_limits<std::uint64_t>::max)())
            return fail(ERendererError::CAPACITY, status.view);
        status.requested_extent = extent;
        ++status.request_sequence;
        status.state = extent.width && extent.height ? EViewState::RESIZING : EViewState::SUSPENDED;
        return {};
    }
    RenderResult<void> ViewResources::beginClose() noexcept
    {
        if (auto checked = check(); !checked)
            return checked;
        if (status.state == EViewState::CLOSED)
            return {};
        close_requested = true;
        status.state = EViewState::CLOSING;
        return {};
    }
    void ViewResources::rememberFailure(lux::render::RenderError error, std::uint64_t request,
                                        std::optional<std::uint32_t> backend_status) noexcept
    {
        RendererFailure failure{ERendererError::DEVICE_FAILURE, error, status.view, request, backend_status};
        if (error.ok() && !backend_status)
            failure.code = ERendererError::CONTRACT_FAILURE;
        if (!status.failure)
            status.failure = failure;
        pending_failure = failure;
        if (!close_requested)
            status.state = EViewState::FAILED;
    }
    void ViewResources::acceptReplies() noexcept
    {
        if (create_view.valid() && create_view.isReady())
        {
            const auto result = create_view.tryResult();
            if (result && result->get().error.ok() && result->get().view.isValid())
                view = result->get().view;
            else
                rememberFailure(result ? result->get().error : result.error(), create_view.requestId());
            create_view = {};
        }
        if (create_target.valid() && create_target.isReady())
        {
            const auto result = create_target.tryResult();
            if (result && result->get().status == 0 && result->get().target.isValid())
                target = result->get().target;
            else
                rememberFailure(result ? lux::render::RenderError{} : result.error(), create_target.requestId(),
                                result ? std::optional{result->get().status} : std::nullopt);
            create_target = {};
        }
        if (resize.valid() && resize.isReady())
        {
            const auto result = resize.tryResult();
            if (result && result->get().status == 0 && result->get().target == target)
            {
                status.ready_extent = {result->get().extent.width, result->get().extent.height};
                status.acknowledged_sequence = in_flight_sequence;
                version->extent = status.ready_extent;
                version->generation = in_flight_sequence;
                version->backing_revision.store(result->get().backing_revision, std::memory_order_release);
                version->last_recording = 0;
                version->last_submission = 0;
            }
            else
                rememberFailure(result ? lux::render::RenderError{} : result.error(), resize.requestId(),
                                result ? std::optional{result->get().status} : std::nullopt);
            resize = {};
        }
        if (release_view.valid() && release_view.isReady())
        {
            const auto result = release_view.tryResult();
            if (result && result->get().error.ok())
                view = {};
            else
                rememberFailure(result ? result->get().error : result.error(), release_view.requestId());
            release_view = {};
        }
        if (release_target.valid() && release_target.isReady())
        {
            const auto result = release_target.tryResult();
            // The backend documents status 1 as already absent: release is idempotent.
            if (result && result->get().status <= 1 && result->get().target == target)
                target = {};
            else
                rememberFailure(result ? lux::render::RenderError{} : result.error(), release_target.requestId(),
                                result ? std::optional{result->get().status} : std::nullopt);
            release_target = {};
        }
    }

    RenderResult<void> ViewResources::prepareControlStep() noexcept
    {
        if (auto checked = check(); !checked)
            return checked;
        if (status.state == EViewState::CLOSED)
            return {};
        acceptReplies();
        // A stopped backend has already performed its terminal GPU cleanup. CPU references still gate closure.
        if (renderer.state() == ERendererState::STOPPED || renderer.state() == ERendererState::FAILED)
        {
            if (version.use_count() != 1)
                return {};
            if (close_requested)
            {
                create_view = {};
                create_target = {};
                resize = {};
                release_view = {};
                release_target = {};
                target = {};
                view = {};
                version.reset();
                status.state = EViewState::CLOSED;
            }
            return {};
        }
        if (!renderer.controlAvailable())
            return {};
        try
        {
            if (close_requested)
            {
                if (create_view.valid() || create_target.valid() || resize.valid() || !drained())
                    return {};
                if (linked)
                {
                    control.removeLayer(target, 0);
                    linked = false;
                    return {};
                }
                if (target.isValid() && !release_target.valid())
                {
                    release_target = control.destroyRenderTarget(target);
                    return {};
                }
                if (view.isValid() && !release_view.valid())
                {
                    release_view = control.removeView(scene, view);
                    return {};
                }
                if (!target.isValid() && !view.isValid())
                {
                    version.reset();
                    status.state = EViewState::CLOSED;
                }
                return {};
            }
            if (status.state == EViewState::FAILED)
                return {};
            const auto desired = status.requested_extent;
            if (!desired.width || !desired.height)
            {
                if (linked && drained())
                {
                    control.removeLayer(target, 0);
                    linked = false;
                }
                status.state = EViewState::SUSPENDED;
                return {};
            }
            if (!view_requested)
            {
                create_view = control.addView(scene, {desired.width, desired.height}, "Editor view");
                view_requested = true;
                return {};
            }
            if (!target_requested)
            {
                create_target = control.createOffscreenRenderTarget({desired.width, desired.height},
                                                                    lux::render::kTargetFlagSampled);
                in_flight_extent = desired;
                in_flight_sequence = status.request_sequence;
                target_requested = true;
                return {};
            }
            if (!view.isValid() || !target.isValid())
                return {};
            if (!status.acknowledged_sequence)
            {
                status.ready_extent = in_flight_extent;
                status.acknowledged_sequence = in_flight_sequence;
                version->id = status.view;
                version->scene = scene;
                version->view = view;
                version->target = target;
                version->extent = status.ready_extent;
                version->generation = in_flight_sequence;
                // The renderer incarnation and monotonically issued view generation define a unique token.
                version->texture = lux::ui::TextureHandle{status.view.generation};
            }
            if (resize.valid())
                return {};
            if (status.ready_extent != desired)
            {
                status.state = EViewState::RESIZING;
                if (!drained())
                    return {};
                if (linked)
                {
                    control.removeLayer(target, 0);
                    linked = false;
                    return {};
                }
                resize = control.requestResizeTarget(target, {desired.width, desired.height});
                in_flight_sequence = status.request_sequence;
                return {};
            }
            if (!linked)
            {
                control.setLayer(target, 0, scene, view);
                linked = true;
            }
            status.acknowledged_sequence = status.request_sequence;
            status.state = EViewState::READY;
            return {};
        }
        catch (const std::bad_alloc &)
        {
            return fail(ERendererError::ALLOCATION_FAILURE, status.view);
        }
    }
} // namespace lux::editor::rendering::detail
