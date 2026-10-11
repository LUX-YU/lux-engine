#pragma once

#include <array>
#include <limits>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/vulkan/Error.hpp>

struct VmaAllocator_T;

namespace lux::render::vulkan
{
    struct InstanceOptions
    {
        std::span<const char* const> extensions{};
        bool validation{false};
        PFN_vkDebugUtilsMessengerCallbackEXT diagnostic{};
        void* diagnostic_user{};
    };

    class VulkanInstance
    {
    public:
        [[nodiscard]] static RenderResult<VulkanInstance> create(const InstanceOptions& options = {}) noexcept;
        ~VulkanInstance() noexcept;
        VulkanInstance(VulkanInstance&& other) noexcept;
        VulkanInstance& operator=(VulkanInstance&& other) noexcept;
        VulkanInstance(const VulkanInstance&) = delete;
        VulkanInstance& operator=(const VulkanInstance&) = delete;

        [[nodiscard]] VkInstance native() const noexcept
        {
            return instance_;
        }

    private:
        VulkanInstance(VkInstance instance, VkDebugUtilsMessengerEXT messenger) noexcept;
        void release() noexcept;

        VkInstance instance_;
        VkDebugUtilsMessengerEXT messenger_;
    };

    struct DeviceOptions
    {
        std::span<const char* const> extensions{};
        // Explicit enumeration index, or prefer a discrete eligible device.
        std::uint32_t physical_device_index{std::numeric_limits<std::uint32_t>::max()};
        bool dynamic_rendering{false};
        bool multiple_queues{false};
        bool multiview{false};
        bool local_read{false};
        bool separate_depth_stencil{false};
        bool prefer_dedicated_queues{true};
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

    [[nodiscard]] RenderResult<std::array<QueueLocation, 3>> selectQueues(
        std::span<const VkQueueFamilyProperties> families,
        bool multiple,
        bool prefer_dedicated = true
    ) noexcept;

    [[nodiscard]] RenderResult<std::uint32_t> selectQueueFamily(std::span<const VkQueueFamilyProperties> families
    ) noexcept;

    // Borrows the native instance. Every child must die before its device; the
    // device must die before its instance. Moving the wrapper preserves native identity.
    class VulkanDevice
    {
    public:
        [[nodiscard]] static RenderResult<VulkanDevice> create(
            const VulkanInstance& instance,
            const DeviceOptions& options = {}
        ) noexcept;
        ~VulkanDevice() noexcept;
        VulkanDevice(VulkanDevice&& other) noexcept;
        VulkanDevice& operator=(VulkanDevice&& other) noexcept;
        VulkanDevice(const VulkanDevice&) = delete;
        VulkanDevice& operator=(const VulkanDevice&) = delete;

        [[nodiscard]] VkDevice native() const noexcept
        {
            return device_;
        }

        [[nodiscard]] VkInstance instance() const noexcept
        {
            return instance_;
        }

        [[nodiscard]] VkPhysicalDevice physical() const noexcept
        {
            return physical_;
        }

        [[nodiscard]] VkQueue queue() const noexcept
        {
            return queue_;
        }

        [[nodiscard]] std::uint32_t queueFamily() const noexcept
        {
            return queue_family_;
        }

        [[nodiscard]] const VkPhysicalDeviceProperties& properties() const noexcept
        {
            return properties_;
        }

        [[nodiscard]] DeviceCaps caps() const noexcept;

        [[nodiscard]] bool dynamicRendering() const noexcept
        {
            return dynamic_rendering_;
        }

        [[nodiscard]] NativeQueue queue(EQueueRole role) const noexcept
        {
            return queues_[static_cast<std::size_t>(role)];
        }

        [[nodiscard]] bool multiview() const noexcept
        {
            return multiview_;
        }

        [[nodiscard]] bool localRead() const noexcept
        {
            return local_read_;
        }

        [[nodiscard]] bool separateDepthStencil() const noexcept
        {
            return separate_depth_stencil_;
        }

        [[nodiscard]] bool timelineSemaphore() const noexcept
        {
            return timeline_;
        }

    private:
        VulkanDevice(
            VkInstance instance,
            VkPhysicalDevice physical,
            VkDevice device,
            std::array<NativeQueue, 3> queues,
            const DeviceOptions& options
        ) noexcept;
        void release() noexcept;

        VkInstance instance_;
        VkPhysicalDevice physical_;
        VkDevice device_;
        VkQueue queue_;
        std::uint32_t queue_family_;
        VkPhysicalDeviceProperties properties_;
        bool dynamic_rendering_;
        std::array<NativeQueue, 3> queues_;
        bool multiview_;
        bool local_read_;
        bool timeline_;
        bool separate_depth_stencil_;
    };

    // Borrows device/instance; allocations must be destroyed before this owner.
    class VulkanAllocator
    {
    public:
        [[nodiscard]] static RenderResult<VulkanAllocator> create(const VulkanDevice& device) noexcept;
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
        VulkanAllocator(VmaAllocator_T* allocator, VkDevice device) noexcept;
        void release() noexcept;

        VmaAllocator_T* allocator_;
        VkDevice device_;
    };
} // namespace lux::render::vulkan
