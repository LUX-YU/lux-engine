#pragma once
#include "lux/engine/gapi/vk/Image.hpp"
#include "lux/engine/gapi/vk/PhysicalDevice.hpp"
#include "lux/engine/gapi/vk/Surface.hpp"
#include "lux/engine/gapi/vk/SwapchainError.hpp"
#include <lux/cxx/compile_time/expected.hpp>
#include <vector>
#include <array>
#include <optional>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace lux::gapi::vk
{
    class SwapchainBuilder;
    class Swapchain;

    using SwapchainBuildResult = SwapchainBuildExp<Swapchain>;
    using SwapchainImagesResult = SwapchainBuildExp<std::vector<Image>>;

    struct SwapchainBuildOps final
    {
        SurfaceQueryOps surface_queries{};
        PFN_vkCreateSwapchainKHR create_swapchain{nullptr};
        PFN_vkDestroySwapchainKHR destroy_swapchain{nullptr};
        PFN_vkGetSwapchainImagesKHR get_swapchain_images{nullptr};

        [[nodiscard]] static SwapchainBuildOps defaults() noexcept
        {
            return SwapchainBuildOps{
                SurfaceQueryOps::defaults(),
                &vkCreateSwapchainKHR,
                &vkDestroySwapchainKHR,
                &vkGetSwapchainImagesKHR,
            };
        }
    };

    class Swapchain
    {
    public:
        using Builder = SwapchainBuilder;

        Swapchain() noexcept = default;
        ~Swapchain() noexcept
        {
            reset();
        }

        [[nodiscard]] static SwapchainBuildResult create(
            VkDevice device,
            const VkSwapchainCreateInfoKHR& info,
            const VkAllocationCallbacks* allocator = nullptr,
            PFN_vkCreateSwapchainKHR create_swapchain = &vkCreateSwapchainKHR,
            PFN_vkDestroySwapchainKHR destroy_swapchain = &vkDestroySwapchainKHR
        ) noexcept;

        Swapchain(const Swapchain&) = delete;
        Swapchain& operator=(const Swapchain&) = delete;

        Swapchain(Swapchain&& other) noexcept
            : device_(std::exchange(other.device_, VkDevice{})), allocator_(std::exchange(other.allocator_, nullptr)),
              destroy_swapchain_(std::exchange(other.destroy_swapchain_, nullptr)),
              swapchain_(std::exchange(other.swapchain_, VkSwapchainKHR{}))
        {
        }

        Swapchain& operator=(Swapchain&& other) noexcept
        {
            if (this == &other)
                return *this;

            reset();
            device_ = std::exchange(other.device_, VkDevice{});
            allocator_ = std::exchange(other.allocator_, nullptr);
            destroy_swapchain_ = std::exchange(other.destroy_swapchain_, nullptr);
            swapchain_ = std::exchange(other.swapchain_, VkSwapchainKHR{});
            return *this;
        }

        /// Destroy the owned swapchain now. Normal scope exit calls the same
        /// operation automatically; explicit reset is only needed when rebuild
        /// ordering requires image views to die first.
        void reset() noexcept
        {
            if (swapchain_ != VK_NULL_HANDLE)
            {
                if (device_ != VK_NULL_HANDLE && destroy_swapchain_ != nullptr)
                {
                    destroy_swapchain_(device_, swapchain_, allocator_);
                }
            }
            device_ = VK_NULL_HANDLE;
            allocator_ = nullptr;
            destroy_swapchain_ = nullptr;
            swapchain_ = VK_NULL_HANDLE;
        }

        [[nodiscard]] SwapchainImagesResult
        images(PFN_vkGetSwapchainImagesKHR get_swapchain_images = &vkGetSwapchainImagesKHR) const noexcept;

        inline VkSwapchainKHR handle() const noexcept
        {
            return swapchain_;
        }
        inline const VkSwapchainKHR* handlePtr() const noexcept
        {
            return &swapchain_;
        }

    private:
        explicit Swapchain(
            VkDevice device,
            const VkAllocationCallbacks* allocator,
            PFN_vkDestroySwapchainKHR destroy_swapchain,
            VkSwapchainKHR handle
        ) noexcept
            : device_(device), allocator_(allocator), destroy_swapchain_(destroy_swapchain), swapchain_(handle)
        {
        }

        VkDevice device_{VK_NULL_HANDLE};
        const VkAllocationCallbacks* allocator_{nullptr};
        PFN_vkDestroySwapchainKHR destroy_swapchain_{nullptr};
        VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    };

    inline SwapchainBuildResult Swapchain::create(
        VkDevice device,
        const VkSwapchainCreateInfoKHR& info,
        const VkAllocationCallbacks* allocator,
        PFN_vkCreateSwapchainKHR create_swapchain,
        PFN_vkDestroySwapchainKHR destroy_swapchain
    ) noexcept
    {
        if (device == VK_NULL_HANDLE || create_swapchain == nullptr || destroy_swapchain == nullptr)
        {
            return lux::cxx::unexpected(SwapchainBuildError{
                ESwapchainBuildStage::CREATE,
                std::nullopt,
            }
            );
        }

        VkSwapchainKHR handle = VK_NULL_HANDLE;
        const VkResult result = create_swapchain(device, &info, allocator, &handle);
        if (result != VK_SUCCESS)
        {
            return lux::cxx::unexpected(SwapchainBuildError{
                ESwapchainBuildStage::CREATE,
                result,
            }
            );
        }
        if (handle == VK_NULL_HANDLE)
        {
            return lux::cxx::unexpected(SwapchainBuildError{
                ESwapchainBuildStage::CREATE,
                std::nullopt,
            }
            );
        }
        return Swapchain(device, allocator, destroy_swapchain, handle);
    }

    inline SwapchainImagesResult Swapchain::images(PFN_vkGetSwapchainImagesKHR get_swapchain_images) const noexcept
    {
        if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE || get_swapchain_images == nullptr)
        {
            return lux::cxx::unexpected(SwapchainBuildError{
                ESwapchainBuildStage::ENUMERATE_IMAGES,
                std::nullopt,
            }
            );
        }

        for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
        {
            std::uint32_t image_count = 0;
            const VkResult count_result = get_swapchain_images(device_, swapchain_, &image_count, nullptr);
            if (count_result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::ENUMERATE_IMAGES,
                    count_result,
                }
                );
            }
            if (image_count == 0)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::ENUMERATE_IMAGES,
                    std::nullopt,
                }
                );
            }

            const std::uint32_t capacity = image_count;
            std::vector<VkImage> raw_images(capacity);
            const VkResult values_result = get_swapchain_images(device_, swapchain_, &image_count, raw_images.data());
            if (values_result == VK_INCOMPLETE)
                continue;
            if (values_result != VK_SUCCESS)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::ENUMERATE_IMAGES,
                    values_result,
                }
                );
            }
            if (image_count > capacity)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::ENUMERATE_IMAGES,
                    std::nullopt,
                }
                );
            }

            std::vector<Image> result_images;
            result_images.reserve(image_count);
            for (std::uint32_t index = 0; index < image_count; ++index)
                result_images.emplace_back(raw_images[index]);
            return result_images;
        }

        return lux::cxx::unexpected(SwapchainBuildError{
            ESwapchainBuildStage::ENUMERATE_IMAGES,
            VK_INCOMPLETE,
        }
        );
    }

    class SwapchainBuilder
    {
    public:
        SwapchainBuilder(const PhysicalDevice& physical_device, const Surface& surface)
            : SwapchainBuilder(physical_device.handle(), surface.handle(), SwapchainBuildOps::defaults())
        {
        }

        SwapchainBuilder(VkPhysicalDevice physical_device, VkSurfaceKHR surface, SwapchainBuildOps ops) noexcept
            : physical_device_{physical_device}, surface_handle_{surface}, ops_(ops)
        {
            create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            create_info.surface = surface;
            create_info.pNext = nullptr;
            create_info.flags = 0;
            create_info.imageFormat = VK_FORMAT_UNDEFINED;
            create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            create_info.imageExtent = {0, 0};
            create_info.imageArrayLayers = 1;
            create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            create_info.queueFamilyIndexCount = 0;
            create_info.pQueueFamilyIndices = nullptr;
            create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
            create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            create_info.clipped = VK_TRUE;
            create_info.oldSwapchain = VK_NULL_HANDLE;
        }

        [[nodiscard]] SwapchainBuildResult
        build(VkDevice device, const VkAllocationCallbacks* allocator = nullptr) noexcept
        {
            auto support = detail::querySwapChainSupport(physical_device_, surface_handle_, ops_.surface_queries);
            if (!support)
                return lux::cxx::unexpected(support.error());
            support_details = std::move(*support);

            if (support_details.formats.empty())
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::SURFACE_FORMATS,
                    std::nullopt,
                }
                );
            }
            if (support_details.presentModes.empty())
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::SURFACE_PRESENT_MODES,
                    std::nullopt,
                }
                );
            }

            const auto& capabilities = support_details.capabilities;
            const bool is_invalid_current_extent = capabilities.currentExtent.width == 0 ||
                capabilities.currentExtent.height == 0;
            const bool is_invalid_max_extent = capabilities.maxImageExtent.width == 0 ||
                capabilities.maxImageExtent.height == 0;
            const bool is_invalid_capabilities = is_invalid_current_extent || is_invalid_max_extent;
            if (is_invalid_capabilities)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::SURFACE_CAPABILITIES,
                    std::nullopt,
                }
                );
            }
            const bool is_invalid_image_extent = create_info.imageExtent.width == 0 ||
                create_info.imageExtent.height == 0;
            const bool is_invalid_queue_families = queue_family_index[0] == UINT32_MAX ||
                queue_family_index[1] == UINT32_MAX;
            const bool is_invalid_configuration = is_invalid_image_extent || is_invalid_queue_families;
            if (is_invalid_configuration)
            {
                return lux::cxx::unexpected(SwapchainBuildError{
                    ESwapchainBuildStage::CONFIGURE,
                    std::nullopt,
                }
                );
            }

            create_info.imageExtent.width = std::clamp(
                create_info.imageExtent.width,
                capabilities.minImageExtent.width,
                capabilities.maxImageExtent.width
            );
            create_info.imageExtent.height = std::clamp(
                create_info.imageExtent.height,
                capabilities.minImageExtent.height,
                capabilities.maxImageExtent.height
            );
            create_info.presentMode = choosePresentMode(support_details.presentModes, vsync_enabled_);
            if (!format_explicit_)
            {
                const auto format = chooseSwapSurfaceFormat(support_details.formats, hdr_enabled_);
                create_info.imageFormat = format.format;
                create_info.imageColorSpace = format.colorSpace;
            }

            configureSwapchainSettings();
            return Swapchain::create(device, create_info, allocator, ops_.create_swapchain, ops_.destroy_swapchain);
        }

        SwapchainBuilder& enableVsync(bool enable)
        {
            vsync_enabled_ = enable;
            return *this;
        }

        SwapchainBuilder& enableHDR(bool enable)
        {
            hdr_enabled_ = enable;
            return *this;
        }

        SwapchainBuilder& setQueueFamilyIndices(const uint32_t (&index)[2])
        {
            queue_family_index[0] = index[0];
            queue_family_index[1] = index[1];
            return *this;
        }

        SwapchainBuilder& setQueueFamilyIndices(const std::array<uint32_t, 2>& indices)
        {
            queue_family_index = indices;
            return *this;
        }

        SwapchainBuilder& setExtent(VkExtent2D extent)
        {
            create_info.imageExtent = extent;
            return *this;
        }

        SwapchainBuilder& setFormat(VkFormat format)
        {
            create_info.imageFormat = format;
            format_explicit_ = true;
            return *this;
        }

        SwapchainBuilder& setOldSwapchain(VkSwapchainKHR oldSwapchain)
        {
            create_info.oldSwapchain = oldSwapchain;
            return *this;
        }

        SwapchainBuilder& setCompositeAlpha(VkCompositeAlphaFlagBitsKHR compositeAlpha)
        {
            create_info.compositeAlpha = compositeAlpha;
            return *this;
        }

        /// Enable VK_EXT_swapchain_maintenance1 present scaling (STRETCH). When on,
        /// imageExtent no longer must exactly equal the surface currentExtent — the
        /// presentation engine scales — which lifts VUID-07781 and lets a swapchain
        /// keep presenting through window-size churn without recreation. Requires the
        /// device to have swapchain_maintenance1 enabled (caller's responsibility).
        SwapchainBuilder& enablePresentScaling(bool enable)
        {
            present_scaling_enabled_ = enable;
            return *this;
        }

        /// 提供解析实例级扩展入口点所需的 VkInstance。不设的话缩放能力查询无从
        /// 进行,supportsStretchScaling 一律返回 false(安全侧:退回精确尺寸)。
        SwapchainBuilder& setInstance(VkInstance instance)
        {
            instance_ = instance;
            return *this;
        }

        const std::array<uint32_t, 2>& getQueueFamilyIndices() const
        {
            return queue_family_index;
        }

        const VkSwapchainCreateInfoKHR& getCreateInfo() const
        {
            return create_info;
        }

        [[nodiscard]] PFN_vkGetSwapchainImagesKHR imageEnumerationFn() const noexcept
        {
            return ops_.get_swapchain_images;
        }

        /// Whether the last build() actually chained present scaling — which is
        /// enablePresentScaling(true) AND the surface advertising STRETCH for the
        /// chosen present mode. Callers that change behaviour based on scaling
        /// (e.g. treating SUBOPTIMAL as benign instead of rebuilding) must read
        /// THIS, not what they requested: a request that quietly did not take and
        /// is still believed would suppress every rebuild the swapchain needs.
        bool presentScalingActive() const noexcept
        {
            return scaling_active_;
        }

    private:
        /// vsync OFF means "do not cap my render rate", which BOTH mailbox and
        /// immediate deliver — neither blocks the application between presents.
        /// They differ in one thing: immediate replaces the image mid-scanout, so
        /// it tears; mailbox keeps only the newest image and hands it over at
        /// vblank, so it does not.
        ///
        /// Mailbox is therefore preferred: same uncapped throughput (a frame-time
        /// benchmark measures the same number), no tearing. Immediate is kept as
        /// the fallback for drivers that do not expose mailbox, and is the only
        /// mode that shaves the last vblank of latency — at the cost of a torn
        /// picture, which is easily mistaken for a renderer bug.
        VkPresentModeKHR
        choosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vsyncEnabled)
        {
            if (!vsyncEnabled)
            {
                for (const auto& mode : availablePresentModes)
                {
                    if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                        return mode;
                }
                // No mailbox on this driver — fall back to immediate (tears).
                for (const auto& mode : availablePresentModes)
                {
                    if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                        return mode;
                }
            }

            // vsync ON → FIFO_KHR (guaranteed available, caps frame rate to refresh rate)
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats, bool hdr)
        {
            // 空表守卫:surface 在 ctor 时刻已死则 formats 为空 —— [0] 是 UB。
            // 返回哨兵即可:build() 会因空 formats 拒建,这个值到不了驱动。
            if (availableFormats.empty())
                return {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
            VkSurfaceFormatKHR chosenFormat = availableFormats[0];
            for (const auto& availableFormat : availableFormats)
            {
                if (hdr)
                {
                    if (availableFormat.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                        availableFormat.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32)
                    {
                        chosenFormat = availableFormat;
                        break;
                    }
                }
                else
                {
                    if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
                        availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    {
                        chosenFormat = availableFormat;
                        break;
                    }
                }
            }
            return chosenFormat;
        }

        void configureSwapchainSettings()
        {
            // Request minImageCount + 1 to guarantee forward progress when using
            // UINT64_MAX acquire timeout (VUID-vkAcquireNextImageKHR-surface-07783).
            // With only minImageCount images the presentation engine may hold all of
            // them, leaving none for the application to acquire.
            bool is_mailbox = create_info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR;
            uint32_t imageCount = support_details.capabilities.minImageCount + 1;
            if (is_mailbox)
                imageCount = std::max(imageCount, 3u);
            if (support_details.capabilities.maxImageCount > 0)
            {
                imageCount = std::min(imageCount, support_details.capabilities.maxImageCount);
            }
            create_info.minImageCount = imageCount;

            bool isQueueComplete{true};
            const auto& queue_family_indices = getQueueFamilyIndices();
            if (queue_family_indices[0] == UINT32_MAX)
            {
                isQueueComplete = true;
            }
            else if (queue_family_indices[0] != queue_family_indices[1])
            {
                isQueueComplete = false;
            }

            if (!isQueueComplete)
            {
                create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                create_info.queueFamilyIndexCount = 2;
                create_info.pQueueFamilyIndices = queue_family_indices.data();
            }
            else
            {
                create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                create_info.queueFamilyIndexCount = 0;
                create_info.pQueueFamilyIndices = nullptr;
            }

            create_info.preTransform = support_details.capabilities.currentTransform;
            create_info.imageArrayLayers = 1;
            create_info.clipped = VK_TRUE;

            // Present scaling (VK_EXT_swapchain_maintenance1): chain a STRETCH scaling
            // struct so imageExtent may differ from currentExtent. present_scaling_ is a
            // builder member so it outlives build(); safe to re-point create_info.pNext
            // on each (re)configure since nothing else uses that chain.
            //
            // STRETCH is only legal if it is in supportedPresentScaling **for this
            // surface AND this present mode** (VUID-VkSwapchainCreateInfoKHR-pNext-07782)
            // — and the two are genuinely independent: enableVsync() picks between
            // FIFO / MAILBOX / IMMEDIATE, and a driver may scale under one and not
            // another. Chaining it unasked is how a create with a perfectly valid
            // extent comes back VK_ERROR_INITIALIZATION_FAILED. Ask first; when the
            // answer is no, drop the chain and fall back to the exact-extent path
            // (which is what a device without maintenance1 does anyway).
            if (present_scaling_enabled_ && supportsStretchScaling(create_info.presentMode))
            {
                present_scaling_ = {};
                present_scaling_.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT;
                present_scaling_.scalingBehavior = VK_PRESENT_SCALING_STRETCH_BIT_EXT;
                create_info.pNext = &present_scaling_;
                scaling_active_ = true;
            }
            else
            {
                create_info.pNext = nullptr;
                scaling_active_ = false;
            }
        }

        /// Whether the presentation engine advertises STRETCH scaling for @p mode.
        ///
        /// Chaining VkSurfacePresentModeEXT into the query is itself only legal with
        /// VK_EXT_surface_maintenance1 enabled — which is exactly the precondition
        /// the caller had to meet before setting enablePresentScaling(true), so
        /// reaching here without it is a caller bug, not a case to handle.
        ///
        /// 入口点走 vkGetInstanceProcAddr 而不是直接调:
        /// vkGetPhysicalDeviceSurfaceCapabilities2KHR 是**扩展**函数,只有桌面的
        /// vulkan-1.lib 把它当普通导出符号提供;Android 的 libvulkan.so 只导出核心
        /// 符号,直接引用会在链接期报 undefined symbol。逐次解析是本代码库既有的
        /// 做法(见 DeviceMemory.hpp 对 vkGetMemoryFdKHR 的处理 —— 引擎不挂 volk
        /// 派发表)。解析不到就当作"不支持缩放",退回精确尺寸路径。
        bool supportsStretchScaling(VkPresentModeKHR mode) const
        {
            if (instance_ == VK_NULL_HANDLE)
                return false;

            const auto fn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
                vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")
            );
            if (fn == nullptr)
                return false;

            VkSurfacePresentModeEXT mode_info{};
            mode_info.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT;
            mode_info.presentMode = mode;

            VkPhysicalDeviceSurfaceInfo2KHR surface_info{};
            surface_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
            surface_info.pNext = &mode_info;
            surface_info.surface = surface_handle_;

            VkSurfacePresentScalingCapabilitiesEXT scaling{};
            scaling.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_EXT;

            VkSurfaceCapabilities2KHR caps2{};
            caps2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
            caps2.pNext = &scaling;

            if (fn(physical_device_, &surface_info, &caps2) != VK_SUCCESS)
                return false;

            return (scaling.supportedPresentScaling & VK_PRESENT_SCALING_STRETCH_BIT_EXT) != 0;
        }

        VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
        VkSurfaceKHR surface_handle_{VK_NULL_HANDLE};
        SwapchainBuildOps ops_{};
        /// 解析实例级扩展入口点用(见 supportsStretchScaling)。未设时缩放查询
        /// 直接返回"不支持",退回精确尺寸路径 —— 与设备没有 maintenance1 同解。
        VkInstance instance_{VK_NULL_HANDLE};
        SwapChainSupportDetails support_details{};
        VkSwapchainCreateInfoKHR create_info{};
        bool threeD{true};
        std::array<uint32_t, 2> queue_family_index{UINT32_MAX, UINT32_MAX};
        VkSwapchainPresentScalingCreateInfoEXT present_scaling_{};
        bool vsync_enabled_{true};
        bool hdr_enabled_{false};
        bool format_explicit_{false};
        bool present_scaling_enabled_{false};
        bool scaling_active_{false};
    };
}
