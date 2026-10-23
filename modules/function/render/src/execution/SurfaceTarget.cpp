#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/SurfaceTarget.hpp>

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
        const auto windows = unsigned(surface.window.windows.hwnd != nullptr) +
                             unsigned(surface.window.x11.dpy && surface.window.x11.window) +
                             unsigned(surface.window.wayland.display && surface.window.wayland.surface) +
                             unsigned(surface.window.metal.caMetalLayer != nullptr);
        const bool invalid_window = windows != 1;
        if (invalid_kind || invalid_window || invalid_extent)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
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
        swap_chain.window = surface.window;
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
            status = core.CreateFence(session.device(), nri::SWAPCHAIN_SEMAPHORE, released);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            output->release_semaphores_.push_back(released);
        }
        for (std::uint32_t slot = 0; slot < frame_count; ++slot)
        {
            nri::Fence* acquired = nullptr;
            status = core.CreateFence(session.device(), nri::SWAPCHAIN_SEMAPHORE, acquired);
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

    RenderResult<void> SurfaceTarget::acquire(std::uint32_t frame_slot) noexcept
    {
        if (failure_.type != 0)
        {
            return cxx::unexpected(failure_);
        }
        if (acquired_image_ == UINT32_MAX)
        {
            const auto status = swap_chain_operations_.AcquireNextTexture(
                *swap_chain_,
                *acquire_semaphores_[frame_slot],
                acquired_image_
            );
            if (status != nri::Result::SUCCESS)
            {
                acquired_image_ = UINT32_MAX;
                failure_ = nativeError(status);
                return cxx::unexpected(failure_);
            }
            presentation_ = {acquire_semaphores_[frame_slot], release_semaphores_[acquired_image_]};
        }
        frames_[frame_slot] = {images_[acquired_image_], views_[acquired_image_], &presentation_};
        return {};
    }

    RenderResult<bool> SurfaceTarget::present() noexcept
    {
        if (!presentation_.rendered_signalled)
        {
            return false;
        }
        const auto status = swap_chain_operations_.QueuePresent(*swap_chain_, *presentation_.rendered);
        has_presented_ = true;
        acquired_image_ = UINT32_MAX;
        presentation_ = {};
        if (status != nri::Result::SUCCESS)
        {
            failure_ = nativeError(status);
            return cxx::unexpected(failure_);
        }
        return true;
    }

    void SurfaceTarget::retire(void* completion, Completion complete) noexcept
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
        session_.deferRelease(
            [core = &session_.nriCore(),
             operations = swap_chain_operations_,
             swap_chain,
             queue = &session_.queue().native(),
             presented = has_presented_,
             views = std::move(views_),
             acquired = std::move(acquire_semaphores_),
             released = std::move(release_semaphores_),
             unused_acquire =
                 acquired_image_ != UINT32_MAX && !presentation_.acquired_consumed ? presentation_.acquired : nullptr,
             completion,
             complete]() noexcept
            {
                // GPU queue frontiers do not prove presentation consumed its binary semaphore.
                // This cold close/rebuild boundary drains only the actual presentation queue.
                auto status = nri::Result::SUCCESS;
                if (unused_acquire)
                {
                    // An abandoned acquired image still has a WSI signal operation. Consume it
                    // with an empty presentation wait before destroying its semaphore. There is
                    // no command allocator, frame resource or second submission queue here.
                    const nri::FenceSubmitDesc wait{unused_acquire, 0, nri::StageBits::ALL};
                    nri::QueueSubmitDesc consume{};
                    consume.waitFences = &wait;
                    consume.waitFenceNum = 1;
                    status = core->QueueSubmit(*queue, consume);
                    if (status != nri::Result::SUCCESS && status != nri::Result::DEVICE_LOST)
                    {
                        std::terminate(); // No proof that the pending WSI signal may be destroyed.
                    }
                }
                if (presented || unused_acquire)
                {
                    status = core->QueueWaitIdle(queue);
                }
                if (status != nri::Result::SUCCESS && status != nri::Result::DEVICE_LOST)
                {
                    std::terminate();
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
            }
        );
    }
} // namespace lux::render::detail
