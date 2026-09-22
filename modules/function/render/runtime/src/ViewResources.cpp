#include <limits>
#include <lux/engine/render/detail/ViewResources.hpp>

namespace lux::render::detail
{
namespace
{
auto fail(ERendererError code, RenderViewId view) noexcept
{
    return lux::cxx::unexpected(RendererFailure{code, {}, view});
}
} // namespace
ViewResources::ViewResources(RenderRuntime &backend, lux::render::RenderControlSession &channel, RenderViewId identity,
                             RenderSceneLease source, ViewConfig config)
    : renderer(backend), control(channel), scene(source.id()), scene_use(std::move(source)),
      version(std::make_shared<ImageVersion>())
{
    output = config.output;
    status = {EViewState::CREATING, identity, config.extent, {}, 1, 0};
}
ViewResources::~ViewResources() noexcept = default;
RenderResult<void> ViewResources::check() const noexcept
{
    if (owner != std::this_thread::get_id())
    {
        return fail(ERendererError::WRONG_THREAD, status.view);
    }
    return {};
}
bool ViewResources::drained() const noexcept
{
    if (!version)
    {
        return true;
    }
    return version.use_count() == 1 &&
           renderer.statistics().gpu_completed >= version->last_submission.load(std::memory_order_acquire);
}
RenderResult<void> ViewResources::requestExtent(PixelExtent extent) noexcept
{
    if (auto checked = check(); !checked)
    {
        return checked;
    }
    if (close_requested || status.state == EViewState::CLOSED)
    {
        return fail(ERendererError::STOPPING, status.view);
    }
    if (status.state == EViewState::FAILED && status.failure)
    {
        return lux::cxx::unexpected(*status.failure);
    }
    if (extent.width > 16384 || extent.height > 16384)
    {
        return fail(ERendererError::INVALID_ARGUMENT, status.view);
    }
    if (extent == status.requested_extent)
    {
        return {};
    }
    if (status.request_sequence == (std::numeric_limits<std::uint64_t>::max)())
    {
        return fail(ERendererError::CAPACITY, status.view);
    }
    status.requested_extent = extent;
    ++status.request_sequence;
    status.state = extent.width && extent.height ? EViewState::RESIZING : EViewState::SUSPENDED;
    return {};
}
RenderResult<void> ViewResources::beginClose() noexcept
{
    if (auto checked = check(); !checked)
    {
        return checked;
    }
    if (status.state == EViewState::CLOSED)
    {
        return {};
    }
    close_requested = true;
    status.state = EViewState::CLOSING;
    return {};
}
void ViewResources::rememberFailure(lux::render::RenderError error, std::uint64_t request,
                                    std::optional<std::uint32_t> backend_status) noexcept
{
    RendererFailure failure{ERendererError::DEVICE_FAILURE, error, status.view, request, backend_status};
    if (error.ok() && !backend_status)
    {
        failure.code = ERendererError::CONTRACT_FAILURE;
    }
    if (!status.failure)
    {
        status.failure = failure;
    }
    pending_failure = failure;
    if (!close_requested)
    {
        status.state = EViewState::FAILED;
    }
}
void ViewResources::acceptReplies() noexcept
{
    if (create_view.valid() && create_view.isReady())
    {
        const auto result = create_view.tryResult();
        if (result && result->get().error.ok() && result->get().view.isValid())
        {
            view = result->get().view;
        }
        else
        {
            rememberFailure(result ? result->get().error : result.error(), create_view.requestId());
        }
        create_view = {};
    }
    if (create_target.valid() && create_target.isReady())
    {
        const auto result = create_target.tryResult();
        if (result && result->get().status == 0 && result->get().target.isValid())
        {
            target = result->get().target;
        }
        else
        {
            rememberFailure(result ? lux::render::RenderError{} : result.error(), create_target.requestId(),
                            result ? std::optional{result->get().status} : std::nullopt);
        }
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
        {
            rememberFailure(result ? lux::render::RenderError{} : result.error(), resize.requestId(),
                            result ? std::optional{result->get().status} : std::nullopt);
        }
        resize = {};
    }
    if (release_view.valid() && release_view.isReady())
    {
        const auto result = release_view.tryResult();
        if (result && result->get().error.ok())
        {
            view = {};
        }
        else
        {
            rememberFailure(result ? result->get().error : result.error(), release_view.requestId());
        }
        release_view = {};
    }
    if (release_target.valid() && release_target.isReady())
    {
        const auto result = release_target.tryResult();
        // The backend documents status 1 as already absent: release is idempotent.
        if (result && result->get().status <= 1 && result->get().target == target)
        {
            target = {};
        }
        else
        {
            rememberFailure(result ? lux::render::RenderError{} : result.error(), release_target.requestId(),
                            result ? std::optional{result->get().status} : std::nullopt);
        }
        release_target = {};
    }
}

RenderResult<void> ViewResources::prepareControlStep(std::size_t &budget)
{
    if (auto checked = check(); !checked)
    {
        return checked;
    }
    if (status.state == EViewState::CLOSED)
    {
        return {};
    }
    acceptReplies();
    if (close_requested)
    {
        status.state = EViewState::CLOSING;
    }
    const auto backend = renderer.status();
    if (backend.state != ERenderRuntimeState::ACTIVE && !status.failure)
    {
        rememberFailure(backend.error.ok() ? renderError<err::comm::ChannelStopping>() : backend.error, 0);
    }
    // A stopped backend has already performed its terminal GPU cleanup. CPU references still gate closure.
    if (backend.state == ERenderRuntimeState::RETIRED)
    {
        if (version.use_count() != 1)
        {
            return {};
        }
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
            scene_use = {};
            status.state = EViewState::CLOSED;
        }
        return {};
    }
    if (!budget || !renderer.controlAvailable())
    {
        return {};
    }
    {
        if (close_requested)
        {
            if (create_view.valid() || create_target.valid() || resize.valid() || !drained())
            {
                return {};
            }
            if (linked)
            {
                control.removeLayer(target, 0);
                --budget;
                linked = false;
                return {};
            }
            if (target.isValid() && !release_target.valid())
            {
                release_target = control.destroyRenderTarget(target);
                --budget;
                return {};
            }
            if (view.isValid() && !release_view.valid())
            {
                release_view = control.removeView(scene, view);
                --budget;
                return {};
            }
            if (!target.isValid() && !view.isValid())
            {
                version.reset();
                scene_use = {};
                status.state = EViewState::CLOSED;
            }
            return {};
        }
        if (status.state == EViewState::FAILED)
        {
            return {};
        }
        const auto desired = status.requested_extent;
        if (!desired.width || !desired.height)
        {
            if (linked && drained())
            {
                control.removeLayer(target, 0);
                --budget;
                linked = false;
            }
            status.state = EViewState::SUSPENDED;
            return {};
        }
        if (!view_requested)
        {
            auto requested_scene = scene;
            create_view = control.addView(requested_scene, {desired.width, desired.height}, "Scene view");
            --budget;
            view_requested = true;
            return {};
        }
        if (!target_requested)
        {
            create_target = std::visit(
                [&](const auto &destination) {
                    if constexpr (std::same_as<std::decay_t<decltype(destination)>, NativeSurfaceOutput>)
                    {
                        return control.createSurfaceRenderTarget(destination.native_window,
                                                                 {desired.width, desired.height});
                    }
                    else
                    {
                        return control.createOffscreenRenderTarget({desired.width, desired.height}, kTargetFlagSampled);
                    }
                },
                output);
            --budget;
            in_flight_extent = desired;
            in_flight_sequence = status.request_sequence;
            target_requested = true;
            return {};
        }
        if (!view.isValid() || !target.isValid())
        {
            return {};
        }
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
            version->texture = std::uint64_t{status.view.generation};
        }
        if (resize.valid())
        {
            return {};
        }
        if (status.ready_extent != desired)
        {
            status.state = EViewState::RESIZING;
            if (!drained())
            {
                return {};
            }
            if (linked)
            {
                control.removeLayer(target, 0);
                --budget;
                linked = false;
                return {};
            }
            resize = control.requestResizeTarget(target, {desired.width, desired.height});
            --budget;
            in_flight_sequence = status.request_sequence;
            return {};
        }
        if (!linked)
        {
            control.setLayer(target, 0, scene, view);
            --budget;
            linked = true;
        }
        status.acknowledged_sequence = status.request_sequence;
        status.state = EViewState::READY;
        return {};
    }
}
} // namespace lux::render::detail
