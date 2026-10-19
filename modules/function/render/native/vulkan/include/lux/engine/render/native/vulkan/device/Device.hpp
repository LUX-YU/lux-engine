#pragma once

#include <array>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <lux/engine/render/native/vulkan/device/DeviceOptions.hpp>
#include <memory>

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

    // Borrows device/instance; allocations must be destroyed before this owner.
    class VulkanAllocator
    {
    public:
        [[nodiscard]] static RenderResult<VulkanAllocator> create(
            VkInstance instance,
            VkPhysicalDevice physical,
            VkDevice device,
            bool buffer_device_address
        ) noexcept;
        // Returned objects own their Native allocation; this allocator must outlive every reference.
        [[nodiscard]] RenderResult<std::unique_ptr<Buffer>> createBuffer(
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            EMemoryAccess access,
            std::span<const std::uint32_t> families = {}
        ) noexcept;
        [[nodiscard]] RenderResult<std::unique_ptr<Image>> createImage(const ImageDescription& description) noexcept;
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
} // namespace lux::render::vulkan
