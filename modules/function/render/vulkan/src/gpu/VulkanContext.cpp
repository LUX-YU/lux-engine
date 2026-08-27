#include "lux/engine/render/gpu/VulkanContext.hpp"
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <vk_mem_alloc.h>
#include <limits>
#include <iomanip>
#include <string_view>
#include <vector>

namespace lux::render
{
    /**
     * @brief Vulkan debug report callback function
     * @details Called by Vulkan validation layers to report debug information
     */
    static inline VkBool32 debug_report_callback(
        VkDebugReportFlagsEXT flags,
        VkDebugReportObjectTypeEXT objectType,
        uint64_t object,
        size_t location,
        int32_t messageCode,
        const char* pLayerPrefix,
        const char* pMessage,
        void* pUserData
    );

    // InstanceContext implementation
    InstanceContext::InstanceContext(
        const std::vector<const char*>& required_extensions,
        DebugCallback debug_callback,
        VkAllocationCallbacks* allocator
    )
        : debug_callback_(debug_callback), allocator_(allocator)
    {
        lux::gapi::vk::Instance::Builder instance_builder;
        instance_builder.addExtensions(required_extensions);
        for (const char* e : required_extensions)
            enabled_extensions_.emplace_back(e);

        // Instance-level prerequisites for VK_EXT_swapchain_maintenance1 (present
        // scaling): VK_EXT_surface_maintenance1 depends on VK_KHR_get_surface_
        // capabilities2. Enable-if-present — absent on old drivers → device-level
        // swapchain_maintenance1 stays off and swapchain creation keeps its
        // exact-extent path. Adding an unsupported instance extension would fail
        // vkCreateInstance, so gate on the available list.
        {
            const auto avail = lux::gapi::vk::Instance::extensionProperties();
            auto has_inst_ext = [&](const char* name) {
                for (const auto& e : avail)
                    if (std::string_view(e.extensionName) == name)
                        return true;
                return false;
            };
            if (has_inst_ext(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                has_inst_ext(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME))
            {
                instance_builder.addExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)
                    .addExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
                enabled_extensions_.emplace_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                enabled_extensions_.emplace_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            }
        }

        if (debug_callback_)
        {
            instance_builder.enableDebugReport();
        }

        instance_ = instance_builder.build(allocator_);

        if (debug_callback_)
        {
            debug_report_ = instance_.createDebugReport(&debug_report_callback, &debug_callback_, allocator_);
        }
    }

    bool InstanceContext::isInstanceExtensionEnabled(const char* name) const
    {
        for (const auto& e : enabled_extensions_)
            if (e == name)
                return true;
        return false;
    }

    InstanceContext::~InstanceContext()
    {
        // Release resources in reverse creation order
        if (debug_callback_)
        {
            debug_report_.release(instance_, allocator_);
        }
        instance_.release(allocator_);
    }

    // DeviceContext implementation
    DeviceContext::DeviceContext(InstanceContext& instance_context) : instance_context_(instance_context)
    {
    }

    Expected<void> DeviceContext::init(EPhysicalDeviceSelectionPolicy policy)
    {
        // Select physical device
        auto devices = instance_context_.instance().listPhysicalDevices();
        if (devices.empty())
            return renderFailure<err::memory::GpuAllocationFailed>();

        switch (policy)
        {
        case EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED: {
            // First try to find a discrete GPU
            for (auto& device : devices)
            {
                if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
                {
                    physical_device_ = std::move(device);
                    break;
                }
            }
            // If no discrete GPU found, try integrated GPU as fallback
            if (!physical_device_)
            {
                for (auto& device : devices)
                {
                    if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
                    {
                        physical_device_ = std::move(device);
                        break;
                    }
                }
            }
            break;
        }
        case EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED: {
            // First try to find an integrated GPU
            for (auto& device : devices)
            {
                if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
                {
                    physical_device_ = std::move(device);
                    break;
                }
            }
            // If no integrated GPU found, try discrete GPU as fallback
            if (!physical_device_)
            {
                for (auto& device : devices)
                {
                    if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
                    {
                        physical_device_ = std::move(device);
                        break;
                    }
                }
            }
            break;
        }
        }

        // Final fallback: use the first available device if we haven't found a suitable one
        if (!physical_device_ && !devices.empty())
        {
            physical_device_ = std::move(devices.front());
        }

        if (!physical_device_)
            return renderFailure<err::memory::GpuAllocationFailed>();

        // ── Tier whitelist results (mobile-adaptation topic ①, item 1-2) ──
        // These features decide the EFeatureLevel a device can reach but are
        // NOT required to boot: they are enabled if present and recorded in
        // DeviceCaps; features negotiate against caps at attach time. Every
        // desktop GPU we target has all of them, so desktop behavior is
        // unchanged. Declared at function scope: consumed by the enable
        // structs and the VMA flags below.
        VkBool32 wl_draw_indirect_count = VK_FALSE;
        VkBool32 wl_shader_output_layer = VK_FALSE;
        VkBool32 wl_buffer_device_address = VK_FALSE;
        VkBool32 wl_shader_int64 = VK_FALSE;
        VkBool32 wl_wide_lines = VK_FALSE;
        // KHR_dynamic_rendering_local_read (Vulkan 1.4 core): input-attachment
        // style tile-local G-buffer reads under dynamic rendering — the mobile
        // deferred-lighting read path (line-B). Extension + feature pair.
        VkBool32 wl_dynamic_rendering_local_read = VK_FALSE;

        // S-10: Verify all required Vulkan features are supported BEFORE attempting
        // logical device creation.  Without this check, vkCreateDevice returns
        // VK_ERROR_FEATURE_NOT_PRESENT with no indication of which feature is absent.
        {
            VkPhysicalDeviceDynamicRenderingLocalReadFeatures feat_local_read{};
            feat_local_read.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES;

            VkPhysicalDeviceVulkan13Features feat13{};
            feat13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            feat13.pNext = &feat_local_read;

            VkPhysicalDeviceVulkan12Features feat12{};
            feat12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            feat12.pNext = &feat13;

            VkPhysicalDeviceVulkan11Features feat11{};
            feat11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            feat11.pNext = &feat12;

            VkPhysicalDeviceFeatures2 feat2{};
            feat2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            feat2.pNext = &feat11;

            vkGetPhysicalDeviceFeatures2(physical_device_, &feat2);

            std::string missing;
            auto require = [&](VkBool32 supported, const char* name) {
                if (!supported)
                {
                    missing += ' ';
                    missing += name;
                }
            };

            // ── Core floor: the engine cannot run AT ALL without these, on any
            //    tier (the EFeatureLevel threshold sits above descriptor
            //    indexing by design — see the mobile investigation §1.2).
            // Vulkan 1.3
            require(feat13.synchronization2, "synchronization2");
            require(feat13.dynamicRendering, "dynamicRendering");
            // Vulkan 1.2 — the full bindless bundle. The enable struct below
            // turns on every one of these, so every one must be checked here
            // (enabling an unsupported feature is a VUID violation; the old
            // gate silently skipped the storage/uniform-UAB entries).
            require(feat12.descriptorIndexing, "descriptorIndexing");
            require(feat12.runtimeDescriptorArray, "runtimeDescriptorArray");
            require(feat12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");
            require(feat12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
            require(
                feat12.descriptorBindingSampledImageUpdateAfterBind,
                "descriptorBindingSampledImageUpdateAfterBind"
            );
            require(
                feat12.descriptorBindingStorageBufferUpdateAfterBind,
                "descriptorBindingStorageBufferUpdateAfterBind"
            );
            require(
                feat12.descriptorBindingUniformBufferUpdateAfterBind,
                "descriptorBindingUniformBufferUpdateAfterBind"
            );
            require(feat12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending");
            require(feat12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
            require(feat12.timelineSemaphore, "timelineSemaphore");
            // Vulkan 1.1
            require(feat11.shaderDrawParameters, "shaderDrawParameters");
            // Vulkan 1.0 base features
            require(feat2.features.samplerAnisotropy, "samplerAnisotropy");
            require(feat2.features.multiDrawIndirect, "multiDrawIndirect");
            require(feat2.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");
            require(feat2.features.shaderClipDistance, "shaderClipDistance");
            // RenderCluster's 1x1 asynchronous picking pass performs an
            // atomicMin from the fragment stage. Enabling only the shader-side
            // capability is insufficient; Vulkan gates fragment SSBO writes on
            // this core feature bit.
            require(feat2.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics");

            if (!missing.empty())
                return renderFailure<err::memory::GpuAllocationFailed>();

            // ── Tier whitelist: enable-if-present. Absence no longer blocks
            //    device creation; the feature simply reads false in DeviceCaps
            //    and attach-time negotiation (①-4) rejects/downgrades the
            //    render features that need it.
            //      drawIndirectCount     → GPU-driven indirect-count draws
            //      shaderOutputLayer     → shadow caster VS gl_Layer routing
            //      bufferDeviceAddress + shaderInt64 → BDA cull (buffer_reference
            //        SPIR-V declares Int64; the pair is consumed together)
            //      wideLines             → editor gizmo/grid line width
            wl_draw_indirect_count = feat12.drawIndirectCount;
            wl_shader_output_layer = feat12.shaderOutputLayer;
            wl_buffer_device_address = feat12.bufferDeviceAddress;
            wl_shader_int64 = feat2.features.shaderInt64;
            wl_wide_lines = feat2.features.wideLines;
            wl_dynamic_rendering_local_read = feat_local_read.dynamicRenderingLocalRead;
        }

        // Create logical device
        constexpr uint32_t INVALID_QUEUE_FAMILY_INDEX = std::numeric_limits<uint32_t>::max();

        graphics_queue_family_index_ = physical_device_.findQueueFamilyIndexByFlags(VK_QUEUE_GRAPHICS_BIT);

        if (graphics_queue_family_index_ == INVALID_QUEUE_FAMILY_INDEX)
            return renderFailure<err::memory::GpuAllocationFailed>();

        // Discover dedicated async compute queue (COMPUTE but NOT GRAPHICS)
        {
            const auto& families = physical_device_.queueFamilyProperties();
            for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
            {
                const auto& props = families[i].queueFamilyProperties;
                if ((props.queueFlags & VK_QUEUE_COMPUTE_BIT) && !(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    props.queueCount > 0)
                {
                    async_compute_queue_family_index_ = i;
                    has_async_compute_ = true;
                    break;
                }
            }
        }

        // Discover dedicated transfer queue (TRANSFER but NOT GRAPHICS and NOT COMPUTE)
        {
            const auto& families = physical_device_.queueFamilyProperties();
            for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
            {
                const auto& props = families[i].queueFamilyProperties;
                const bool has_transfer_queue = (props.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
                const bool has_graphics_queue = (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                const bool has_compute_queue = (props.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                const bool has_queues = props.queueCount > 0;
                const bool is_dedicated_transfer = has_transfer_queue && !has_graphics_queue &&
                    !has_compute_queue && has_queues;
                if (is_dedicated_transfer)
                {
                    transfer_queue_family_index_ = i;
                    has_transfer_ = true;
                    break;
                }
            }
        }

        float queue_priority[] = {1.0f};

        // CRITICAL FIX: Enable necessary Vulkan features for bindless descriptor indexing
        VkPhysicalDeviceFeatures device_features{};
        device_features.samplerAnisotropy = VK_TRUE;
        device_features.multiDrawIndirect = VK_TRUE;
        device_features.drawIndirectFirstInstance = VK_TRUE;
        device_features.shaderClipDistance = VK_TRUE;
        device_features.fragmentStoresAndAtomics = VK_TRUE;
        // Whitelisted (enable-if-present, ①-2):
        device_features.wideLines = wl_wide_lines;
        // buffer_reference shaders (e.g. mesh_cull_unified.comp reading the instance
        // cull-mask via a 64-bit address) declare the SPIR-V Int64 capability, which
        // requires shaderInt64. Without it the validation layer raises
        // VUID-VkShaderModuleCreateInfo-pCode-08740 on every such shader module.
        device_features.shaderInt64 = wl_shader_int64;

        // Enable Vulkan 1.2 features for descriptor indexing
        // NOTE: VkPhysicalDeviceVulkan12Features includes all descriptor indexing features,
        // so we don't need a separate VkPhysicalDeviceDescriptorIndexingFeatures struct
        VkPhysicalDeviceVulkan12Features vulkan12_features{};
        vulkan12_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12_features.pNext = nullptr;

        // Core descriptor indexing feature (required when using VK_EXT_descriptor_indexing)
        vulkan12_features.descriptorIndexing = VK_TRUE;

        // Bindless descriptor features
        vulkan12_features.runtimeDescriptorArray = VK_TRUE;
        vulkan12_features.descriptorBindingVariableDescriptorCount = VK_TRUE;
        vulkan12_features.descriptorBindingPartiallyBound = VK_TRUE;
        vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
        vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
        vulkan12_features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        vulkan12_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vulkan12_features.timelineSemaphore = VK_TRUE;
        // Whitelisted (enable-if-present, ①-2):
        vulkan12_features.drawIndirectCount = wl_draw_indirect_count;
        vulkan12_features.shaderOutputLayer = wl_shader_output_layer;
        // Buffer device address (BDA): lets a shader read an SSBO via a 64-bit
        // address carried in push-constants instead of a dedicated descriptor
        // binding. Used by the GPU-driven cull so the world-partition active-mask
        // is data-driven (no fixed binding 8) and large-world is opt-in. The VMA
        // allocator below must also opt in (VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT).
        vulkan12_features.bufferDeviceAddress = wl_buffer_device_address;

        // Enable Vulkan 1.1 features for shader draw parameters
        VkPhysicalDeviceVulkan11Features vulkan11_features{};
        vulkan11_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        vulkan11_features.pNext = nullptr;
        vulkan11_features.shaderDrawParameters = VK_TRUE;

        // Enable synchronization2 feature for modern barrier commands
        VkPhysicalDeviceVulkan13Features vulkan13_features{};
        vulkan13_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vulkan13_features.pNext = nullptr;
        vulkan13_features.synchronization2 = VK_TRUE;
        vulkan13_features.dynamicRendering = VK_TRUE;

        // Query and conditionally enable VK_EXT_robustness2 nullDescriptor.
        // When enabled, VUID-vkCmdDraw-None-04008's guard condition
        // ("If the nullDescriptor feature is not enabled") becomes false, which
        // suppresses spurious validation errors that fire when a pipeline with zero
        // vertex input bindings is drawn after a pipeline that did bind vertex buffers.
        // This is a known Validation Layer 1.3.280 bug.
        // 设备扩展列表只枚举一次,robustness2 / local_read / interop 共用。
        uint32_t device_ext_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &device_ext_count, nullptr);
        std::vector<VkExtensionProperties> device_exts(device_ext_count);
        vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &device_ext_count, device_exts.data());
        auto has_device_ext = [&](const char* name) {
            for (const auto& e : device_exts)
                if (std::string_view(e.extensionName) == name)
                    return true;
            return false;
        };

        const bool has_robustness2 = has_device_ext(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

        // local_read 白名单收紧:feature 位为真但扩展名不在设备列表(某些
        // 驱动/层组合)时,无条件 addExtension 会让 vkCreateDevice 直接
        // EXTENSION_NOT_PRESENT——引擎整体起不来,违背 enable-if-present 的
        // 初衷。注意生效 API 版本被 instance 请求(Instance.hpp 的
        // VK_API_VERSION_1_3)钳制,local_read 在 1.3 语义下永远是扩展——
        // 引擎升 1.4 时此处再引入 core 分支(免扩展名)。
        if (wl_dynamic_rendering_local_read && !has_device_ext(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME))
        {
            wl_dynamic_rendering_local_read = VK_FALSE;
        }
        VkPhysicalDeviceRobustness2FeaturesEXT robustness2_features{};
        robustness2_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
        robustness2_features.pNext = nullptr;
        robustness2_features.nullDescriptor = VK_TRUE;

        // KHR_dynamic_rendering_local_read enable struct (whitelisted, ①-2):
        // tile-local G-buffer reads for the mobile deferred read path. Requires
        // BOTH the feature chain entry and the extension name at device create
        // (core only from Vulkan 1.4).
        VkPhysicalDeviceDynamicRenderingLocalReadFeatures local_read_features{};
        local_read_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES;
        local_read_features.pNext = nullptr;
        local_read_features.dynamicRenderingLocalRead = VK_TRUE;

        // VK_EXT_swapchain_maintenance1 present-scaling enable struct. Requires the
        // instance prerequisites (surface_maintenance1) AND the device extension AND
        // the feature bit. Enable-if-present: absent → present scaling stays off and
        // swapchains keep the exact-extent create path. Closes the caps↔create TOCTOU
        // race (VUID-07781) for cross-thread imgui secondary-viewport swapchains.
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maint1_features{};
        swapchain_maint1_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        swapchain_maint1_features.pNext = nullptr;
        swapchain_maint1_features.swapchainMaintenance1 = VK_TRUE;
        bool enable_swapchain_maint1 =
            instance_context_.isInstanceExtensionEnabled(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
            has_device_ext(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        if (enable_swapchain_maint1)
        {
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT q{};
            q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 q2{};
            q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            q2.pNext = &q;
            vkGetPhysicalDeviceFeatures2(physical_device_, &q2);
            enable_swapchain_maint1 = (q.swapchainMaintenance1 == VK_TRUE);
        }
        supports_swapchain_maintenance1_ = enable_swapchain_maint1;
        // Chain the features: vulkan12 -> vulkan11 -> vulkan13 [-> robustness2] [-> local_read]
        vulkan12_features.pNext = &vulkan11_features;
        vulkan11_features.pNext = &vulkan13_features;
        void** chain_tail = &vulkan13_features.pNext;
        if (has_robustness2)
        {
            *chain_tail = &robustness2_features;
            chain_tail = &robustness2_features.pNext;
        }
        if (wl_dynamic_rendering_local_read)
        {
            *chain_tail = &local_read_features;
            chain_tail = &local_read_features.pNext;
        }
        if (enable_swapchain_maint1)
        {
            *chain_tail = &swapchain_maint1_features;
            chain_tail = &swapchain_maint1_features.pNext;
        }

        // External-memory/semaphore interop (CUDA<->Vulkan zero-copy direct-display).
        // On a 1.3 device the base external_memory/semaphore + properties2 are CORE; only
        // the platform handle-export extensions need explicit enabling: Win32 handle on
        // Windows, opaque fd on POSIX. Conditional: absent (non-NV / unsupported driver)
        // -> interop stays off and callers fall back to host upload.
        bool has_external_interop = false;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        has_external_interop = has_device_ext(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) &&
                               has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
        has_external_interop = has_device_ext(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
                               has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif

        // Build logical device with all needed queues
        // Vulkan requires unique queue family indices in create infos,
        // so we need to collect unique families and request appropriate counts
        auto builder = lux::gapi::vk::LogicalDevice::Builder()
                           .addExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)
                           .addExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        if (has_robustness2)
            builder.addExtension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        if (wl_dynamic_rendering_local_read)
            builder.addExtension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        if (enable_swapchain_maint1)
            builder.addExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        if (has_external_interop)
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            builder.addExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
                .addExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
            builder.addExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)
                .addExtension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
        }
        builder.setEnabledFeatures(&device_features)
            .setNextChain(&vulkan12_features) // Head of the feature chain
            .addQueueCreateInfo(graphics_queue_family_index_, 1, queue_priority);

        // Request async compute queue if it's a different family
        if (has_async_compute_ && async_compute_queue_family_index_ != graphics_queue_family_index_)
        {
            builder.addQueueCreateInfo(async_compute_queue_family_index_, 1, queue_priority);
        }

        // Request transfer queue if it's a different family (and different from compute)
        if (has_transfer_ && transfer_queue_family_index_ != graphics_queue_family_index_ &&
            transfer_queue_family_index_ != async_compute_queue_family_index_)
        {
            builder.addQueueCreateInfo(transfer_queue_family_index_, 1, queue_priority);
        }

        logical_device_ = builder.build(physical_device_, instance_context_.allocator());

        if (!logical_device_)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        // Record the interop gate: the platform external extensions are now enabled on
        // the device (or were unavailable). Consumers query supportsExternalMemory().
        supports_external_interop_ = has_external_interop;

        graphics_queue_ = logical_device_.getQueue(graphics_queue_family_index_, 0);

        // Retrieve async compute queue
        if (has_async_compute_)
        {
            async_compute_queue_ = logical_device_.getQueue(async_compute_queue_family_index_, 0);
        }

        // Retrieve transfer queue
        if (has_transfer_)
        {
            transfer_queue_ = logical_device_.getQueue(transfer_queue_family_index_, 0);
        }

        // ── DeviceCaps snapshot (mobile-adaptation topic ①, item 1-1) ──────
        // Record what this device actually enabled + the limits our pipeline
        // architecture depends on. Today the gate above hard-requires all of
        // these, so the booleans mirror the enable structs verbatim; when the
        // gate becomes a whitelist (item 1-2) this block is where optional
        // results land.
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physical_device_, &props);

            caps_.synchronization2 = vulkan13_features.synchronization2 == VK_TRUE;
            caps_.dynamic_rendering = vulkan13_features.dynamicRendering == VK_TRUE;
            caps_.descriptor_indexing = vulkan12_features.runtimeDescriptorArray == VK_TRUE &&
                                        vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE &&
                                        vulkan12_features.descriptorBindingVariableDescriptorCount == VK_TRUE &&
                                        vulkan12_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
            caps_.storage_buffer_uab = vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE;
            caps_.uniform_buffer_uab = vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
            caps_.draw_indirect_count = vulkan12_features.drawIndirectCount == VK_TRUE;
            caps_.shader_output_layer = vulkan12_features.shaderOutputLayer == VK_TRUE;
            caps_.buffer_device_address = vulkan12_features.bufferDeviceAddress == VK_TRUE;
            caps_.timeline_semaphore = vulkan12_features.timelineSemaphore == VK_TRUE;
            caps_.shader_draw_parameters = vulkan11_features.shaderDrawParameters == VK_TRUE;
            caps_.shader_int64 = device_features.shaderInt64 == VK_TRUE;
            caps_.sampler_anisotropy = device_features.samplerAnisotropy == VK_TRUE;
            caps_.multi_draw_indirect = device_features.multiDrawIndirect == VK_TRUE;
            caps_.draw_indirect_first_instance = device_features.drawIndirectFirstInstance == VK_TRUE;
            caps_.wide_lines = device_features.wideLines == VK_TRUE;
            caps_.shader_clip_distance = device_features.shaderClipDistance == VK_TRUE;
            caps_.null_descriptor = has_robustness2;
            caps_.external_memory_interop = has_external_interop;
            caps_.dynamic_rendering_local_read = wl_dynamic_rendering_local_read == VK_TRUE;

            caps_.max_bound_descriptor_sets = props.limits.maxBoundDescriptorSets;
            caps_.max_per_stage_storage_buffers = props.limits.maxPerStageDescriptorStorageBuffers;
            caps_.max_per_stage_sampled_images = props.limits.maxPerStageDescriptorSampledImages;
            caps_.max_push_constants_size = props.limits.maxPushConstantsSize;
            caps_.max_image_dimension_2d = props.limits.maxImageDimension2D;
            caps_.max_image_array_layers = props.limits.maxImageArrayLayers;
            caps_.max_storage_buffer_range = props.limits.maxStorageBufferRange;
            caps_.max_color_attachments = props.limits.maxColorAttachments;

            caps_.has_async_compute = has_async_compute_;
            caps_.has_dedicated_transfer = has_transfer_;

            // Merged pipeline layouts are exactly 4 sets (the Mali floor); a
            // device below that cannot create ANY of our pipeline layouts, so
            // fail init loudly instead of dying later at layout creation
            // (closes the "maxBoundDescriptorSets never queried" gap).
            if (caps_.max_bound_descriptor_sets < 4)
                return renderFailure<err::memory::GpuAllocationFailed>();
        }

        // Create VMA allocator for memory management.
        // BUFFER_DEVICE_ADDRESS_BIT: required so VMA-managed buffers created with
        // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT can be queried for their GPU
        // address (vkGetBufferDeviceAddress). Pairs with the device feature above.
        //
        // ⚠️ **故意不传 VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT。**
        // 这是一条承重的隐式依赖,此前只存在于"没人加过那个 flag"这个事实里:
        // 单一 transfer 线程会并发调 vmaCreateBuffer / vmaDestroyBuffer
        // (staging 缓冲的分配与回收),而渲染线程同时也在分配自己的资源。
        // 不传这个 flag ⇒ **VMA 自己内部加锁**,上面那个并发是安全的。
        // 谁哪天为了省掉那把内部锁把它加上,transfer 与渲染线程立刻变数据竞争 ——
        // 而症状会是随机的堆损坏,不是一句报错。这该写下来,不该靠考古发现。
        VmaAllocatorCreateInfo vma_info{
            // BDA flag only when the device actually enabled the feature (①-2
            // whitelist) — passing it without the feature is a VMA usage error.
            .flags = wl_buffer_device_address ? VmaAllocatorCreateFlags{VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT}
                                              : VmaAllocatorCreateFlags{0},
            .physicalDevice = physical_device_,
            .device = logical_device_,
            .pAllocationCallbacks = instance_context_.allocator(),
            .instance = instance_context_.instance(),
            .vulkanApiVersion = VK_API_VERSION_1_3, // S-06: match actual API level used
        };

        if (vmaCreateAllocator(&vma_info, &vma_allocator_) != VK_SUCCESS)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        return {};
    }

    VkResult DeviceContext::waitIdle() noexcept
    {
        std::unique_lock graphics_lock(graphics_queue_mutex_);
        const VkQueue graphics = graphicsQueue().handle();

        std::unique_lock<std::mutex> compute_lock(async_compute_queue_mutex_, std::defer_lock);
        const VkQueue compute = asyncComputeQueue().handle();
        if (compute != graphics)
            compute_lock.lock();

        std::unique_lock<std::mutex> transfer_lock(transfer_queue_mutex_, std::defer_lock);
        const VkQueue transfer = transferQueue().handle();
        if (transfer != graphics && transfer != compute)
            transfer_lock.lock();

        return vkDeviceWaitIdle(logical_device_.handle());
    }

    DeviceContext::~DeviceContext()
    {
        // Destroy VMA allocator
        if (vma_allocator_)
        {
            // ── Leak diagnostic (self-gating) ────────────────────────────────
            // DeviceContext is destroyed LAST (it is created first in the render
            // server's Impl, so reverse-order member destruction frees it after
            // every render resource). Therefore ANY VMA allocation still live at
            // this point is a genuine leak — a buffer/image whose owner forgot to
            // free it before teardown. Only dumps when something actually leaked,
            // so it is silent on a clean exit. The detailed map names each live
            // allocation (render-graph buffers carry names like "ClusterParams"),
            // which pinpoints the leaking owner without guessing.
            VmaTotalStatistics vma_stats{};
            vmaCalculateStatistics(vma_allocator_, &vma_stats);
            if (vma_stats.total.statistics.allocationCount > 0)
            {
                renderFatal("VMA allocations remain live at DeviceContext teardown");
            }

            vmaDestroyAllocator(vma_allocator_);
            vma_allocator_ = nullptr;
        }

        // Release logical device
        logical_device_.release(instance_context_.allocator());
    }

    // ResourceContext implementation
    ResourceContext::ResourceContext(DeviceContext& device_context) : device_context_(device_context)
    {
    }

    Expected<void> ResourceContext::init(const DescriptorPoolConfig& pool_config)
    {
        // Create descriptor pool
        descriptor_pool_ =
            lux::gapi::vk::DescriptorPool::Builder()
                .setFlags(
                    VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                )
                .addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLER, pool_config.sampler)
                .addPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pool_config.combined_image_sampler)
                .addPoolSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pool_config.sampled_image)
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pool_config.storage_image)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, pool_config.uniform_texel_buffer)
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, pool_config.storage_texel_buffer)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pool_config.uniform_buffer)
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pool_config.storage_buffer)
                .addPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, pool_config.uniform_buffer_dynamic)
                .addPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, pool_config.storage_buffer_dynamic)
                .addPoolSize(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, pool_config.input_attachment)
                .setMaxSets(pool_config.max_sets)
                .build(device_context_.logicalDevice());

        if (!descriptor_pool_)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        // Create command pool (for graphics queue)
        command_pool_ = lux::gapi::vk::CommandPool::Builder()
                            .setQueueFamilyIndex(device_context_.graphicsQueueFamilyIndex())
                            .build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

        // Create compute command pool (for async compute queue)
        compute_command_pool_ =
            lux::gapi::vk::CommandPool::Builder()
                .setQueueFamilyIndex(device_context_.asyncComputeQueueFamilyIndex())
                .build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

        // Create transfer command pool (for dedicated transfer queue)
        transfer_command_pool_ =
            lux::gapi::vk::CommandPool::Builder()
                .setQueueFamilyIndex(device_context_.transferQueueFamilyIndex())
                .build(device_context_.logicalDevice(), device_context_.instanceContext().allocator());

        return {};
    }

    ResourceContext::~ResourceContext()
    {
        // Release resources in reverse creation order
        transfer_command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
        compute_command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
        command_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
        descriptor_pool_.release(device_context_.logicalDevice(), device_context_.instanceContext().allocator());
    }

    VkBool32 debug_report_callback(
        VkDebugReportFlagsEXT flags,
        VkDebugReportObjectTypeEXT objectType,
        uint64_t object,
        size_t location,
        int32_t messageCode,
        const char* pLayerPrefix,
        const char* pMessage,
        void* pUserData
    )
    {
        DebugCallbackInfo info{
            .flags = flags,
            .objectType = objectType,
            .object = object,
            .location = location,
            .messageCode = messageCode,
            .layerPrefix = pLayerPrefix,
            .message = pMessage};

        auto callback = reinterpret_cast<DebugCallback*>(pUserData);
        return (*callback)(info) ? VK_TRUE : VK_FALSE;
    }
}
