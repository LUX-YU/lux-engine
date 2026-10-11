#include <lux/engine/render/vulkan/device/Device.hpp>

#include "Native.hpp"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    namespace
    {
        template <typename T, typename F> RenderResult<std::vector<T>> enumerate(F&& function) noexcept
        {
            for (unsigned attempt = 0; attempt < 4; ++attempt)
            {
                std::uint32_t count{};
                auto result = function(&count, static_cast<T*>(nullptr));
                if (result != VK_SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                std::vector<T> values(count);
                result = function(&count, values.data());
                if (result == VK_SUCCESS)
                {
                    values.resize(count);
                    return values;
                }
                if (result != VK_INCOMPLETE)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
            return cxx::unexpected(nativeError(VK_INCOMPLETE));
        }

        bool hasExtension(std::span<const VkExtensionProperties> properties, const char* name) noexcept
        {
            return std::any_of(
                properties.begin(),
                properties.end(),
                [name](const auto& property) { return std::strcmp(name, property.extensionName) == 0; }
            );
        }

        RenderResult<std::vector<const char*>> extensionNames(std::span<const char* const> requested) noexcept
        {
            std::vector<const char*> names;
            for (auto name : requested)
            {
                if (!name || !*name)
                {
                    return cxx::unexpected(RenderError{kInvalidArgument});
                }
                const bool is_duplicate = std::any_of(
                    names.begin(),
                    names.end(),
                    [name](const char* previous) { return std::strcmp(name, previous) == 0; }
                );
                if (!is_duplicate)
                {
                    names.push_back(name);
                }
            }
            return names;
        }
    } // namespace

    VulkanInstance::VulkanInstance(VkInstance instance, VkDebugUtilsMessengerEXT messenger) noexcept
        : instance_(instance), messenger_(messenger)
    {
    }

    RenderResult<VulkanInstance> VulkanInstance::create(const InstanceOptions& options) noexcept
    {
        const bool is_invalid_diagnostic = options.validation && !options.diagnostic;
        if (is_invalid_diagnostic)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::uint32_t version{};
        auto result = vkEnumerateInstanceVersion(&version);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        if (version < VK_API_VERSION_1_3)
        {
            return cxx::unexpected(RenderError{kUnsupported, {VK_API_VERSION_1_3}});
        }
        auto requested = extensionNames(options.extensions);
        if (!requested)
        {
            return cxx::unexpected(requested.error());
        }
        auto extensions = enumerate<VkExtensionProperties>(
            [](auto* count, auto* values) { return vkEnumerateInstanceExtensionProperties(nullptr, count, values); }
        );
        if (!extensions)
        {
            return cxx::unexpected(extensions.error());
        }
        const char* layer = "VK_LAYER_KHRONOS_validation";
        if (options.validation)
        {
            auto layers = enumerate<VkLayerProperties>([](auto* count, auto* values)
                                                       { return vkEnumerateInstanceLayerProperties(count, values); });
            if (!layers)
            {
                return cxx::unexpected(layers.error());
            }
            const bool has_validation = std::any_of(
                layers->begin(),
                layers->end(),
                [layer](const auto& available) { return std::strcmp(available.layerName, layer) == 0; }
            );
            if (!has_validation)
            {
                return cxx::unexpected(
                    RenderError{kUnsupported, {static_cast<std::uint32_t>(VK_ERROR_LAYER_NOT_PRESENT)}}
                );
            }
            auto layer_extensions = enumerate<VkExtensionProperties>(
                [layer](auto* count, auto* values)
                { return vkEnumerateInstanceExtensionProperties(layer, count, values); }
            );
            if (!layer_extensions)
            {
                return cxx::unexpected(layer_extensions.error());
            }
            extensions->insert(extensions->end(), layer_extensions->begin(), layer_extensions->end());
            requested->push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            requested->push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            requested = extensionNames(*requested);
        }
        for (auto name : *requested)
        {
            if (!hasExtension(*extensions, name))
            {
                return cxx::unexpected(
                    RenderError{kUnsupported, {static_cast<std::uint32_t>(VK_ERROR_EXTENSION_NOT_PRESENT)}}
                );
            }
        }
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "Lux Render Vulkan Foundation";
        application.apiVersion = VK_API_VERSION_1_3;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = options.diagnostic;
        debug.pUserData = options.diagnostic_user;
        const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation.enabledValidationFeatureCount = 1;
        validation.pEnabledValidationFeatures = &sync;
        validation.pNext = &debug;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = static_cast<std::uint32_t>(requested->size());
        info.ppEnabledExtensionNames = requested->data();
        info.enabledLayerCount = options.validation ? 1 : 0;
        info.ppEnabledLayerNames = options.validation ? &layer : nullptr;
        info.pNext = options.validation ? &validation : nullptr;
        VkInstance instance{};
        result = LUX_NATIVE("instance", vkCreateInstance(&info, nullptr, &instance));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        VulkanInstance owner(instance, VK_NULL_HANDLE);
        if (options.validation)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")
            );
            if (!create)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            result = LUX_NATIVE("messenger", create(instance, &debug, nullptr, &owner.messenger_));
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        return owner;
    }

    void VulkanInstance::release() noexcept
    {
        if (messenger_)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT")
            );
            LUX_DESTROY("messenger", destroy(instance_, messenger_, nullptr));
        }
        if (instance_)
        {
            LUX_DESTROY("instance", vkDestroyInstance(instance_, nullptr));
        }
    }

    VulkanInstance::~VulkanInstance() noexcept
    {
        release();
    }

    VulkanInstance::VulkanInstance(VulkanInstance&& other) noexcept
        : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
          messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE))
    {
    }

    VulkanInstance& VulkanInstance::operator=(VulkanInstance&& other) noexcept
    {
        if (this != &other)
        {
            release();
            instance_ = std::exchange(other.instance_, VK_NULL_HANDLE);
            messenger_ = std::exchange(other.messenger_, VK_NULL_HANDLE);
        }
        return *this;
    }

    RenderResult<std::uint32_t> selectQueueFamily(std::span<const VkQueueFamilyProperties> families) noexcept
    {
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        for (std::uint32_t index = 0; index < families.size(); ++index)
        {
            const bool is_eligible =
                families[index].queueCount > 0 && (families[index].queueFlags & required) == required;
            if (is_eligible)
            {
                return index;
            }
        }
        return cxx::unexpected(RenderError{kUnsupported});
    }

    RenderResult<std::array<QueueLocation, 3>> selectQueues(
        std::span<const VkQueueFamilyProperties> families,
        bool multiple,
        bool prefer_dedicated
    ) noexcept
    {
        auto graphics = selectQueueFamily(families);
        if (!graphics)
        {
            return cxx::unexpected(graphics.error());
        }
        std::array<QueueLocation, 3> result{{{*graphics, 0}, {*graphics, 0}, {*graphics, 0}}};
        if (!multiple)
        {
            return result;
        }
        // Prefer dedicated families; otherwise use distinct indices when available.
        for (std::uint32_t role = 1; role < result.size(); ++role)
        {
            const auto required = role == 1 ? VK_QUEUE_COMPUTE_BIT : VK_QUEUE_TRANSFER_BIT;
            int best = -1;
            for (std::uint32_t f = 0; f < families.size(); ++f)
            {
                const auto flags = families[f].queueFlags;
                const bool supports = (flags & required) != 0 ||
                                      (role == 2 && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0);
                if (families[f].queueCount == 0 || !supports || (!prefer_dedicated && f != *graphics))
                {
                    continue;
                }
                const int score = role == 1 ? ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 ? 2 : 0)
                                            : ((flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0 ? 3
                                               : (flags & VK_QUEUE_GRAPHICS_BIT) == 0                        ? 2
                                                                                                             : 0);
                if (score > best)
                {
                    best = score;
                    result[role] = {f, 0};
                }
            }
            auto& selected = result[role];
            for (std::uint32_t previous = 0; previous < role; ++previous)
            {
                if (result[previous].family == selected.family)
                {
                    selected.index = std::max(selected.index, result[previous].index + 1);
                }
            }
            if (selected.index >= families[selected.family].queueCount)
            {
                selected.index = 0;
            }
        }
        return result;
    }

    VulkanDevice::VulkanDevice(
        VkInstance instance,
        VkPhysicalDevice physical,
        VkDevice device,
        std::array<NativeQueue, 3> queues,
        const DeviceOptions& options
    ) noexcept
        : instance_(instance), physical_(physical), device_(device), queue_(queues[0].handle),
          queue_family_(queues[0].family), dynamic_rendering_(options.dynamic_rendering), queues_(queues),
          multiview_(options.multiview), local_read_(options.local_read), timeline_(options.multiple_queues),
          separate_depth_stencil_(options.separate_depth_stencil)
    {
        vkGetPhysicalDeviceProperties(physical, &properties_);
    }

    RenderResult<VulkanDevice> VulkanDevice::create(
        const VulkanInstance& instance,
        const DeviceOptions& options
    ) noexcept
    {
        if (options.local_read && !options.dynamic_rendering)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto requested = extensionNames(options.extensions);
        if (!requested)
        {
            return cxx::unexpected(requested.error());
        }
        if (options.local_read)
        {
            requested->push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
            requested = extensionNames(*requested);
        }
        auto devices =
            enumerate<VkPhysicalDevice>([&](auto* count, auto* values)
                                        { return vkEnumeratePhysicalDevices(instance.native(), count, values); });
        if (!devices)
        {
            return cxx::unexpected(devices.error());
        }
        const auto automatic = std::numeric_limits<std::uint32_t>::max();
        const bool is_bad_index =
            options.physical_device_index != automatic && options.physical_device_index >= devices->size();
        if (is_bad_index)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkPhysicalDevice selected{};
        std::array<NativeQueue, 3> queues{};
        int best_score = -1;
        for (std::uint32_t index = 0; index < devices->size(); ++index)
        {
            const bool is_other_device =
                options.physical_device_index != automatic && options.physical_device_index != index;
            if (is_other_device)
            {
                continue;
            }
            const auto physical = (*devices)[index];
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR supported_local{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR
            };
            supported.pNext = &supported12;
            supported12.pNext = &supported11;
            supported11.pNext = options.local_read ? &supported_local : nullptr;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &supported;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            const bool is_unsupported = properties.apiVersion < VK_API_VERSION_1_3 || !supported.synchronization2 ||
                                        (options.dynamic_rendering && !supported.dynamicRendering) ||
                                        (options.multiple_queues && !supported12.timelineSemaphore) ||
                                        (options.multiview && !supported11.multiview) ||
                                        (options.separate_depth_stencil && !supported12.separateDepthStencilLayouts) ||
                                        (options.local_read && !supported_local.dynamicRenderingLocalRead);
            if (is_unsupported)
            {
                continue;
            }
            auto extensions = enumerate<VkExtensionProperties>(
                [physical](auto* count, auto* values)
                { return vkEnumerateDeviceExtensionProperties(physical, nullptr, count, values); }
            );
            if (!extensions)
            {
                return cxx::unexpected(extensions.error());
            }
            const bool has_required_extensions = std::all_of(
                requested->begin(),
                requested->end(),
                [&](auto name) { return hasExtension(*extensions, name); }
            );
            if (!has_required_extensions)
            {
                continue;
            }
            std::uint32_t count{};
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
            families.resize(count);
            auto eligible = selectQueues(families, options.multiple_queues, options.prefer_dedicated_queues);
            if (!eligible)
            {
                continue;
            }
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (score > best_score)
            {
                selected = physical;
                for (std::size_t q = 0; q < queues.size(); ++q)
                {
                    const auto location = (*eligible)[q];
                    queues[q] = {VK_NULL_HANDLE, location.family, location.index, families[location.family].queueFlags};
                }
                best_score = score;
            }
        }
        if (!selected)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const std::array priorities{1.0f, 1.0f, 1.0f};
        std::array<VkDeviceQueueCreateInfo, 3> queue_infos{};
        std::uint32_t queue_count = 0;
        for (const auto& queue : queues)
        {
            std::uint32_t index = 0;
            while (index < queue_count && queue_infos[index].queueFamilyIndex != queue.family)
            {
                ++index;
            }
            if (index == queue_count)
            {
                queue_infos[index] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue_infos[index].queueFamilyIndex = queue.family;
                queue_infos[index].pQueuePriorities = priorities.data();
                ++queue_count;
            }
            queue_infos[index].queueCount = std::max(queue_infos[index].queueCount, queue.index + 1);
        }
        VkPhysicalDeviceVulkan13Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        enabled.synchronization2 = VK_TRUE;
        enabled.dynamicRendering = options.dynamic_rendering;
        VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.timelineSemaphore = options.multiple_queues;
        enabled12.separateDepthStencilLayouts = options.separate_depth_stencil;
        VkPhysicalDeviceVulkan11Features enabled11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        enabled11.multiview = options.multiview;
        VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR enabled_local{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR
        };
        enabled_local.dynamicRenderingLocalRead = options.local_read;
        enabled.pNext = &enabled12;
        enabled12.pNext = &enabled11;
        enabled11.pNext = options.local_read ? &enabled_local : nullptr;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &enabled;
        info.queueCreateInfoCount = queue_count;
        info.pQueueCreateInfos = queue_infos.data();
        info.enabledExtensionCount = static_cast<std::uint32_t>(requested->size());
        info.ppEnabledExtensionNames = requested->data();
        VkDevice device{};
        const auto result = LUX_NATIVE("device", vkCreateDevice(selected, &info, nullptr, &device));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        for (auto& queue : queues)
        {
            vkGetDeviceQueue(device, queue.family, queue.index, &queue.handle);
        }
        return VulkanDevice{instance.native(), selected, device, queues, options};
    }

    void VulkanDevice::release() noexcept
    {
        // Host has completed submitted work and released children. No hidden
        // wait-idle or recovery here; this is Vulkan's ordinary parent lifetime.
        if (device_)
        {
            LUX_DESTROY("device", vkDestroyDevice(device_, nullptr));
        }
    }

    VulkanDevice::~VulkanDevice() noexcept
    {
        release();
    }

    VulkanDevice::VulkanDevice(VulkanDevice&& other) noexcept
        : instance_(other.instance_), physical_(other.physical_), device_(std::exchange(other.device_, VK_NULL_HANDLE)),
          queue_(other.queue_), queue_family_(other.queue_family_), properties_(other.properties_),
          dynamic_rendering_(other.dynamic_rendering_), queues_(other.queues_), multiview_(other.multiview_),
          local_read_(other.local_read_), timeline_(other.timeline_),
          separate_depth_stencil_(other.separate_depth_stencil_)
    {
    }

    VulkanDevice& VulkanDevice::operator=(VulkanDevice&& other) noexcept
    {
        if (this != &other)
        {
            release();
            instance_ = other.instance_;
            physical_ = other.physical_;
            device_ = std::exchange(other.device_, VK_NULL_HANDLE);
            queue_ = other.queue_;
            queue_family_ = other.queue_family_;
            properties_ = other.properties_;
            dynamic_rendering_ = other.dynamic_rendering_;
            queues_ = other.queues_;
            multiview_ = other.multiview_;
            local_read_ = other.local_read_;
            timeline_ = other.timeline_;
            separate_depth_stencil_ = other.separate_depth_stencil_;
        }
        return *this;
    }

    DeviceCaps VulkanDevice::caps() const noexcept
    {
        return {properties_.limits.maxImageDimension2D, properties_.limits.maxColorAttachments};
    }

    VulkanAllocator::VulkanAllocator(VmaAllocator_T* allocator, VkDevice device) noexcept
        : allocator_(allocator), device_(device)
    {
    }

    RenderResult<VulkanAllocator> VulkanAllocator::create(const VulkanDevice& device) noexcept
    {
        VmaAllocatorCreateInfo info{};
        info.instance = device.instance();
        info.physicalDevice = device.physical();
        info.device = device.native();
        info.vulkanApiVersion = VK_API_VERSION_1_3;
        VmaAllocator allocator{};
        const auto result = LUX_NATIVE("allocator", vmaCreateAllocator(&info, &allocator));
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return VulkanAllocator{allocator, device.native()};
    }

    void VulkanAllocator::release() noexcept
    {
        if (allocator_)
        {
            LUX_DESTROY("allocator", vmaDestroyAllocator(allocator_));
        }
    }

    VulkanAllocator::~VulkanAllocator() noexcept
    {
        release();
    }

    VulkanAllocator::VulkanAllocator(VulkanAllocator&& other) noexcept
        : allocator_(std::exchange(other.allocator_, nullptr)), device_(other.device_)
    {
    }

    VulkanAllocator& VulkanAllocator::operator=(VulkanAllocator&& other) noexcept
    {
        if (this != &other)
        {
            release();
            allocator_ = std::exchange(other.allocator_, nullptr);
            device_ = other.device_;
        }
        return *this;
    }
} // namespace lux::render::vulkan
