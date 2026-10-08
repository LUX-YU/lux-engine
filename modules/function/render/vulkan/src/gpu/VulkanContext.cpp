#include "lux/engine/render/gpu/VulkanContext.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <string_view>
#include <vector>
#include <vk_mem_alloc.h>

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

    InstanceContext::InstanceContext(
        std::unique_ptr<DebugCallback> callback,
        lux::gapi::vk::Instance instance,
        lux::gapi::vk::DebugReport report,
        VkAllocationCallbacks* allocator,
        std::vector<std::string> extensions
    ) noexcept
        : debug_callback_(std::move(callback)), instance_(std::move(instance)), debug_report_(std::move(report)),
          allocator_(allocator), enabled_extensions_(std::move(extensions))
    {
    }

    InstanceContext::CreateResult InstanceContext::create(
        const std::vector<const char*>& required_extensions,
        DebugCallback debug_callback,
        VkAllocationCallbacks* allocator
    ) noexcept
    {
        using lux::gapi::vk::DebugReport;
        using lux::gapi::vk::Instance;
        std::vector<std::string> extensions;
        auto add_extension = [&](const char* name)
        {
            if (std::ranges::find(extensions, name) == extensions.end())
            {
                extensions.emplace_back(name);
            }
        };
        for (const char* name : required_extensions)
        {
            const bool is_invalid_name = !name || !*name;
            if (is_invalid_name)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }
            add_extension(name);
        }
        // DeviceContext supports swapchains before the first native window exists.
        add_extension(VK_KHR_SURFACE_EXTENSION_NAME);
        auto available = Instance::extensionProperties();
        if (!available)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(available.error()));
        }
        auto has_extension = [&](const char* name)
        {
            return std::ranges::any_of(
                *available,
                [name](const auto& entry) { return std::string_view(entry.extensionName) == name; }
            );
        };
        const bool has_maintenance = has_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                                     has_extension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        if (has_maintenance)
        {
            add_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            add_extension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }
        if (has_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        {
            add_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }

        std::vector<const char*> layers;
        if (debug_callback)
        {
            auto available_layers = Instance::layerProperties();
            if (!available_layers)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(available_layers.error()));
            }
            const bool has_validation = std::ranges::any_of(
                *available_layers,
                [](const auto& entry) { return std::string_view(entry.layerName) == "VK_LAYER_KHRONOS_validation"; }
            );
            // Missing optional validation still permits release/mobile startup.
            if (has_validation)
            {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                add_extension(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
                add_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
        }
        std::vector<const char*> extension_names;
        extension_names.reserve(extensions.size());
        for (const auto& name : extensions)
        {
            extension_names.push_back(name.c_str());
        }
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Default";
        application.applicationVersion = VK_MAKE_VERSION(1, 3, 0);
        application.pEngineName = "Default";
        application.engineVersion = VK_MAKE_VERSION(1, 3, 0);
        application.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &application;
        info.enabledLayerCount = static_cast<uint32_t>(layers.size());
        info.ppEnabledLayerNames = layers.data();
        info.enabledExtensionCount = static_cast<uint32_t>(extension_names.size());
        info.ppEnabledExtensionNames = extension_names.data();

        // Native callbacks can execute during creation and destruction. The storage
        // precedes both native owners and never moves when the complete context adopts it.
        auto callback = debug_callback ? std::make_unique<DebugCallback>(std::move(debug_callback)) : nullptr;
        auto instance = Instance::create(info, allocator);
        if (!instance)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(instance.error()));
        }
        DebugReport report;
        const bool has_debug_report =
            callback && std::ranges::find(extensions, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) != extensions.end();
        if (has_debug_report)
        {
            VkDebugReportCallbackCreateInfoEXT report_info{};
            report_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
            report_info.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
            report_info.pfnCallback = &debug_report_callback;
            report_info.pUserData = callback.get();
            auto created = DebugReport::create(instance->handle(), report_info, allocator);
            if (!created)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(created.error()));
            }
            report = std::move(*created);
        }
        return std::unique_ptr<InstanceContext>(new InstanceContext(
            std::move(callback),
            std::move(*instance),
            std::move(report),
            allocator,
            std::move(extensions)
        ));
    }

    bool InstanceContext::isInstanceExtensionEnabled(const char* name) const noexcept
    {
        return std::ranges::find(enabled_extensions_, name) != enabled_extensions_.end();
    }

    // DeviceContext implementation
    DeviceContext::DeviceContext(InstanceContext& instance_context, Backing backing) noexcept
        : instance_context_(instance_context), backing_(std::move(backing))
    {
    }

    DeviceContext::CreateResult DeviceContext::create(
        InstanceContext& instance_context,
        EPhysicalDeviceSelectionPolicy policy
    ) noexcept
    {
        const bool is_invalid_policy = policy != EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED &&
                                       policy != EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED;
        if (is_invalid_policy)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        Backing backing;
        // Select physical device
        auto discovered = instance_context.instance().listPhysicalDevices();
        if (!discovered)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(discovered.error()));
        }
        auto& devices = *discovered;
        if (devices.empty())
        {
            return renderFailure<err::memory::GpuAllocationFailed>();
        }

        switch (policy)
        {
        case EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED:
        {
            // First try to find a discrete GPU
            for (auto& device : devices)
            {
                if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
                {
                    backing.physical_device = std::move(device);
                    break;
                }
            }
            // If no discrete GPU found, try integrated GPU as fallback
            if (!backing.physical_device)
            {
                for (auto& device : devices)
                {
                    if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
                    {
                        backing.physical_device = std::move(device);
                        break;
                    }
                }
            }
            break;
        }
        case EPhysicalDeviceSelectionPolicy::INTEGRATED_GPU_PREFERRED:
        {
            // First try to find an integrated GPU
            for (auto& device : devices)
            {
                if (device.type() == lux::gapi::EDeviceType::INTEGRATED_GPU)
                {
                    backing.physical_device = std::move(device);
                    break;
                }
            }
            // If no integrated GPU found, try discrete GPU as fallback
            if (!backing.physical_device)
            {
                for (auto& device : devices)
                {
                    if (device.type() == lux::gapi::EDeviceType::DISCRETE_GPU)
                    {
                        backing.physical_device = std::move(device);
                        break;
                    }
                }
            }
            break;
        }
        }

        // Final fallback: use the first available device if we haven't found a suitable one
        if (!backing.physical_device && !devices.empty())
        {
            backing.physical_device = std::move(devices.front());
        }

        if (!backing.physical_device)
        {
            return renderFailure<err::memory::GpuAllocationFailed>();
        }

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

            vkGetPhysicalDeviceFeatures2(backing.physical_device, &feat2);

            std::string missing;
            auto require = [&](VkBool32 supported, const char* name)
            {
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
            {
                return renderFailure<err::memory::GpuAllocationFailed>();
            }

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

        backing.graphics_queue_family_index =
            backing.physical_device.findQueueFamilyIndexByFlags(VK_QUEUE_GRAPHICS_BIT);

        if (backing.graphics_queue_family_index == INVALID_QUEUE_FAMILY_INDEX)
        {
            return renderFailure<err::memory::GpuAllocationFailed>();
        }

        // Discover dedicated async compute queue (COMPUTE but NOT GRAPHICS)
        {
            const auto& families = backing.physical_device.queueFamilyProperties();
            for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
            {
                const auto& props = families[i].queueFamilyProperties;
                if ((props.queueFlags & VK_QUEUE_COMPUTE_BIT) && !(props.queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    props.queueCount > 0)
                {
                    backing.async_compute_queue_family_index = i;
                    backing.has_async_compute = true;
                    break;
                }
            }
        }

        // Discover dedicated transfer queue (TRANSFER but NOT GRAPHICS and NOT COMPUTE)
        {
            const auto& families = backing.physical_device.queueFamilyProperties();
            for (uint32_t i = 0; i < static_cast<uint32_t>(families.size()); ++i)
            {
                const auto& props = families[i].queueFamilyProperties;
                const bool has_transfer_queue = (props.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
                const bool has_graphics_queue = (props.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                const bool has_compute_queue = (props.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                const bool has_queues = props.queueCount > 0;
                const bool is_dedicated_transfer =
                    has_transfer_queue && !has_graphics_queue && !has_compute_queue && has_queues;
                if (is_dedicated_transfer)
                {
                    backing.transfer_queue_family_index = i;
                    backing.has_transfer = true;
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
        auto extensions = lux::gapi::vk::detail::enumerateVulkanValues<VkExtensionProperties>(
            [&](uint32_t* count, VkExtensionProperties* values) noexcept
            { return vkEnumerateDeviceExtensionProperties(backing.physical_device, nullptr, count, values); }
        );
        if (!extensions)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(extensions.error()));
        }
        const auto& device_exts = *extensions;
        auto has_device_ext = [&](const char* name)
        {
            for (const auto& e : device_exts)
            {
                if (std::string_view(e.extensionName) == name)
                {
                    return true;
                }
            }
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
            instance_context.isInstanceExtensionEnabled(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
            has_device_ext(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        if (enable_swapchain_maint1)
        {
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT q{};
            q.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 q2{};
            q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            q2.pNext = &q;
            vkGetPhysicalDeviceFeatures2(backing.physical_device, &q2);
            enable_swapchain_maint1 = (q.swapchainMaintenance1 == VK_TRUE);
        }
        backing.supports_swapchain_maintenance1 = enable_swapchain_maint1;
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
        std::vector<const char*> enabled_extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME
        };
        auto add_extension = [&](const char* name) { enabled_extensions.push_back(name); };
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (has_device_ext(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        {
            add_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
#endif
        if (has_device_ext(VK_EXT_HDR_METADATA_EXTENSION_NAME))
        {
            add_extension(VK_EXT_HDR_METADATA_EXTENSION_NAME);
        }
        // synchronization2 is core in our required Vulkan 1.3; no redundant
        // extension alias or second extension enumeration is necessary.
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        auto add_queue = [&](uint32_t family, uint32_t count, const float* priorities)
        {
            VkDeviceQueueCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            info.queueFamilyIndex = family;
            info.queueCount = count;
            info.pQueuePriorities = priorities;
            queue_infos.push_back(info);
        };
        if (has_robustness2)
        {
            add_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        }
        if (wl_dynamic_rendering_local_read)
        {
            add_extension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
        }
        if (enable_swapchain_maint1)
        {
            add_extension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }
        if (has_external_interop)
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            add_extension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
            add_extension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
#else
            add_extension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            add_extension(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
        }
        add_queue(backing.graphics_queue_family_index, 1, queue_priority);

        // Request async compute queue if it's a different family
        const bool has_separate_compute = backing.has_async_compute && backing.async_compute_queue_family_index !=
                                                                           backing.graphics_queue_family_index;
        if (has_separate_compute)
        {
            add_queue(backing.async_compute_queue_family_index, 1, queue_priority);
        }

        // Request transfer queue if it's a different family (and different from compute)
        const bool has_separate_transfer =
            backing.has_transfer && backing.transfer_queue_family_index != backing.graphics_queue_family_index &&
            backing.transfer_queue_family_index != backing.async_compute_queue_family_index;
        if (has_separate_transfer)
        {
            add_queue(backing.transfer_queue_family_index, 1, queue_priority);
        }

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &vulkan12_features;
        device_info.pEnabledFeatures = &device_features;
        device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
        device_info.pQueueCreateInfos = queue_infos.data();
        device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
        device_info.ppEnabledExtensionNames = enabled_extensions.data();
        auto logical_device =
            lux::gapi::vk::LogicalDevice::create(backing.physical_device, device_info, instance_context.allocator());
        if (!logical_device)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(logical_device.error()));
        }
        backing.logical_device = std::move(*logical_device);

        // Record the interop gate: the platform external extensions are now enabled on
        // the device (or were unavailable). Consumers query supportsExternalMemory().
        backing.supports_external_interop = has_external_interop;

        backing.graphics_queue = backing.logical_device.getQueue(backing.graphics_queue_family_index, 0);

        // Retrieve async compute queue
        if (backing.has_async_compute)
        {
            backing.async_compute_queue = backing.logical_device.getQueue(backing.async_compute_queue_family_index, 0);
        }

        // Retrieve transfer queue
        if (backing.has_transfer)
        {
            backing.transfer_queue = backing.logical_device.getQueue(backing.transfer_queue_family_index, 0);
        }

        // ── DeviceCaps snapshot (mobile-adaptation topic ①, item 1-1) ──────
        // Record what this device actually enabled + the limits our pipeline
        // architecture depends on. Today the gate above hard-requires all of
        // these, so the booleans mirror the enable structs verbatim; when the
        // gate becomes a whitelist (item 1-2) this block is where optional
        // results land.
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(backing.physical_device, &props);

            backing.caps.synchronization2 = vulkan13_features.synchronization2 == VK_TRUE;
            backing.caps.dynamic_rendering = vulkan13_features.dynamicRendering == VK_TRUE;
            backing.caps.descriptor_indexing =
                vulkan12_features.runtimeDescriptorArray == VK_TRUE &&
                vulkan12_features.descriptorBindingPartiallyBound == VK_TRUE &&
                vulkan12_features.descriptorBindingVariableDescriptorCount == VK_TRUE &&
                vulkan12_features.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
            backing.caps.storage_buffer_uab =
                vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE;
            backing.caps.uniform_buffer_uab =
                vulkan12_features.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
            backing.caps.draw_indirect_count = vulkan12_features.drawIndirectCount == VK_TRUE;
            backing.caps.shader_output_layer = vulkan12_features.shaderOutputLayer == VK_TRUE;
            backing.caps.buffer_device_address = vulkan12_features.bufferDeviceAddress == VK_TRUE;
            backing.caps.timeline_semaphore = vulkan12_features.timelineSemaphore == VK_TRUE;
            backing.caps.shader_draw_parameters = vulkan11_features.shaderDrawParameters == VK_TRUE;
            backing.caps.shader_int64 = device_features.shaderInt64 == VK_TRUE;
            backing.caps.sampler_anisotropy = device_features.samplerAnisotropy == VK_TRUE;
            backing.caps.multi_draw_indirect = device_features.multiDrawIndirect == VK_TRUE;
            backing.caps.draw_indirect_first_instance = device_features.drawIndirectFirstInstance == VK_TRUE;
            backing.caps.wide_lines = device_features.wideLines == VK_TRUE;
            backing.caps.shader_clip_distance = device_features.shaderClipDistance == VK_TRUE;
            backing.caps.null_descriptor = has_robustness2;
            backing.caps.external_memory_interop = has_external_interop;
            backing.caps.dynamic_rendering_local_read = wl_dynamic_rendering_local_read == VK_TRUE;

            backing.caps.max_bound_descriptor_sets = props.limits.maxBoundDescriptorSets;
            backing.caps.max_per_stage_storage_buffers = props.limits.maxPerStageDescriptorStorageBuffers;
            backing.caps.max_per_stage_sampled_images = props.limits.maxPerStageDescriptorSampledImages;
            backing.caps.max_push_constants_size = props.limits.maxPushConstantsSize;
            backing.caps.max_image_dimension_2d = props.limits.maxImageDimension2D;
            backing.caps.max_image_array_layers = props.limits.maxImageArrayLayers;
            backing.caps.max_storage_buffer_range = props.limits.maxStorageBufferRange;
            backing.caps.max_color_attachments = props.limits.maxColorAttachments;

            backing.caps.has_async_compute = backing.has_async_compute;
            backing.caps.has_dedicated_transfer = backing.has_transfer;

            // Merged pipeline layouts are exactly 4 sets (the Mali floor); a
            // device below that cannot create ANY of our pipeline layouts, so
            // fail init loudly instead of dying later at layout creation
            // (closes the "maxBoundDescriptorSets never queried" gap).
            if (backing.caps.max_bound_descriptor_sets < 4)
            {
                return renderFailure<err::memory::GpuAllocationFailed>();
            }
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
            .physicalDevice = backing.physical_device,
            .device = backing.logical_device,
            .pAllocationCallbacks = instance_context.allocator(),
            .instance = instance_context.instance(),
            .vulkanApiVersion = VK_API_VERSION_1_3, // S-06: match actual API level used
        };

        auto allocator = VmaAllocatorOwner::create(vma_info);
        if (!allocator)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(allocator.error()));
        }
        backing.vma_allocator = std::move(*allocator);

        return std::unique_ptr<DeviceContext>(new DeviceContext(instance_context, std::move(backing)));
    }

    VkResult DeviceContext::waitIdle() noexcept
    {
        std::unique_lock graphics_lock(graphics_queue_mutex_);
        const VkQueue graphics = graphicsQueue().handle();

        std::unique_lock<std::mutex> compute_lock(async_compute_queue_mutex_, std::defer_lock);
        const VkQueue compute = asyncComputeQueue().handle();
        if (compute != graphics)
        {
            compute_lock.lock();
        }

        std::unique_lock<std::mutex> transfer_lock(transfer_queue_mutex_, std::defer_lock);
        const VkQueue transfer = transferQueue().handle();
        if (transfer != graphics && transfer != compute)
        {
            transfer_lock.lock();
        }

        return vkDeviceWaitIdle(backing_.logical_device.handle());
    }

    DeviceContext::~DeviceContext()
    {
        // A published context always has VMA backing. Children must have retired
        // all allocations at the original owner-safe point; destruction does not wait.
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(backing_.vma_allocator.get(), &statistics);
        if (statistics.total.statistics.allocationCount > 0)
        {
            renderFatal("VMA allocations remain live at DeviceContext teardown");
        }
        // Backing member order releases VMA before its owning logical device.
    }

    // ResourceContext implementation
    ResourceContext::ResourceContext(DeviceContext& device_context, Pools pools) noexcept
        : device_context_(device_context), pools_(std::move(pools))
    {
    }

    ResourceContext::CreateResult ResourceContext::create(
        DeviceContext& device_context,
        const DescriptorPoolConfig& pool_config
    ) noexcept
    {
        const VkDevice device = device_context.logicalDevice();
        const auto* allocator = device_context.instanceContext().allocator();
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_SAMPLER, pool_config.sampler},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, pool_config.combined_image_sampler},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, pool_config.sampled_image},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pool_config.storage_image},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, pool_config.uniform_texel_buffer},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, pool_config.storage_texel_buffer},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, pool_config.uniform_buffer},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, pool_config.storage_buffer},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, pool_config.uniform_buffer_dynamic},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, pool_config.storage_buffer_dynamic},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, pool_config.input_attachment}
        };
        const VkDescriptorPoolCreateInfo descriptor_info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = pool_config.max_sets,
            .poolSizeCount = static_cast<std::uint32_t>(std::size(sizes)),
            .pPoolSizes = sizes
        };
        auto descriptors = DescriptorPoolOwner::create(device, descriptor_info, allocator);
        if (!descriptors)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(descriptors.error()));
        }

        Pools pools{.descriptors = std::move(*descriptors)};
        const std::uint32_t queue_families[]{
            device_context.graphicsQueueFamilyIndex(),
            device_context.asyncComputeQueueFamilyIndex(),
            device_context.transferQueueFamilyIndex()
        };
        CommandPoolOwner* command_pools[]{&pools.graphics, &pools.compute, &pools.transfer};
        for (std::size_t i = 0; i < std::size(queue_families); ++i)
        {
            const VkCommandPoolCreateInfo command_info{
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = queue_families[i]
            };
            auto command_pool = CommandPoolOwner::create(device, command_info, allocator);
            if (!command_pool)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(command_pool.error()));
            }
            *command_pools[i] = std::move(*command_pool);
        }
        return std::unique_ptr<ResourceContext>(new ResourceContext(device_context, std::move(pools)));
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
            .message = pMessage
        };

        auto callback = reinterpret_cast<DebugCallback*>(pUserData);
        return (*callback)(info) ? VK_TRUE : VK_FALSE;
    }
} // namespace lux::render
