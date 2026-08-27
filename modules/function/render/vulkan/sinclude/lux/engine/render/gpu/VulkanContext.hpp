#pragma once
#include "lux/engine/gapi/vk/vk.hpp" // platform::gapi
#include "lux/engine/function/visibility.h"
#include <lux/engine/render/gpu/VmaFwd.hpp>
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

    /**
     * @brief Instance-level rendering context (shared across the application)
     * @details Manages Vulkan instance creation and validation layers
     */
    class LUX_FUNCTION_PUBLIC InstanceContext
    {
    public:
        /**
         * @brief Construct with explicit configuration
         * @param required_extensions List of required Vulkan extensions
         * @param debug_callback Optional debug callback for validation layers
         * @param allocator Optional custom memory allocator
         */
        InstanceContext(
            const std::vector<const char*>& required_extensions,
            DebugCallback debug_callback = nullptr,
            VkAllocationCallbacks* allocator = nullptr
        );

        ~InstanceContext();

        /// @brief Get mutable reference to Vulkan instance
        lux::gapi::vk::Instance& instance()
        {
            return instance_;
        }

        /// @brief Get immutable reference to Vulkan instance
        const lux::gapi::vk::Instance& instance() const
        {
            return instance_;
        }

        /// @brief Get mutable reference to debug report
        lux::gapi::vk::DebugReport& debugReport()
        {
            return debug_report_;
        }

        /// @brief Get immutable reference to debug report
        const lux::gapi::vk::DebugReport& debugReport() const
        {
            return debug_report_;
        }

        /// @brief Get custom memory allocator
        VkAllocationCallbacks* allocator()
        {
            return allocator_;
        }

        const VkAllocationCallbacks* allocator() const
        {
            return allocator_;
        }

        /// @brief Whether the named instance extension was actually enabled at instance
        ///        creation. Generic query over the enabled-extension set — used e.g. to
        ///        gate a device extension whose prerequisites are instance-level
        ///        (VK_EXT_swapchain_maintenance1 needs VK_EXT_surface_maintenance1).
        bool isInstanceExtensionEnabled(const char* name) const;

    private:
        lux::gapi::vk::Instance instance_;            ///< Vulkan instance handle
        lux::gapi::vk::DebugReport debug_report_;     ///< Debug report for validation layers
        DebugCallback debug_callback_;                ///< User-provided debug callback
        VkAllocationCallbacks* allocator_;            ///< Custom memory allocator
        std::vector<std::string> enabled_extensions_; ///< instance extensions actually enabled
    };

    /**
     * @brief Device-level rendering context (one per GPU device)
     * @details Manages physical device selection and logical device creation
     */
    class LUX_FUNCTION_PUBLIC DeviceContext
    {
    public:
        /**
         * @brief Construct device context (minimal — call init() to complete)
         * @param instance_context Reference to instance context
         */
        explicit DeviceContext(InstanceContext& instance_context);

        /**
         * @brief Initialize device context: select physical device, create logical device, VMA
         * @param policy Policy for selecting physical device
         * @return Expected<void> Success or error code
         */
        [[nodiscard]] Expected<void> init(EPhysicalDeviceSelectionPolicy policy);

        ~DeviceContext();

        /// @brief Get mutable reference to physical device
        lux::gapi::vk::PhysicalDevice& physicalDevice()
        {
            return physical_device_;
        }

        /// @brief Get immutable reference to physical device
        const lux::gapi::vk::PhysicalDevice& physicalDevice() const
        {
            return physical_device_;
        }

        /// @brief Physical-GPU UUID (VK_UUID_SIZE bytes) for matching the CUDA device
        ///        in CUDA-Vulkan external-memory interop. See PhysicalDevice::deviceUUID.
        std::array<uint8_t, VK_UUID_SIZE> deviceUUID() const
        {
            return physical_device_.deviceUUID();
        }

        /// @brief Whether external-memory/semaphore interop (CUDA zero-copy) is enabled
        ///        on this device — the platform handle path (Win32 handle / POSIX fd) was
        ///        available and got enabled. False on unsupported drivers / platforms —
        ///        callers must fall back to the host-upload path.
        bool supportsExternalMemory() const
        {
            return supports_external_interop_;
        }

        /// @brief Whether VK_EXT_swapchain_maintenance1 present-scaling is enabled.
        ///        When true, a swapchain can be created with VkSwapchainPresentScaling
        ///        CreateInfoEXT so imageExtent need not exactly equal currentExtent —
        ///        closing the caps-query↔create TOCTOU race that fires VUID-07781 for
        ///        cross-thread (imgui secondary viewport) swapchain creation.
        bool supportsSwapchainMaintenance1() const
        {
            return supports_swapchain_maintenance1_;
        }

        /// @brief What the created VkDevice actually enabled + key limits.
        ///        Valid after init() succeeds; the attach-time EFeatureLevel
        ///        negotiation reads this (mobile-adaptation topic ①).
        const DeviceCaps& caps() const
        {
            return caps_;
        }

        /// @brief Resolve the session feature level = min(device-achievable,
        ///        caller preference). Called once by RenderServer::init right
        ///        after device creation; features read featureLevel() at attach.
        void resolveFeatureLevel(EFeatureLevel preferred)
        {
            const EFeatureLevel achievable = achievableFeatureLevel(caps_);
            feature_level_ = preferred < achievable ? preferred : achievable;
        }

        /// @brief The resolved session tier (valid after resolveFeatureLevel).
        EFeatureLevel featureLevel() const
        {
            return feature_level_;
        }

        /// @brief Get mutable reference to logical device
        lux::gapi::vk::LogicalDevice& logicalDevice()
        {
            return logical_device_;
        }

        /// @brief Get immutable reference to logical device
        const lux::gapi::vk::LogicalDevice& logicalDevice() const
        {
            return logical_device_;
        }

        /// @brief Get mutable reference to graphics queue
        lux::gapi::vk::Queue& graphicsQueue()
        {
            return graphics_queue_;
        }

        /// @brief Get immutable reference to graphics queue
        const lux::gapi::vk::Queue& graphicsQueue() const
        {
            return graphics_queue_;
        }

        std::mutex& graphicsQueueMutex() noexcept
        {
            return graphics_queue_mutex_;
        }

        /// @brief Get graphics queue family index
        uint32_t graphicsQueueFamilyIndex() const
        {
            return graphics_queue_family_index_;
        }

        /// @brief Check if a dedicated async compute queue is available
        bool hasAsyncComputeQueue() const
        {
            return has_async_compute_;
        }

        /// @brief Get mutable reference to async compute queue (falls back to graphics queue if unavailable)
        lux::gapi::vk::Queue& asyncComputeQueue()
        {
            return has_async_compute_ ? async_compute_queue_ : graphics_queue_;
        }

        /// @brief Get immutable reference to async compute queue
        const lux::gapi::vk::Queue& asyncComputeQueue() const
        {
            return has_async_compute_ ? async_compute_queue_ : graphics_queue_;
        }

        std::mutex& asyncComputeQueueMutex() noexcept
        {
            return has_async_compute_ ? async_compute_queue_mutex_ : graphics_queue_mutex_;
        }

        /// @brief Get async compute queue family index (falls back to graphics family if unavailable)
        uint32_t asyncComputeQueueFamilyIndex() const
        {
            return has_async_compute_ ? async_compute_queue_family_index_ : graphics_queue_family_index_;
        }

        /// @brief Check if a dedicated transfer queue is available
        bool hasTransferQueue() const
        {
            return has_transfer_;
        }

        /// @brief Get mutable reference to transfer queue (falls back to graphics queue if unavailable)
        lux::gapi::vk::Queue& transferQueue()
        {
            return has_transfer_ ? transfer_queue_ : graphics_queue_;
        }

        /// @brief Get immutable reference to transfer queue
        const lux::gapi::vk::Queue& transferQueue() const
        {
            return has_transfer_ ? transfer_queue_ : graphics_queue_;
        }

        std::mutex& transferQueueMutex() noexcept
        {
            return has_transfer_ ? transfer_queue_mutex_ : graphics_queue_mutex_;
        }

        /// Sole runtime entry for vkDeviceWaitIdle. Vulkan requires the call
        /// to be externally synchronized against every queue on the device.
        [[nodiscard]] VkResult waitIdle() noexcept;

        /// @brief Get transfer queue family index (falls back to graphics family if unavailable)
        uint32_t transferQueueFamilyIndex() const
        {
            return has_transfer_ ? transfer_queue_family_index_ : graphics_queue_family_index_;
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

        /// @brief Get VMA allocator
        VmaAllocator vmaAllocator()
        {
            return vma_allocator_;
        }

        /// @brief Get VMA allocator (const)
        const VmaAllocator& vmaAllocator() const
        {
            return vma_allocator_;
        }

    private:
        InstanceContext& instance_context_; ///< Reference to instance context

        // Default-initialized: vmaCreateAllocator only assigns vma_allocator_ at the
        // very end of init(); if init() fails early (no physical device, missing
        // feature, logical-device creation failure) the destructor's
        // `if (vma_allocator_) vmaDestroyAllocator(...)` would otherwise read an
        // indeterminate pointer and call vmaDestroyAllocator on garbage during the
        // graceful-failure path. graphics_queue_family_index_ is likewise read by
        // ResourceContext::init via graphicsQueueFamilyIndex(). (#28)
        VmaAllocator vma_allocator_{nullptr};
        lux::gapi::vk::PhysicalDevice physical_device_; ///< Selected physical device
        lux::gapi::vk::LogicalDevice logical_device_;   ///< Created logical device
        lux::gapi::vk::Queue graphics_queue_;           ///< Graphics queue handle
        std::mutex graphics_queue_mutex_;
        uint32_t graphics_queue_family_index_{std::numeric_limits<uint32_t>::max()}; ///< Graphics queue family index

        // Async compute queue (dedicated compute-only queue when available)
        lux::gapi::vk::Queue async_compute_queue_;
        std::mutex async_compute_queue_mutex_;
        uint32_t async_compute_queue_family_index_ = std::numeric_limits<uint32_t>::max();
        bool has_async_compute_ = false;

        // Transfer queue (dedicated transfer-only queue when available)
        lux::gapi::vk::Queue transfer_queue_;
        std::mutex transfer_queue_mutex_;
        uint32_t transfer_queue_family_index_ = std::numeric_limits<uint32_t>::max();
        bool has_transfer_ = false;

        // External-memory/semaphore interop (CUDA<->Vulkan zero-copy) enabled — platform
        // handle path (Win32 handle on Windows / opaque fd on POSIX).
        bool supports_external_interop_ = false;

        // VK_EXT_swapchain_maintenance1 present-scaling enabled on this device.
        bool supports_swapchain_maintenance1_ = false;

        // Snapshot of enabled features + limits, filled by init() right after
        // logical-device creation (see DeviceCaps.hpp for semantics).
        DeviceCaps caps_{};

        // Resolved session tier — min(achievable-from-caps, caller preference).
        // Defaults to Desktop so pre-existing paths that never call
        // resolveFeatureLevel keep today's behaviour.
        EFeatureLevel feature_level_{EFeatureLevel::Desktop};
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

    /**
     * @brief Resource management context (may change dynamically)
     * @details Manages descriptor pools and command pools for resource allocation
     */
    class LUX_FUNCTION_PUBLIC ResourceContext
    {
    public:
        /**
         * @brief Construct resource context (minimal — call init() to complete)
         * @param device_context Reference to device context
         */
        explicit ResourceContext(DeviceContext& device_context);

        /**
         * @brief Initialize resource context: create descriptor pool, command pools
         * @param pool_config Descriptor pool capacity
         * @return Expected<void> Success or error code
         */
        [[nodiscard]] Expected<void> init(const DescriptorPoolConfig& pool_config = {});

        ~ResourceContext();

        /// @brief Get mutable reference to descriptor pool
        lux::gapi::vk::DescriptorPool& descriptorPool()
        {
            return descriptor_pool_;
        }

        /// @brief Get immutable reference to descriptor pool
        const lux::gapi::vk::DescriptorPool& descriptorPool() const
        {
            return descriptor_pool_;
        }

        /// @brief Get mutable reference to command pool
        lux::gapi::vk::CommandPool& commandPool()
        {
            return command_pool_;
        }

        /// @brief Get immutable reference to command pool
        const lux::gapi::vk::CommandPool& commandPool() const
        {
            return command_pool_;
        }

        /// @brief Get mutable reference to compute command pool (for async compute queue)
        lux::gapi::vk::CommandPool& computeCommandPool()
        {
            return compute_command_pool_;
        }

        /// @brief Get immutable reference to compute command pool
        const lux::gapi::vk::CommandPool& computeCommandPool() const
        {
            return compute_command_pool_;
        }

        /// @brief Get mutable reference to transfer command pool (for dedicated transfer queue)
        lux::gapi::vk::CommandPool& transferCommandPool()
        {
            return transfer_command_pool_;
        }

        /// @brief Get immutable reference to transfer command pool
        const lux::gapi::vk::CommandPool& transferCommandPool() const
        {
            return transfer_command_pool_;
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
        /// @brief Get mutable reference to logical device
        lux::gapi::vk::LogicalDevice& logicalDevice()
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
        DeviceContext& device_context_; ///< Reference to device context

        lux::gapi::vk::DescriptorPool descriptor_pool_; ///< Descriptor pool for allocating descriptor sets
        lux::gapi::vk::CommandPool command_pool_; ///< Command pool for allocating command buffers (graphics queue)
        lux::gapi::vk::CommandPool compute_command_pool_;  ///< Command pool for async compute queue
        lux::gapi::vk::CommandPool transfer_command_pool_; ///< Command pool for dedicated transfer queue
    };
}
