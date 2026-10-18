#pragma once

#include <array>
#include <memory>
#include <lux/engine/render/native/vulkan/device/DeviceOptions.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>

struct VmaAllocator_T;

namespace lux::render::vulkan
{
    class Buffer;
    class Image;
    struct ImageDescription;
    enum class EMemoryAccess;

    struct InstanceOptions
    {
        std::span<const char* const> extensions{};
        bool validation{false};
        PFN_vkDebugUtilsMessengerCallbackEXT diagnostic{};
        void* diagnostic_user{};
    };

    class InstanceContext
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
            const InstanceContext& instance,
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
            return queues_[0].handle;
        }

        [[nodiscard]] std::uint32_t queueFamily() const noexcept
        {
            return queues_[0].family;
        }

        [[nodiscard]] const VkPhysicalDeviceProperties& properties() const noexcept
        {
            return properties_;
        }

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

        [[nodiscard]] bool textureCompressionBc() const noexcept { return texture_compression_bc_; }
        [[nodiscard]] bool textureDescriptors() const noexcept { return texture_descriptors_; }
        [[nodiscard]] bool hostQueryReset() const noexcept { return host_query_reset_; }

        [[nodiscard]] bool swapchainMaintenance() const noexcept
        {
            return swapchain_maintenance_;
        }

    private:
        bool texture_compression_bc_ = false;
        bool texture_descriptors_ = false;
        bool host_query_reset_ = false;
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
        VkPhysicalDeviceProperties properties_;
        bool dynamic_rendering_;
        std::array<NativeQueue, 3> queues_;
        bool multiview_;
        bool local_read_;
        bool timeline_;
        bool separate_depth_stencil_;
        bool swapchain_maintenance_;
    };

    // Borrows device/instance; allocations must be destroyed before this owner.
    class VulkanAllocator
    {
    public:
        [[nodiscard]] static RenderResult<VulkanAllocator> create(const VulkanDevice& device) noexcept;
        // Returned objects own their Native allocation; this allocator must outlive every reference.
        [[nodiscard]] RenderResult<std::shared_ptr<Buffer>> createBuffer(
            VkDeviceSize size, VkBufferUsageFlags usage, EMemoryAccess access,
            std::span<const std::uint32_t> families = {}
        ) noexcept;
        [[nodiscard]] RenderResult<std::shared_ptr<Image>> createImage(const ImageDescription& description) noexcept;
        [[nodiscard]] RenderResult<std::shared_ptr<Image>> aliasImage(const Image& source) noexcept;
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
