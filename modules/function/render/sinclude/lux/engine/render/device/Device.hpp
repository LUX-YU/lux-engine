#pragma once

#include <lux/engine/render/visibility.h>

#include <array>
#include <lux/engine/gapi/vk/Vma.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/runtime/DeviceOptions.hpp>
#include <memory>

struct VmaAllocator_T;

namespace lux::render::detail
{
    // Features actually enabled by successful logical-device creation.
    struct EnabledDeviceFeatures final
    {
        bool dynamic_rendering = false;
        bool timeline_semaphore = false;
        bool multiview = false;
        bool local_read = false;
        bool separate_depth_stencil = false;
        bool swapchain_maintenance = false;
        bool texture_compression_bc = false;
        bool texture_descriptors = false;
        bool host_query_reset = false;
        bool buffer_device_address = false;
    };

    enum class EMemoryAccess
    {
        DEVICE,
        UPLOAD,
        READBACK
    };

    struct ImageDescription final
    {
        VkExtent2D extent;
        VkFormat format;
        VkImageUsageFlags usage;
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        std::uint32_t mip_levels{1};
        std::uint32_t array_layers{1};
    };

    struct InstanceOptions
    {
        std::span<const char* const> extensions{};
        bool validation{false};
        PFN_vkDebugUtilsMessengerCallbackEXT diagnostic{};
        void* diagnostic_user{};
    };

    class LUX_RENDER_PUBLIC InstanceContext
    {
    public:
        [[nodiscard]] static RenderResult<InstanceContext> create(const InstanceOptions& options = {}) noexcept;
        ~InstanceContext() noexcept;
        InstanceContext(InstanceContext&& other) noexcept;
        InstanceContext& operator=(InstanceContext&& other) noexcept;
        InstanceContext(const InstanceContext&) = delete;
        InstanceContext& operator=(const InstanceContext&) = delete;

        [[nodiscard]] VkInstance native() const noexcept
        {
            return instance_;
        }

    private:
        InstanceContext(VkInstance instance, VkDebugUtilsMessengerEXT messenger) noexcept;
        void release() noexcept;

        VkInstance instance_;
        VkDebugUtilsMessengerEXT messenger_;
    };

    enum class EQueueRole
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER
    };

    // A borrowed native queue selected at device construction. Roles may share
    // one queue on constrained devices; family/index are the actual identity.
    struct NativeQueue
    {
        VkQueue handle;
        std::uint32_t family;
        std::uint32_t index;
        VkQueueFlags flags;
        bool operator==(const NativeQueue&) const noexcept = default;
    };

    struct QueueLocation
    {
        std::uint32_t family;
        std::uint32_t index;
        bool operator==(const QueueLocation&) const noexcept = default;
    };

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<std::array<QueueLocation, 3>> selectQueues(
        std::span<const VkQueueFamilyProperties> families,
        bool multiple,
        bool prefer_dedicated = true
    ) noexcept;

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<std::uint32_t> selectQueueFamily(
        std::span<const VkQueueFamilyProperties> families
    ) noexcept;

    // Borrows device/instance; allocations must be destroyed before this owner.
    class LUX_RENDER_PUBLIC VulkanAllocator
    {
    public:
        [[nodiscard]] static RenderResult<VulkanAllocator> create(
            VkInstance instance,
            VkPhysicalDevice physical,
            VkDevice device,
            bool buffer_device_address
        ) noexcept;
        // Returns lightweight allocation records. Their actual collection owner supplies this allocator at release.
        [[nodiscard]] RenderResult<gapi::vk::VmaBuffer> createBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access,
            std::span<const std::uint32_t> families = {}
        ) noexcept;
        [[nodiscard]] RenderResult<gapi::vk::VmaImage> createImage(const ImageDescription& description) noexcept;
        ~VulkanAllocator() noexcept;
        VulkanAllocator(VulkanAllocator&& other) noexcept;
        VulkanAllocator& operator=(VulkanAllocator&& other) noexcept;
        VulkanAllocator(const VulkanAllocator&) = delete;
        VulkanAllocator& operator=(const VulkanAllocator&) = delete;

        [[nodiscard]] VmaAllocator_T* native() const noexcept
        {
            return allocator_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        VulkanAllocator(VmaAllocator_T* allocator, VkDevice device, bool buffer_device_address) noexcept;
        void release() noexcept;

        VmaAllocator_T* allocator_;
        VkDevice device_;
        bool buffer_device_address_;
    };
} // namespace lux::render::detail
