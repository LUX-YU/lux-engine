#include "Presentation.hpp"
#include <Windows.h>
#include <algorithm>
#include <vulkan/vulkan_win32.h>

namespace lux::render::detail
{
    using namespace vulkan;

    namespace
    {
        struct SwapchainBacking
        {
            const VulkanDevice& device;
            SubmissionQueue& queue;
            VkSwapchainKHR swapchain{};
            VkFormat format{};
            VkExtent2D extent{};
            std::vector<VkImage> images;
            std::vector<VkSemaphore> acquired, rendered;
            std::vector<VkFence> present_fences;
            std::vector<VkFence> acquire_fences;
            std::vector<bool> present_pending;
            std::vector<bool> acquire_pending;
            std::vector<std::optional<SubmissionTicket>> acquire_use;
            std::vector<Buffer> readbacks;
            std::optional<std::uint32_t> latest_image;
            std::optional<SubmissionTicket> latest_copy;
            bool retired{};

            ~SwapchainBacking() noexcept
            {
                // Present fence is independent of submission completion. This is
                // a cold target replacement/final teardown, never per-resource idle.
                for (std::size_t i = 0; i < present_fences.size(); ++i)
                {
                    if (present_pending[i])
                    {
                        auto result = vkWaitForFences(device.native(), 1, &present_fences[i], VK_TRUE, UINT64_MAX);
                        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                        {
                            std::terminate();
                        }
                    }
                }
                if (latest_copy)
                {
                    auto done = queue.wait(*latest_copy, UINT64_MAX);
                    if (!done && !queue.deviceLost())
                    {
                        std::terminate();
                    }
                }
                for (std::size_t i = 0; i < acquire_fences.size(); ++i)
                {
                    if (!acquire_pending[i])
                    {
                        continue;
                    }
                    auto result = vkWaitForFences(device.native(), 1, &acquire_fences[i], VK_TRUE, UINT64_MAX);
                    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
                    {
                        std::terminate();
                    }
                }
                for (auto handle : acquired)
                {
                    if (handle)
                    {
                        vkDestroySemaphore(device.native(), handle, nullptr);
                    }
                }
                for (auto handle : rendered)
                {
                    if (handle)
                    {
                        vkDestroySemaphore(device.native(), handle, nullptr);
                    }
                }
                for (auto handle : present_fences)
                {
                    if (handle)
                    {
                        vkDestroyFence(device.native(), handle, nullptr);
                    }
                }
                for (auto handle : acquire_fences)
                {
                    if (handle)
                    {
                        vkDestroyFence(device.native(), handle, nullptr);
                    }
                }
                if (swapchain)
                {
                    vkDestroySwapchainKHR(device.native(), swapchain, nullptr);
                }
            }
        };

        RenderResult<std::unique_ptr<SwapchainBacking>> makeSwapchain(
            const VulkanDevice& device,
            const VulkanAllocator& allocator,
            SubmissionQueue& queue,
            VkSurfaceKHR surface,
            VkExtent2D requested,
            std::uint32_t frames,
            SwapchainBacking* previous = nullptr
        ) noexcept
        {
            VkSurfaceCapabilitiesKHR caps;
            auto result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physical(), surface, &caps);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            VkExtent2D extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : requested;
            extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
            const bool invalid_extent = extent.width == 0 || extent.height == 0;
            constexpr auto usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (invalid_extent || (caps.supportedUsageFlags & usage) != usage)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            std::uint32_t count{};
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical(), surface, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            std::vector<VkSurfaceFormatKHR> formats(count);
            result = vkGetPhysicalDeviceSurfaceFormatsKHR(device.physical(), surface, &count, formats.data());
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            const auto found = std::find_if(
                formats.begin(),
                formats.end(),
                [](auto value)
                {
                    return value.format == VK_FORMAT_B8G8R8A8_UNORM &&
                           value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                }
            );
            if (found == formats.end())
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            auto owner = std::make_unique<SwapchainBacking>(device, queue);
            owner->format = found->format;
            owner->extent = extent;
            VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            info.surface = surface;
            info.minImageCount = std::max(caps.minImageCount, frames + 1);
            if (caps.maxImageCount)
            {
                info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
            }
            info.imageFormat = found->format;
            info.imageColorSpace = found->colorSpace;
            info.imageExtent = extent;
            info.imageArrayLayers = 1;
            info.imageUsage = usage;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            if (!(caps.supportedCompositeAlpha & info.compositeAlpha))
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            info.clipped = VK_FALSE; // Actual swapchain readback is part of this target contract.
            info.oldSwapchain = previous ? previous->swapchain : VK_NULL_HANDLE;
            // Vulkan retires oldSwapchain even if this native call fails. Keep
            // that WSI fact explicit; never pretend the old target still presents.
            if (previous)
            {
                previous->retired = true;
            }
            result = vkCreateSwapchainKHR(device.native(), &info, nullptr, &owner->swapchain);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            result = vkGetSwapchainImagesKHR(device.native(), owner->swapchain, &count, nullptr);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            owner->images.resize(count);
            result = vkGetSwapchainImagesKHR(device.native(), owner->swapchain, &count, owner->images.data());
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            owner->acquired.resize(frames);
            owner->acquire_use.resize(frames);
            owner->acquire_fences.resize(frames);
            owner->acquire_pending.resize(frames);
            owner->rendered.resize(count);
            owner->present_fences.resize(count);
            owner->present_pending.resize(count);
            owner->readbacks.reserve(count);
            const VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (auto& handle : owner->acquired)
            {
                result = vkCreateSemaphore(device.native(), &semaphore, nullptr, &handle);
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            for (auto& handle : owner->acquire_fences)
            {
                result = vkCreateFence(device.native(), &fence, nullptr, &handle);
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            for (std::uint32_t i = 0; i < count; ++i)
            {
                result = vkCreateSemaphore(device.native(), &semaphore, nullptr, &owner->rendered[i]);
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                result = vkCreateFence(device.native(), &fence, nullptr, &owner->present_fences[i]);
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                auto readback = Buffer::create(
                    allocator,
                    VkDeviceSize{extent.width} * extent.height * 4,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    EMemoryAccess::READBACK
                );
                if (!readback)
                {
                    return cxx::unexpected(readback.error());
                }
                owner->readbacks.push_back(std::move(*readback));
            }
            return owner;
        }

        void imageBarrier(
            VkCommandBuffer command,
            VkImage image,
            VkImageLayout before,
            VkImageLayout after,
            VkPipelineStageFlags2 source_stage,
            VkAccessFlags2 source_access,
            VkPipelineStageFlags2 destination_stage,
            VkAccessFlags2 destination_access,
            std::uint32_t source_family = VK_QUEUE_FAMILY_IGNORED,
            std::uint32_t destination_family = VK_QUEUE_FAMILY_IGNORED
        ) noexcept
        {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = source_stage;
            barrier.srcAccessMask = source_access;
            barrier.dstStageMask = destination_stage;
            barrier.dstAccessMask = destination_access;
            barrier.oldLayout = before;
            barrier.newLayout = after;
            barrier.srcQueueFamilyIndex = source_family;
            barrier.dstQueueFamilyIndex = destination_family;
            barrier.image = image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }
    } // namespace

    struct PresentationTarget::Backing
    {
        const VulkanDevice& device;
        const VulkanAllocator& allocator;
        SubmissionQueue& queue;
        VkSurfaceKHR surface{};
        std::unique_ptr<SwapchainBacking> current;
        std::uint32_t frames;
        std::uint64_t presents{};

        ~Backing() noexcept
        {
            current.reset();
            if (surface)
            {
                vkDestroySurfaceKHR(device.instance(), surface, nullptr);
            }
        }
    };

    PresentationTarget::PresentationTarget(std::unique_ptr<Backing> backing) noexcept : backing_(std::move(backing)) {}

    PresentationTarget::~PresentationTarget() noexcept = default;

    RenderResult<std::unique_ptr<PresentationTarget>> PresentationTarget::create(
        const VulkanDevice& device,
        const VulkanAllocator& allocator,
        SubmissionQueue& queue,
        void* window_instance,
        void* window,
        VkExtent2D extent,
        std::uint32_t frames
    ) noexcept
    {
        const bool invalid = !window || !window_instance || frames < 2 || frames > 3 || !device.swapchainMaintenance();
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto owner = std::make_unique<Backing>(device, allocator, queue, VK_NULL_HANDLE, nullptr, frames);
        VkWin32SurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        info.hinstance = static_cast<HINSTANCE>(window_instance);
        info.hwnd = static_cast<HWND>(window);
        auto result = vkCreateWin32SurfaceKHR(device.instance(), &info, nullptr, &owner->surface);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        VkBool32 supported{};
        result = vkGetPhysicalDeviceSurfaceSupportKHR(
            device.physical(),
            queue.nativeQueue().family,
            owner->surface,
            &supported
        );
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        if (!supported)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        auto swapchain = makeSwapchain(device, allocator, queue, owner->surface, extent, frames);
        if (!swapchain)
        {
            return cxx::unexpected(swapchain.error());
        }
        owner->current = std::move(*swapchain);
        return std::unique_ptr<PresentationTarget>{new PresentationTarget(std::move(owner))};
    }

    RenderResult<void> PresentationTarget::resize(VkExtent2D extent) noexcept
    {
        auto& b = *backing_;
        auto candidate = makeSwapchain(b.device, b.allocator, b.queue, b.surface, extent, b.frames, b.current.get());
        if (!candidate)
        {
            if (b.current && b.current->retired)
            {
                b.current.reset();
            }
            return cxx::unexpected(candidate.error());
        }
        if ((*candidate)->extent.width != extent.width || (*candidate)->extent.height != extent.height)
        {
            b.current.reset();
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        b.current = std::move(*candidate);
        return {};
    }

    RenderResult<void> PresentationTarget::validateSource(VkFormat format) const noexcept
    {
        if (!backing_->current)
        {
            return cxx::unexpected(nativeError(VK_ERROR_OUT_OF_DATE_KHR));
        }
        VkFormatProperties source{}, target{};
        vkGetPhysicalDeviceFormatProperties(backing_->device.physical(), format, &source);
        vkGetPhysicalDeviceFormatProperties(backing_->device.physical(), backing_->current->format, &target);
        const bool supported = (source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0 &&
                               (target.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
        if (!supported)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        return {};
    }

    RenderResult<bool> PresentationTarget::present(
        std::span<PresentationLayer> layers,
        std::uint32_t frame_slot
    ) noexcept
    {
        auto& b = *backing_;
        if (!b.current)
        {
            return cxx::unexpected(nativeError(VK_ERROR_OUT_OF_DATE_KHR));
        }
        auto& target = *b.current;
        if (layers.empty() || frame_slot >= target.acquired.size())
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (auto& layer : layers)
        {
            auto exported = layer.invocations->exportedBacking(layer.receipt, layer.output);
            if (!exported)
            {
                return cxx::unexpected(exported.error());
            }
            const auto* image = std::get_if<std::reference_wrapper<const Image>>(&*exported);
            if (!image)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            layer.source = &image->get();
            // Native Graph exported state or the previous Target's explicit
            // Driver state, carried by the same Runtime backing owner.
            const bool invalid_source = layer.source->samples() != VK_SAMPLE_COUNT_1_BIT ||
                                        (layer.source->usage() & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0;
            if (invalid_source)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            const auto family = layer.source_state.state.family;
            const bool transfer = family != VK_QUEUE_FAMILY_IGNORED && family != b.queue.nativeQueue().family;
            if (transfer && !layer.source_state.ready)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        if (target.acquire_use[frame_slot])
        {
            auto complete = b.queue.poll();
            if (!complete)
            {
                return cxx::unexpected(complete.error());
            }
            if (*complete < target.acquire_use[frame_slot]->serial())
            {
                return false;
            }
        }
        if (target.acquire_pending[frame_slot])
        {
            auto ready = vkGetFenceStatus(b.device.native(), target.acquire_fences[frame_slot]);
            if (ready == VK_NOT_READY)
            {
                return false;
            }
            if (ready != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(ready));
            }
            auto reset = vkResetFences(b.device.native(), 1, &target.acquire_fences[frame_slot]);
            if (reset != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(reset));
            }
            target.acquire_pending[frame_slot] = false;
        }
        std::uint32_t index{};
        auto result = vkAcquireNextImageKHR(
            b.device.native(),
            target.swapchain,
            0,
            target.acquired[frame_slot],
            target.acquire_fences[frame_slot],
            &index
        );
        if (result == VK_NOT_READY || result == VK_TIMEOUT)
        {
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            return cxx::unexpected(nativeError(result));
        }
        target.acquire_pending[frame_slot] = true;
        if (target.present_pending[index])
        {
            result = vkWaitForFences(b.device.native(), 1, &target.present_fences[index], VK_TRUE, UINT64_MAX);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            result = vkResetFences(b.device.native(), 1, &target.present_fences[index]);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            target.present_pending[index] = false;
        }
        for (auto& layer : layers)
        {
            const auto state = layer.source_state.state;
            if (state.family == VK_QUEUE_FAMILY_IGNORED || state.family == b.queue.nativeQueue().family)
            {
                continue;
            }
            auto& source_queue = const_cast<SubmissionQueue&>(layer.source_state.ready->owner());
            auto release = source_queue.begin();
            if (!release)
            {
                return cxx::unexpected(release.error());
            }
            imageBarrier(
                release->native(),
                layer.source->native(),
                state.layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                state.stages,
                state.access,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE,
                state.family,
                b.queue.nativeQueue().family
            );
            auto released = std::move(*release).submit();
            if (!released)
            {
                return cxx::unexpected(released.error());
            }
            auto pin = layer.invocations->retainExternalUse(layer.receipt, *released);
            if (!pin)
            {
                return cxx::unexpected(pin.error());
            }
            layer.receipt = *pin;
        }
        // Every Native receipt and external-release join is on this same
        // graphics queue. Queue order covers every layer without an unbounded
        // semaphore wait list or CPU wait between Views.
        auto batch = b.queue.begin();
        if (!batch)
        {
            return cxx::unexpected(batch.error());
        }
        const auto command = batch->native();
        imageBarrier(
            command,
            target.images[index],
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT
        );
        const VkClearColorValue background{{0, 0, 0, 1}};
        const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(
            command,
            target.images[index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &background,
            1,
            &whole
        );
        VkPipelineStageFlags2 previous = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        for (const auto& layer : layers)
        {
            const auto state = layer.source_state.state;
            const bool transfer =
                state.family != VK_QUEUE_FAMILY_IGNORED && state.family != b.queue.nativeQueue().family;
            imageBarrier(
                command,
                layer.source->native(),
                state.layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                transfer ? VK_PIPELINE_STAGE_2_NONE : state.stages,
                transfer ? VK_ACCESS_2_NONE : state.access,
                VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                transfer ? state.family : VK_QUEUE_FAMILY_IGNORED,
                transfer ? b.queue.nativeQueue().family : VK_QUEUE_FAMILY_IGNORED
            );
            imageBarrier(
                command,
                target.images[index],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                previous,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT
            );
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {
                static_cast<std::int32_t>(layer.source->extent().width),
                static_cast<std::int32_t>(layer.source->extent().height),
                1
            };
            blit.dstOffsets[0] = {layer.rectangle.offset.x, layer.rectangle.offset.y, 0};
            blit.dstOffsets[1] = {
                layer.rectangle.offset.x + static_cast<std::int32_t>(layer.rectangle.extent.width),
                layer.rectangle.offset.y + static_cast<std::int32_t>(layer.rectangle.extent.height),
                1
            };
            vkCmdBlitImage(
                command,
                layer.source->native(),
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                target.images[index],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &blit,
                VK_FILTER_NEAREST
            );
            previous = VK_PIPELINE_STAGE_2_BLIT_BIT;
        }
        imageBarrier(
            command,
            target.images[index],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {target.extent.width, target.extent.height, 1};
        vkCmdCopyImageToBuffer(
            command,
            target.images[index],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            target.readbacks[index].native(),
            1,
            &copy
        );
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(command, &dependency);
        imageBarrier(
            command,
            target.images[index],
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE
        );
        auto submitted = std::move(*batch).submit({}, target.acquired[frame_slot], target.rendered[index]);
        if (!submitted)
        {
            return cxx::unexpected(submitted.error());
        }
        target.acquire_use[frame_slot] = target.latest_copy = *submitted;
        target.latest_image = index;
        for (auto& layer : layers)
        {
            auto retained = layer.invocations->retainExternalUse(layer.receipt, *submitted);
            if (!retained)
            {
                return cxx::unexpected(retained.error());
            }
            layer.receipt = *retained;
            layer.copied = true;
        }
        VkSwapchainPresentFenceInfoEXT fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        fence.swapchainCount = 1;
        fence.pFences = &target.present_fences[index];
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.pNext = &fence;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &target.rendered[index];
        present.swapchainCount = 1;
        present.pSwapchains = &target.swapchain;
        present.pImageIndices = &index;
        result = vkQueuePresentKHR(b.queue.nativeQueue().handle, &present);
        target.present_pending[index] = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ||
                                        result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR;
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            return cxx::unexpected(nativeError(result));
        }
        ++b.presents;
        return true;
    }

    RenderResult<void> PresentationTarget::readback(std::uint64_t offset, std::span<std::byte> bytes) noexcept
    {
        auto& b = *backing_;
        if (!b.current)
        {
            return cxx::unexpected(nativeError(VK_ERROR_OUT_OF_DATE_KHR));
        }
        auto& target = *b.current;
        if (!target.latest_image || !target.latest_copy)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto complete = b.queue.wait(*target.latest_copy, 10'000'000);
        if (!complete)
        {
            return cxx::unexpected(complete.error());
        }
        if (!*complete)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        return target.readbacks[*target.latest_image].read(offset, bytes);
    }

    VkExtent2D PresentationTarget::extent() const noexcept
    {
        return backing_->current ? backing_->current->extent : VkExtent2D{};
    }

    bool PresentationTarget::available() const noexcept
    {
        return backing_->current != nullptr;
    }

    std::uint64_t PresentationTarget::presents() const noexcept
    {
        return backing_->presents;
    }
} // namespace lux::render::detail
