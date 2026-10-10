#pragma once
#include "lux/engine/gapi/vk/vk.hpp" // platform::gapi
#include "lux/engine/function/visibility.h"
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/client/core/DeviceCaps.hpp>
#include <lux/engine/function/render/client/core/EFeatureLevel.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lux::render
{
    /**
     * @brief Debug callback information structure
     * @details Contains all information passed to Vulkan debug callbacks
     */
    struct DebugCallbackInfo
    {
        VkDebugReportFlagsEXT flags;           ///< Debug report flags
        VkDebugReportObjectTypeEXT objectType; ///< Object type that triggered the callback
        uint64_t object;                       ///< Handle of the object
        size_t location;                       ///< Location within the command buffer or object
        int32_t messageCode;                   ///< Layer-specific message code
        const char* layerPrefix;               ///< Abbreviation of the layer generating the message
        const char* message;                   ///< Debug message string
    };
    /// @brief Function type for debug callbacks
    using DebugCallback = std::function<bool(const DebugCallbackInfo&)>;

    /**
     * @brief Physical device selection policy
     */
    enum class EPhysicalDeviceSelectionPolicy
    {
        DISCRETE_GPU_PREFERRED,  ///< Prefer discrete GPUs, fallback to integrated
        INTEGRATED_GPU_PREFERRED ///< Prefer integrated GPUs, fallback to discrete
    };

    /// Complete instance and optional validation backing. Callback storage remains
    /// fixed through native report destruction; children must release before this owner.
    class LUX_FUNCTION_PUBLIC InstanceContext final
    {
    public:
        using CreateResult = Expected<std::unique_ptr<InstanceContext>>;

        [[nodiscard]] static CreateResult create(
            const std::vector<const char*>& required_extensions,
            DebugCallback debug_callback = {},
            VkAllocationCallbacks* allocator = nullptr
        ) noexcept;

        ~InstanceContext() = default;
        InstanceContext(const InstanceContext&) = delete;
        InstanceContext& operator=(const InstanceContext&) = delete;
        InstanceContext(InstanceContext&&) = delete;
        InstanceContext& operator=(InstanceContext&&) = delete;

        [[nodiscard]] const lux::gapi::vk::Instance& instance() const noexcept
        {
            return instance_;
        }

        [[nodiscard]] VkAllocationCallbacks* allocator() noexcept
        {
            return allocator_;
        }

        [[nodiscard]] const VkAllocationCallbacks* allocator() const noexcept
        {
            return allocator_;
        }

        [[nodiscard]] bool isInstanceExtensionEnabled(const char* name) const noexcept;

    private:
        InstanceContext(
            std::unique_ptr<DebugCallback> callback,
            lux::gapi::vk::Instance instance,
            lux::gapi::vk::DebugReport report,
            VkAllocationCallbacks* allocator,
            std::vector<std::string> extensions
        ) noexcept;

        std::unique_ptr<DebugCallback> debug_callback_;
        lux::gapi::vk::Instance instance_;
        lux::gapi::vk::DebugReport debug_report_;
        VkAllocationCallbacks* allocator_;
        std::vector<std::string> enabled_extensions_;
    };

    /**
     * @brief Device-level rendering context (one per GPU device)
     * @details Manages physical device selection and logical device creation
     */
    class LUX_FUNCTION_PUBLIC DeviceContext final
    {
    public:
        using CreateResult = Expected<std::unique_ptr<DeviceContext>>;

        [[nodiscard]] static CreateResult create(
            InstanceContext& instance_context,
            EPhysicalDeviceSelectionPolicy policy
        ) noexcept;

        ~DeviceContext();
        DeviceContext(const DeviceContext&) = delete;
        DeviceContext& operator=(const DeviceContext&) = delete;
        DeviceContext(DeviceContext&&) = delete;
        DeviceContext& operator=(DeviceContext&&) = delete;

        /// @brief Get mutable reference to physical device
        lux::gapi::vk::PhysicalDevice& physicalDevice()
        {
            return backing_.physical_device;
        }

        /// @brief Get immutable reference to physical device
        const lux::gapi::vk::PhysicalDevice& physicalDevice() const
        {
            return backing_.physical_device;
        }

        /// @brief Physical-GPU UUID (VK_UUID_SIZE bytes) for matching the CUDA device
        ///        in CUDA-Vulkan external-memory interop. See PhysicalDevice::deviceUUID.
        std::array<uint8_t, VK_UUID_SIZE> deviceUUID() const
        {
            return backing_.physical_device.deviceUUID();
        }

        /// @brief Whether external-memory/semaphore interop (CUDA zero-copy) is enabled
        ///        on this device — the platform handle path (Win32 handle / POSIX fd) was
        ///        available and got enabled. False on unsupported drivers / platforms —
        ///        callers must fall back to the host-upload path.
        bool supportsExternalMemory() const
        {
            return backing_.supports_external_interop;
        }

        /// @brief Whether VK_EXT_swapchain_maintenance1 present-scaling is enabled.
        ///        When true, a swapchain can be created with VkSwapchainPresentScaling
        ///        CreateInfoEXT so imageExtent need not exactly equal currentExtent —
        ///        closing the caps-query↔create TOCTOU race that fires VUID-07781 for
        ///        cross-thread (imgui secondary viewport) swapchain creation.
        bool supportsSwapchainMaintenance1() const
        {
            return backing_.supports_swapchain_maintenance1;
        }

        /// @brief What the created VkDevice actually enabled + key limits.
        ///        Complete at construction; the attach-time EFeatureLevel
        ///        negotiation reads this (mobile-adaptation topic ①).
        const DeviceCaps& caps() const
        {
            return backing_.caps;
        }

        /// @brief Resolve the session feature level = min(device-achievable,
        ///        caller preference). Called once by RenderServer::init right
        ///        after device creation; features read featureLevel() at attach.
        void resolveFeatureLevel(EFeatureLevel preferred)
        {
            const EFeatureLevel achievable = achievableFeatureLevel(backing_.caps);
            feature_level_ = preferred < achievable ? preferred : achievable;
        }

        /// @brief The resolved session tier (valid after resolveFeatureLevel).
        EFeatureLevel featureLevel() const
        {
            return feature_level_;
        }

        /// @brief Borrow the logical device; native ownership remains in this context.
        const lux::gapi::vk::LogicalDevice& logicalDevice()
        {
            return backing_.logical_device;
        }

        /// @brief Get immutable reference to logical device
        const lux::gapi::vk::LogicalDevice& logicalDevice() const
        {
            return backing_.logical_device;
        }

        /// @brief Get mutable reference to graphics queue
        lux::gapi::vk::Queue& graphicsQueue()
        {
            return backing_.graphics_queue;
        }

        /// @brief Get immutable reference to graphics queue
        const lux::gapi::vk::Queue& graphicsQueue() const
        {
            return backing_.graphics_queue;
        }

        std::mutex& graphicsQueueMutex() noexcept
        {
            return graphics_queue_mutex_;
        }

        /// @brief Get graphics queue family index
        uint32_t graphicsQueueFamilyIndex() const
        {
            return backing_.graphics_queue_family_index;
        }

        /// @brief Check if a dedicated async compute queue is available
        bool hasAsyncComputeQueue() const
        {
            return backing_.has_async_compute;
        }

        /// @brief Get mutable reference to async compute queue (falls back to graphics queue if unavailable)
        lux::gapi::vk::Queue& asyncComputeQueue()
        {
            return backing_.has_async_compute ? backing_.async_compute_queue : backing_.graphics_queue;
        }

        /// @brief Get immutable reference to async compute queue
        const lux::gapi::vk::Queue& asyncComputeQueue() const
        {
            return backing_.has_async_compute ? backing_.async_compute_queue : backing_.graphics_queue;
        }

        std::mutex& asyncComputeQueueMutex() noexcept
        {
            return backing_.has_async_compute ? async_compute_queue_mutex_ : graphics_queue_mutex_;
        }

        /// @brief Get async compute queue family index (falls back to graphics family if unavailable)
        uint32_t asyncComputeQueueFamilyIndex() const
        {
            return backing_.has_async_compute ? backing_.async_compute_queue_family_index
                                              : backing_.graphics_queue_family_index;
        }

        /// @brief Check if a dedicated transfer queue is available
        bool hasTransferQueue() const
        {
            return backing_.has_transfer;
        }

        /// @brief Get mutable reference to transfer queue (falls back to graphics queue if unavailable)
        lux::gapi::vk::Queue& transferQueue()
        {
            return backing_.has_transfer ? backing_.transfer_queue : backing_.graphics_queue;
        }

        /// @brief Get immutable reference to transfer queue
        const lux::gapi::vk::Queue& transferQueue() const
        {
            return backing_.has_transfer ? backing_.transfer_queue : backing_.graphics_queue;
        }

        std::mutex& transferQueueMutex() noexcept
        {
            return backing_.has_transfer ? transfer_queue_mutex_ : graphics_queue_mutex_;
        }

        /// Sole runtime entry for vkDeviceWaitIdle. Vulkan requires the call
        /// to be externally synchronized against every queue on the device.
        [[nodiscard]] VkResult waitIdle() noexcept;

        /// @brief Get transfer queue family index (falls back to graphics family if unavailable)
        uint32_t transferQueueFamilyIndex() const
        {
            return backing_.has_transfer ? backing_.transfer_queue_family_index : backing_.graphics_queue_family_index;
        }

        /// @brief Get mutable reference to instance context
        InstanceContext& instanceContext()
        {
            return instance_context_;
        }

        /// @brief Get immutable reference to instance context
        const InstanceContext& instanceContext() const
        {
            return instance_context_;
        }

        /// @brief Borrow the VMA allocator; DeviceContext retains ownership.
        VmaAllocator vmaAllocator() const noexcept
        {
            return backing_.vma_allocator.get();
        }

    private:
        struct Backing
        {
            lux::gapi::vk::PhysicalDevice physical_device;
            lux::gapi::vk::LogicalDevice logical_device;
            VmaAllocatorOwner vma_allocator;
            lux::gapi::vk::Queue graphics_queue;
            lux::gapi::vk::Queue async_compute_queue;
            lux::gapi::vk::Queue transfer_queue;
            uint32_t graphics_queue_family_index{std::numeric_limits<uint32_t>::max()};
            uint32_t async_compute_queue_family_index{std::numeric_limits<uint32_t>::max()};
            uint32_t transfer_queue_family_index{std::numeric_limits<uint32_t>::max()};
            bool has_async_compute{};
            bool has_transfer{};
            bool supports_external_interop{};
            bool supports_swapchain_maintenance1{};
            DeviceCaps caps{};
        };

        DeviceContext(InstanceContext& instance_context, Backing backing) noexcept;

        InstanceContext& instance_context_;
        Backing backing_;
        std::mutex graphics_queue_mutex_;
        std::mutex async_compute_queue_mutex_;
        std::mutex transfer_queue_mutex_;

        // Resolved session tier — min(achievable-from-caps, caller preference).
        // Defaults to Desktop so pre-existing paths that never call
        // resolveFeatureLevel keep today's behaviour.
        EFeatureLevel feature_level_{EFeatureLevel::LEVEL_DESKTOP};
    };

    // ---------------------------------------------------------------------------
    // T3-2: Descriptor pool size configuration (sensible defaults match the old
    //       hard-coded values; override any field to raise specific limits).
    // ---------------------------------------------------------------------------
    struct DescriptorPoolConfig
    {
        uint32_t sampler = 32;
        uint32_t combined_image_sampler = 128;
        uint32_t sampled_image = 64;
        uint32_t storage_image = 32;
        uint32_t uniform_texel_buffer = 16;
        uint32_t storage_texel_buffer = 16;
        uint32_t uniform_buffer = 128;
        uint32_t storage_buffer = 128;
        uint32_t uniform_buffer_dynamic = 32;
        uint32_t storage_buffer_dynamic = 16;
        uint32_t input_attachment = 16;
        uint32_t max_sets = 256;
    };

    /// Complete descriptor/command pool ownership. The device and its allocation callbacks
    /// outlive this address-stable context; destruction requires the caller's GPU-safe point.
    class LUX_FUNCTION_PUBLIC ResourceContext
    {
    public:
        using CreateResult = Expected<std::unique_ptr<ResourceContext>>;

        [[nodiscard]] static CreateResult
        create(DeviceContext& device_context, const DescriptorPoolConfig& pool_config = {}) noexcept;

        ~ResourceContext() = default;
        ResourceContext(const ResourceContext&) = delete;
        ResourceContext& operator=(const ResourceContext&) = delete;
        ResourceContext(ResourceContext&&) = delete;
        ResourceContext& operator=(ResourceContext&&) = delete;

        [[nodiscard]] VkDescriptorPool descriptorPool() const noexcept
        {
            return pools_.descriptors.get();
        }

        [[nodiscard]] VkCommandPool commandPool() const noexcept
        {
            return pools_.graphics.get();
        }

        [[nodiscard]] VkCommandPool computeCommandPool() const noexcept
        {
            return pools_.compute.get();
        }

        [[nodiscard]] VkCommandPool transferCommandPool() const noexcept
        {
            return pools_.transfer.get();
        }

        /// @brief Get mutable reference to instance context
        InstanceContext& instanceContext()
        {
            return device_context_.instanceContext();
        }

        /// @brief Get immutable reference to instance context
        const InstanceContext& instanceContext() const
        {
            return device_context_.instanceContext();
        }

        /// @brief Get mutable reference to device context
        DeviceContext& deviceContext()
        {
            return device_context_;
        }

        /// @brief Get immutable reference to device context
        const DeviceContext& deviceContext() const
        {
            return device_context_;
        }

        // Convenience methods to reduce chain calls
        /// @brief Borrow the logical device; native ownership remains in this context.
        const lux::gapi::vk::LogicalDevice& logicalDevice()
        {
            return device_context_.logicalDevice();
        }

        /// @brief Get immutable reference to logical device
        const lux::gapi::vk::LogicalDevice& logicalDevice() const
        {
            return device_context_.logicalDevice();
        }

        /// @brief Get mutable reference to graphics queue
        lux::gapi::vk::Queue& graphicsQueue()
        {
            return device_context_.graphicsQueue();
        }

        /// @brief Get immutable reference to graphics queue
        const lux::gapi::vk::Queue& graphicsQueue() const
        {
            return device_context_.graphicsQueue();
        }

        /// @brief Get VMA allocator
        VmaAllocator vmaAllocator() const
        {
            return device_context_.vmaAllocator();
        }

        /// @brief Get mutable reference to physical device
        lux::gapi::vk::PhysicalDevice& physicalDevice()
        {
            return device_context_.physicalDevice();
        }

        /// @brief Get immutable reference to physical device
        const lux::gapi::vk::PhysicalDevice& physicalDevice() const
        {
            return device_context_.physicalDevice();
        }

        /// @brief Get graphics queue family index
        uint32_t graphicsQueueFamilyIndex() const
        {
            return device_context_.graphicsQueueFamilyIndex();
        }

    private:
        struct Pools
        {
            DescriptorPoolOwner descriptors;
            CommandPoolOwner graphics;
            CommandPoolOwner compute;
            CommandPoolOwner transfer;
        };

        ResourceContext(DeviceContext& device_context, Pools pools) noexcept;

        DeviceContext& device_context_;
        Pools pools_;
    };
}
