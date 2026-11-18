#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/target/SwapchainTarget.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <utility>

namespace lux::render::detail
{
    SwapchainTarget::SwapchainTarget(
        DeviceSession& session, TargetDesc description, SurfaceDescription surface
    ) noexcept
        : session_(session), description_(description), surface_(surface), uses_(session.makeFrontiers())
    {
    }

    RenderResult<SwapchainTarget> SwapchainTarget::create(
        DeviceSession& session,
        TargetDesc description,
        SurfaceDescription surface,
        std::uint32_t frame_count
    ) noexcept
    {
        if (auto valid = description.validate(); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        const bool invalid_kind = description.kind != ETargetKind::SURFACE;
        const auto maximum = session.description().dimensions.attachmentMaxDim;
        const bool invalid_extent = description.width > maximum || description.height > maximum;
        nri::Window window{};
        const bool valid_window = std::visit(
            [&](const auto& value) noexcept
            {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, Win32Surface>)
                {
                    window.windows.hwnd = value.window;
                    return value.window != nullptr;
                }
                else if constexpr (std::same_as<T, X11Surface>)
                {
                    window.x11.dpy = value.display;
                    window.x11.window = value.window;
                    return value.display && value.window;
                }
                else if constexpr (std::same_as<T, WaylandSurface>)
                {
                    window.wayland.display = value.display;
                    window.wayland.surface = value.surface;
                    return value.display && value.surface;
                }
                else if constexpr (std::same_as<T, MetalSurface>)
                {
                    window.metal.caMetalLayer = value.layer;
                    return value.layer != nullptr;
                }
                else
                {
                    return false;
                }
            },
            surface.window
        );
        const bool invalid_window = !valid_window;
        if (invalid_kind || invalid_window || invalid_extent)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!session.supportsPresentRetirement())
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        SwapchainTarget output(session, description, surface);
        auto status = nri::nriGetInterface(
            session.device(),
            NRI_INTERFACE(nri::SwapChainInterface),
            &output.swap_chain_operations_
        );
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        nri::SwapChainDesc swap_chain{};
        swap_chain.window = window;
        swap_chain.queue = &session.queue().native();
        swap_chain.width = static_cast<nri::Dim_t>(description.width);
        swap_chain.height = static_cast<nri::Dim_t>(description.height);
        swap_chain.textureNum = static_cast<std::uint8_t>(frame_count + 1);
        swap_chain.queuedFrameNum = static_cast<std::uint8_t>(frame_count);
        swap_chain.format = nri::SwapChainFormat::BT709_G22_8BIT;
        swap_chain.flags = surface.vertical_sync ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::NONE;
        status = output.swap_chain_operations_.CreateSwapChain(session.device(), swap_chain, output.swap_chain_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        std::uint32_t image_count = 0;
        const auto images = output.swap_chain_operations_.GetSwapChainTextures(*output.swap_chain_, image_count);
        if (!images || image_count == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        output.images_.assign(images, images + image_count);
        const auto& actual = session.nriCore().GetTextureDesc(*images[0]);
        const auto format = actual.format;
        auto& target = output.description_;
        switch (format)
        {
        case nri::Format::RGBA8_UNORM:
            target.format = rdesc::ETextureFormat::RGBA8_UNORM;
            break;
        case nri::Format::RGBA8_SRGB:
            target.format = rdesc::ETextureFormat::RGBA8_SRGB;
            break;
        case nri::Format::BGRA8_UNORM:
            target.format = rdesc::ETextureFormat::BGRA8_UNORM;
            break;
        case nri::Format::BGRA8_SRGB:
            target.format = rdesc::ETextureFormat::BGRA8_SRGB;
            break;
        default:
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        target.width = actual.width;
        target.height = actual.height;
        const auto& core = session.nriCore();
        for (auto* image : output.images_)
        {
            nri::Descriptor* view = nullptr;
            const nri::TextureViewDesc view_description{image, nri::TextureView::COLOR_ATTACHMENT, format, 0, 1, 0, 1};
            status = core.CreateTextureView(view_description, view);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output.views_.push_back(view);
            nri::Fence* released = nullptr;
            status = core.CreateFence(session.device(), nri::FenceDesc{nri::SWAPCHAIN_SEMAPHORE}, released);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output.release_semaphores_.push_back(released);
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            nri::Fence* acquired = nullptr;
            status = core.CreateFence(session.device(), nri::FenceDesc{nri::SWAPCHAIN_SEMAPHORE}, acquired);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output.acquire_semaphores_.push_back({acquired});
            // Cold descriptor construction needs a real compatible image; acquire selects the
            // actual presentation image before this slot records any work.
            output.frames_.push_back(
                {images[slot % image_count], output.views_[slot % image_count], &output.presentation_,
                 UINT32_MAX, &output.uses_}
            );
        }
        return output;
    }

    SwapchainTarget::SwapchainTarget(SwapchainTarget&& other) noexcept
        : state(other.state), session_(other.session_), description_(other.description_), surface_(other.surface_),
          swap_chain_operations_(other.swap_chain_operations_), swap_chain_(std::exchange(other.swap_chain_, nullptr)),
          images_(std::move(other.images_)), views_(std::move(other.views_)),
          acquire_semaphores_(std::move(other.acquire_semaphores_)),
          release_semaphores_(std::move(other.release_semaphores_)), frames_(std::move(other.frames_)),
          presentation_(other.presentation_), acquire_slot_(other.acquire_slot_),
          acquired_image_(other.acquired_image_),
          next_present_id_(other.next_present_id_), failure_(other.failure_), uses_(std::move(other.uses_))
    {
        if (other.nativePending() || other.idle_) std::terminate();
        for (auto& frame : frames_)
        {
            frame.presentation = &presentation_;
            frame.accepted_uses = &uses_;
        }
    }

    SwapchainTarget::~SwapchainTarget() noexcept
    {
        if (!swap_chain_) return;
        session_.deferRelease(uses_, [retirement = takeRetirement()]() mutable noexcept
        {
            (void)retirement.release(retirement.session.deviceLost());
        });
    }

    RenderResult<bool> SwapchainTarget::prefetched() const noexcept
    {
        if (failure_.type) return cxx::unexpected(failure_);
        if (presentation_.phase == EPresentationPhase::WAIT_ACCEPTED)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        return presentation_.phase == EPresentationPhase::READY;
    }

    RenderResult<bool> SwapchainTarget::bindImage(std::uint32_t frame_slot) noexcept
    {
        auto ready = prefetched();
        if (!ready || !*ready) return ready;
        frames_[frame_slot] = {images_[acquired_image_], views_[acquired_image_], &presentation_, UINT32_MAX, &uses_};
        return true;
    }

    SwapchainTarget::ENativeStep SwapchainTarget::beginAdvance(bool acquire) noexcept
    {
        if (!available() || state.resizing) return ENativeStep::NONE;
        if (presentation_.phase == EPresentationPhase::SUBMITTED)
        {
            presentation_.phase = EPresentationPhase::PRESENTING;
            return ENativeStep::PRESENT;
        }
        if (!acquire || presentation_.phase != EPresentationPhase::IDLE) return ENativeStep::NONE;
        for (std::uint32_t slot = 0; slot < acquire_semaphores_.size(); ++slot)
        {
            const auto& point = acquire_semaphores_[slot].last_wait;
            if (point.serial && session_.physicalQueue(point.queue).completed() < point.serial) continue;
            acquire_slot_ = slot;
            presentation_.phase = EPresentationPhase::ACQUIRING;
            return ENativeStep::ACQUIRE;
        }
        return ENativeStep::NONE;
    }

    exec::task<RenderResult<bool>> SwapchainTarget::advance(ENativeStep step) noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        const bool acquire = step == ENativeStep::ACQUIRE;
        auto& fence = acquire_semaphores_[acquire_slot_];
        struct NativeResult final
        {
            nri::Result status;
            std::uint32_t image = UINT32_MAX;
        };
        auto returned = co_await (
            stdexec::schedule(session_.nativeScheduler()) |
            stdexec::then(
                [ops = swap_chain_operations_, swap = swap_chain_, acquire,
                 semaphore = acquire ? fence.native : presentation_.rendered, present_id = next_present_id_]
                () noexcept -> RenderResult<NativeResult>
                {
                    if (!acquire) return NativeResult{ops.QueuePresent(*swap, *semaphore, present_id)};
                    std::uint32_t image = UINT32_MAX;
                    const auto status = ops.AcquireNextTexture(*swap, *semaphore, image);
                    return NativeResult{status, image};
                }
            ) |
            stdexec::upon_error(NativeScheduleFailure<NativeResult>{}) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<NativeResult>
            { return cxx::unexpected(RenderError{kNativeExecutionClosed}); }) |
            stdexec::continues_on(owner) |
            stdexec::upon_error(NativeDeliveryFailure<NativeResult>{})
        );
        cxx::scope_exit notify([this]() noexcept
        {
            if (auto done = std::exchange(idle_, nullptr))
            {
                done(std::exchange(idle_context_, nullptr));
            }
        });
        if (!returned)
        {
            failure_ = returned.error();
        }
        else if (returned->status == nri::Result::NOT_READY)
        {
            presentation_.phase = acquire ? EPresentationPhase::IDLE : EPresentationPhase::SUBMITTED;
            co_return false;
        }
        else if (returned->status != nri::Result::SUCCESS)
        {
            failure_ = nativeError(returned->status);
        }
        else if (acquire)
        {
            if (returned->image >= images_.size()) std::terminate();
            acquired_image_ = returned->image;
            fence.last_wait = {};
            presentation_ = {
                EPresentationPhase::READY, fence.native, release_semaphores_[acquired_image_], &fence.last_wait
            };
            co_return false;
        }
        else
        {
            ++next_present_id_;
            acquired_image_ = UINT32_MAX;
            presentation_ = {};
            co_return true;
        }
        presentation_.phase = EPresentationPhase::FAILED;
        co_return cxx::unexpected(failure_);
    }

    exec::task<RenderResult<void>> SwapchainTarget::close() noexcept
    {
        const auto owner = co_await stdexec::read_env(stdexec::get_scheduler);
        constexpr auto settle = stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}};
        if (nativePending())
        {
            // Resume after advance() and its Server-side completion have finished using the old flight.
            co_await (whenIdle() | stdexec::continues_on(owner) | stdexec::write_env(settle));
        }
        if (!swap_chain_) co_return RenderResult<void>{};
        const auto completed = co_await (session_.waitFrontiers(uses_) | stdexec::write_env(settle));
        if (!completed)
        {
            const auto error = completed.error();
            const bool safe = error.type == kOutcomeUnknown || isNativeFailure(error, nri::Result::DEVICE_LOST);
            if (!safe) std::terminate();
        }
        const bool lost = session_.deviceLost();
        auto retirement = takeRetirement();
        // G has completed. P/A may still wait in the driver and must only hold this device's native worker.
        co_return co_await (
            stdexec::schedule(session_.nativeScheduler()) |
            stdexec::then([&retirement, lost]() noexcept { return retirement.release(lost); }) |
            stdexec::upon_error([](auto&&) noexcept -> RenderResult<void> { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<void> { std::terminate(); }) |
            stdexec::continues_on(owner) |
            stdexec::upon_error(NativeDeliveryFailure<void>{}) |
            stdexec::write_env(settle)
        );
    }

    SwapchainTarget::Retirement SwapchainTarget::takeRetirement() noexcept
    {
        if (nativePending() || idle_) std::terminate();
        auto* unused = acquired_image_ != UINT32_MAX && !acquire_semaphores_[acquire_slot_].last_wait.serial
            ? presentation_.acquired : nullptr;
        return {
            session_, swap_chain_operations_, std::exchange(swap_chain_, nullptr), unused,
            std::move(views_), std::move(acquire_semaphores_), std::move(release_semaphores_)
        };
    }

    RenderResult<void> SwapchainTarget::Retirement::release(bool lost) noexcept
    {
        const auto status = lost ? nri::Result::DEVICE_LOST
                                 : session.retireSwapChain(*swap_chain, unused_acquire, 1'000'000'000);
        if (status != nri::Result::SUCCESS && status != nri::Result::DEVICE_LOST)
        {
            std::terminate(); // Do not destroy handles without the retirement proof.
        }
        const auto& core = session.nriCore();
        for (auto* view : views) core.DestroyDescriptor(view);
        for (const auto& fence : acquired) core.DestroyFence(fence.native);
        for (auto* fence : released) core.DestroyFence(fence);
        operations.DestroySwapChain(swap_chain);
        return status == nri::Result::SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(status));
    }
} // namespace lux::render::detail
