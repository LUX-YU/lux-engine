#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/SurfaceTarget.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

#include <algorithm>

namespace lux::render::detail
{
    SurfaceTarget::SurfaceTarget(DeviceSession& session, TargetDesc description, SurfaceDescription surface) noexcept
        : session_(session), description_(description), surface_(surface)
    {
    }

    RenderResult<std::unique_ptr<SurfaceTarget>> SurfaceTarget::create(
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
        auto output = std::unique_ptr<SurfaceTarget>(new SurfaceTarget(session, description, surface));
        auto status = nri::nriGetInterface(
            session.device(),
            NRI_INTERFACE(nri::SwapChainInterface),
            &output->swap_chain_operations_
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
        status = output->swap_chain_operations_.CreateSwapChain(session.device(), swap_chain, output->swap_chain_);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        std::uint32_t image_count = 0;
        const auto images = output->swap_chain_operations_.GetSwapChainTextures(*output->swap_chain_, image_count);
        output->images_.assign(images, images + image_count);
        const auto& actual = session.nriCore().GetTextureDesc(*images[0]);
        const auto format = actual.format;
        auto& target = output->description_;
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
        for (auto* image : output->images_)
        {
            nri::Descriptor* view = nullptr;
            const nri::TextureViewDesc view_description{image, nri::TextureView::COLOR_ATTACHMENT, format, 0, 1, 0, 1};
            status = core.CreateTextureView(view_description, view);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output->views_.push_back(view);
            nri::Fence* released = nullptr;
            status = core.CreateFence(session.device(), nri::FenceDesc{nri::SWAPCHAIN_SEMAPHORE}, released);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output->release_semaphores_.push_back(released);
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            nri::Fence* acquired = nullptr;
            status = core.CreateFence(session.device(), nri::FenceDesc{nri::SWAPCHAIN_SEMAPHORE}, acquired);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output->acquire_semaphores_.push_back(acquired);
            // Cold descriptor construction needs a real compatible image; acquire selects the
            // actual presentation image before this slot records any work.
            output->frames_.push_back(
                {images[slot % image_count], output->views_[slot % image_count], &output->presentation_}
            );
        }
        return output;
    }

    SurfaceTarget::~SurfaceTarget() noexcept
    {
        retire();
    }

    RenderResult<bool> SurfaceTarget::acquire(std::uint32_t frame_slot) noexcept
    {
        if (failure_.type != 0)
        {
            return cxx::unexpected(failure_);
        }
        if (presentation_.render_signal_submitted)
        {
            return false; // Keep the accepted rendered signal until Present can consume it.
        }
        if (acquired_image_ == UINT32_MAX)
        {
            const auto status = swap_chain_operations_.AcquireNextTexture(
                *swap_chain_,
                *acquire_semaphores_[frame_slot],
                acquired_image_
            );
            if (status == nri::Result::NOT_READY)
            {
                return false;
            }
            if (status != nri::Result::SUCCESS)
            {
                acquired_image_ = UINT32_MAX;
                failure_ = nativeError(status);
                return cxx::unexpected(failure_);
            }
            presentation_ = {acquire_semaphores_[frame_slot], release_semaphores_[acquired_image_]};
        }
        else
        {
            if (presentation_.acquire_wait_submitted)
            {
                // Partial submission is retired, never replayed against an already consumed binary wait.
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            // A failed preparation can advance FIF without consuming this acquire signal. Move its
            // ownership to the reusable slot that will submit the wait; that slot's frontier then
            // protects reuse. Release semaphores remain indexed by the acquired swap-chain image.
            const auto previous = std::ranges::find(acquire_semaphores_, presentation_.acquired);
            std::iter_swap(previous, acquire_semaphores_.begin() + frame_slot);
        }
        frames_[frame_slot] = {images_[acquired_image_], views_[acquired_image_], &presentation_};
        return true;
    }

    RenderResult<bool> SurfaceTarget::present() noexcept
    {
        if (failure_.type != 0)
        {
            return cxx::unexpected(failure_);
        }
        if (!presentation_.render_signal_submitted)
        {
            return false;
        }
        const auto status = swap_chain_operations_.QueuePresent(*swap_chain_, *presentation_.rendered, next_present_id_);
        if (status == nri::Result::NOT_READY)
        {
            return false; // The native call has not enqueued Present; preserve both semaphore identities.
        }
        if (status != nri::Result::SUCCESS)
        {
            failure_ = nativeError(status);
            return cxx::unexpected(failure_);
        }
        ++next_present_id_;
        acquired_image_ = UINT32_MAX;
        presentation_ = {};
        return true;
    }

    void
    SurfaceTarget::retire(const SubmissionFrontiers* frontiers, void* completion, Completion complete) noexcept
    {
        auto* swap_chain = std::exchange(swap_chain_, nullptr);
        if (!swap_chain)
        {
            if (complete)
            {
                complete(completion, {});
            }
            return;
        }
        auto release =
            [
                core = &session_.nriCore(),
                operations = swap_chain_operations_,
                swap_chain,
                session = &session_,
                views = std::move(views_),
                acquired = std::move(acquire_semaphores_),
                released = std::move(release_semaphores_),
                unused_acquire = acquired_image_ != UINT32_MAX && !presentation_.acquire_wait_submitted
                    ? presentation_.acquired : nullptr,
                completion,
                complete
            ]() noexcept
            {
                // GPU frontiers prove G; native maintenance fences and image release prove P/A.
                // One bounded cold operation retains every owner until all three facts hold.
                const auto status = session->deviceLost() ? nri::Result::DEVICE_LOST
                    : session->retireSwapChain(*swap_chain, unused_acquire, 1'000'000'000);
                if (status != nri::Result::SUCCESS && status != nri::Result::DEVICE_LOST)
                {
                    std::terminate(); // Do not release captured handles without the retirement proof.
                }
                for (auto* view : views)
                {
                    core->DestroyDescriptor(view);
                }
                for (auto* fence : acquired)
                {
                    core->DestroyFence(fence);
                }
                for (auto* fence : released)
                {
                    core->DestroyFence(fence);
                }
                operations.DestroySwapChain(swap_chain);
                const auto result = status == nri::Result::SUCCESS
                                        ? RenderResult<void>{}
                                        : RenderResult<void>{cxx::unexpected(nativeError(status))};
                if (complete)
                {
                    complete(completion, result);
                }
            };
        if (frontiers)
        {
            session_.deferRelease(*frontiers, std::move(release));
        }
        else
        {
            session_.deferRelease(std::move(release));
        }
    }
} // namespace lux::render::detail
