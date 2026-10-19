#include <lux/engine/render/native/vulkan/device/Device.hpp>

#include <algorithm>
#include <cstring>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/Error.hpp>
#include <utility>
#include <vector>
#include <vk_mem_alloc.h>

namespace lux::render::vulkan
{
    namespace
    {
        // The finite Image/Vertex formats understood by this Native compiler.
        // Query once before publication; compilation only reads the physical snapshot.
        constexpr std::array native_formats{
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_FORMAT_R8G8B8A8_SRGB,
            VK_FORMAT_B8G8R8A8_UNORM,
            VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D16_UNORM,
            VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
            VK_FORMAT_R32_SFLOAT,
            VK_FORMAT_R32G32_SFLOAT,
            VK_FORMAT_R32G32B32_SFLOAT,
            VK_FORMAT_R32G32B32A32_SFLOAT,
            VK_FORMAT_R32_UINT,
            VK_FORMAT_R32G32_UINT,
            VK_FORMAT_R32G32B32_UINT,
            VK_FORMAT_R32G32B32A32_UINT,
            VK_FORMAT_R32_SINT,
            VK_FORMAT_R32G32_SINT,
            VK_FORMAT_R32G32B32_SINT,
            VK_FORMAT_R32G32B32A32_SINT
        };

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

    InstanceContext::InstanceContext(VkInstance instance, VkDebugUtilsMessengerEXT messenger) noexcept
        : instance_(instance), messenger_(messenger)
    {
    }

    RenderResult<InstanceContext> InstanceContext::create(const InstanceOptions& options) noexcept
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
        result = vkCreateInstance(&info, nullptr, &instance);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        InstanceContext owner(instance, VK_NULL_HANDLE);
        if (options.validation)
        {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")
            );
            if (!create)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            result = create(instance, &debug, nullptr, &owner.messenger_);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
        }
        return owner;
    }

    void InstanceContext::release() noexcept
    {
        if (messenger_)
        {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT")
            );
            destroy(instance_, messenger_, nullptr);
        }
        if (instance_)
        {
            vkDestroyInstance(instance_, nullptr);
        }
    }

    InstanceContext::~InstanceContext() noexcept
    {
        release();
    }

    InstanceContext::InstanceContext(InstanceContext&& other) noexcept
        : instance_(std::exchange(other.instance_, VK_NULL_HANDLE)),
          messenger_(std::exchange(other.messenger_, VK_NULL_HANDLE))
    {
    }

    InstanceContext& InstanceContext::operator=(InstanceContext&& other) noexcept
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

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const InstanceContext& instance,
        const DeviceOptions& options,
        std::uint32_t slots
    ) noexcept
    {
        if (slots == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
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
        if (options.swapchain_maintenance)
        {
            requested->push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            requested->push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
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
        std::optional<gapi::vk::PhysicalDevice> selected;
        auto enabled_options = options;
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
            gapi::vk::PhysicalDevice snapshot((*devices)[index], native_formats);
            const auto physical = snapshot.handle();
            const auto& properties = snapshot.properties().properties;
            VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan11Features supported11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR supported_local{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR
            };
            supported.pNext = &supported12;
            supported12.pNext = &supported11;
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT supported_present{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT
            };
            supported11.pNext = options.swapchain_maintenance ? &supported_present : nullptr;
            supported_present.pNext = options.local_read ? &supported_local : nullptr;
            if (!options.swapchain_maintenance && options.local_read)
            {
                supported11.pNext = &supported_local;
            }
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &supported;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            const bool supports_texture_features =
                !(!supported12.runtimeDescriptorArray || !supported12.descriptorBindingPartiallyBound ||
                  !supported12.descriptorBindingSampledImageUpdateAfterBind ||
                  !supported12.descriptorBindingUpdateUnusedWhilePending ||
                  !supported12.shaderSampledImageArrayNonUniformIndexing ||
                  !features.features.shaderSampledImageArrayDynamicIndexing);
            const bool is_unsupported = properties.apiVersion < VK_API_VERSION_1_3 || !supported.synchronization2 ||
                                        (options.texture_descriptors && !supports_texture_features) ||
                                        (options.dynamic_rendering && !supported.dynamicRendering) ||
                                        (options.multiple_queues && !supported12.timelineSemaphore) ||
                                        (options.host_query_reset && !supported12.hostQueryReset) ||
                                        (options.buffer_device_address && !supported12.bufferDeviceAddress) ||
                                        (options.texture_compression_bc && !features.features.textureCompressionBC) ||
                                        (options.multiview && !supported11.multiview) ||
                                        (options.separate_depth_stencil && !supported12.separateDepthStencilLayouts) ||
                                        (options.local_read && !supported_local.dynamicRenderingLocalRead) ||
                                        (options.swapchain_maintenance && !supported_present.swapchainMaintenance1);
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
            std::vector<VkQueueFamilyProperties> families;
            families.reserve(snapshot.queueFamilyProperties().size());
            for (const auto& family : snapshot.queueFamilyProperties())
            {
                families.push_back(family.queueFamilyProperties);
            }
            auto eligible = selectQueues(families, options.multiple_queues, options.prefer_dedicated_queues);
            if (!eligible)
            {
                continue;
            }
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (score > best_score)
            {
                selected = std::move(snapshot);
                enabled_options.texture_descriptors =
                    options.texture_descriptors || (options.optional_texture_descriptors && supports_texture_features);
                enabled_options.texture_compression_bc =
                    options.texture_compression_bc ||
                    (options.optional_texture_compression_bc && features.features.textureCompressionBC);
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
        enabled12.hostQueryReset = options.host_query_reset;
        enabled12.bufferDeviceAddress = options.buffer_device_address;
        enabled12.separateDepthStencilLayouts = options.separate_depth_stencil;
        enabled12.runtimeDescriptorArray = enabled_options.texture_descriptors;
        enabled12.descriptorBindingPartiallyBound = enabled_options.texture_descriptors;
        enabled12.descriptorBindingSampledImageUpdateAfterBind = enabled_options.texture_descriptors;
        enabled12.descriptorBindingUpdateUnusedWhilePending = enabled_options.texture_descriptors;
        enabled12.shaderSampledImageArrayNonUniformIndexing = enabled_options.texture_descriptors;
        VkPhysicalDeviceVulkan11Features enabled11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        enabled11.multiview = options.multiview;
        VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR enabled_local{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR
        };
        enabled_local.dynamicRenderingLocalRead = options.local_read;
        enabled.pNext = &enabled12;
        enabled12.pNext = &enabled11;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT enabled_present{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT
        };
        enabled_present.swapchainMaintenance1 = options.swapchain_maintenance;
        enabled11.pNext = options.swapchain_maintenance ? &enabled_present : nullptr;
        enabled_present.pNext = options.local_read ? &enabled_local : nullptr;
        if (!options.swapchain_maintenance && options.local_read)
        {
            enabled11.pNext = &enabled_local;
        }
        VkPhysicalDeviceFeatures base_features{};
        base_features.textureCompressionBC = enabled_options.texture_compression_bc;
        base_features.shaderSampledImageArrayDynamicIndexing = enabled_options.texture_descriptors;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pEnabledFeatures = &base_features;
        info.pNext = &enabled;
        info.queueCreateInfoCount = queue_count;
        info.pQueueCreateInfos = queue_infos.data();
        info.enabledExtensionCount = static_cast<std::uint32_t>(requested->size());
        info.ppEnabledExtensionNames = requested->data();
        gapi::vk::LogicalDevice device;
        const auto created = [&]() noexcept
        {
            auto value = gapi::vk::LogicalDevice::create(selected->handle(), info);
            if (!value)
            {
                return value.error();
            }
            device = std::move(*value);
            return VK_SUCCESS;
        }();
        if (created != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(created));
        }
        for (auto& queue : queues)
        {
            vkGetDeviceQueue(device.handle(), queue.family, queue.index, &queue.handle);
        }
        const EnabledDeviceFeatures features{
            bool(enabled.dynamicRendering),
            bool(enabled12.timelineSemaphore),
            bool(enabled11.multiview),
            bool(enabled_local.dynamicRenderingLocalRead),
            bool(enabled12.separateDepthStencilLayouts),
            bool(enabled_present.swapchainMaintenance1),
            bool(base_features.textureCompressionBC),
            bool(enabled12.runtimeDescriptorArray),
            bool(enabled12.hostQueryReset),
            bool(enabled12.bufferDeviceAddress)
        };
        auto allocator = VulkanAllocator::create(
            instance.native(),
            selected->handle(),
            device.handle(),
            features.buffer_device_address
        );
        if (!allocator)
        {
            return cxx::unexpected(allocator.error());
        }
        auto result = std::unique_ptr<DeviceSession>(
            new DeviceSession(std::move(*selected), std::move(device), features, std::move(*allocator))
        );
        result->physical_queues_.reserve(3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto handle = queues[i].handle;
            const auto existing = std::ranges::find_if(
                result->physical_queues_,
                [&](const auto& queue) { return queue->nativeQueue().handle == handle; }
            );
            const auto index = static_cast<std::uint8_t>(existing - result->physical_queues_.begin());
            if (existing == result->physical_queues_.end())
            {
                auto queue =
                    SubmissionQueue::create(result->nativeDevice(), queues[i], features.timeline_semaphore, slots);
                if (!queue)
                {
                    return cxx::unexpected(queue.error());
                }
                result->physical_queues_.push_back(std::move(*queue));
            }
            result->queue_roles_[i] = index;
        }
        return result;
    }

    VulkanAllocator::VulkanAllocator(VmaAllocator_T* allocator, VkDevice device, bool buffer_device_address) noexcept
        : allocator_(allocator), device_(device), buffer_device_address_(buffer_device_address)
    {
    }

    RenderResult<VulkanAllocator> VulkanAllocator::create(
        VkInstance instance,
        VkPhysicalDevice physical,
        VkDevice device,
        bool buffer_device_address
    ) noexcept
    {
        VmaAllocatorCreateInfo info{};
        info.instance = instance;
        info.physicalDevice = physical;
        info.device = device;
        info.vulkanApiVersion = VK_API_VERSION_1_3;
        if (buffer_device_address)
        {
            info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }
        VmaAllocator allocator{};
        const auto result = vmaCreateAllocator(&info, &allocator);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        return VulkanAllocator{allocator, device, buffer_device_address};
    }

    void VulkanAllocator::release() noexcept
    {
        if (allocator_)
        {
            vmaDestroyAllocator(allocator_);
        }
    }

    VulkanAllocator::~VulkanAllocator() noexcept
    {
        release();
    }

    VulkanAllocator::VulkanAllocator(VulkanAllocator&& other) noexcept
        : allocator_(std::exchange(other.allocator_, nullptr)), device_(other.device_),
          buffer_device_address_(other.buffer_device_address_)
    {
    }

    VulkanAllocator& VulkanAllocator::operator=(VulkanAllocator&& other) noexcept
    {
        if (this != &other)
        {
            release();
            allocator_ = std::exchange(other.allocator_, nullptr);
            device_ = other.device_;
            buffer_device_address_ = other.buffer_device_address_;
        }
        return *this;
    }

    RenderResult<gapi::vk::VmaBuffer> VulkanAllocator::createBuffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        EMemoryAccess access,
        std::span<const std::uint32_t> families
    ) noexcept
    {
        const bool is_invalid_access =
            access != EMemoryAccess::DEVICE && access != EMemoryAccess::UPLOAD && access != EMemoryAccess::READBACK;
        const bool is_invalid_config = size == 0 || usage == 0 || is_invalid_access;
        if (is_invalid_config)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const bool is_unsupported_address =
            (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) && !buffer_device_address_;
        if (is_unsupported_address)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = families.empty() ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<std::uint32_t>(families.size());
        info.pQueueFamilyIndices = families.data();
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        if (access == EMemoryAccess::DEVICE)
        {
            memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        }
        else
        {
            memory.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                           (access == EMemoryAccess::UPLOAD ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                                                            : VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT);
            memory.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        }
        auto buffer = gapi::vk::VmaBuffer::create(native(), info, memory);
        if (!buffer)
        {
            return cxx::unexpected(nativeError(buffer.error()));
        }
        if (access != EMemoryAccess::DEVICE && buffer->mappedBytes(native()).empty())
        {
            buffer->release(native());
            return cxx::unexpected(nativeError(VK_ERROR_MEMORY_MAP_FAILED));
        }
        return std::move(*buffer);
    }

    RenderResult<gapi::vk::VmaImage> VulkanAllocator::createImage(const ImageDescription& description) noexcept
    {
        const auto extent = description.extent;
        const auto format = description.format;
        const auto usage = description.usage;
        const auto samples = description.samples;
        const auto mip_levels = description.mip_levels;
        const auto array_layers = description.array_layers;
        std::uint32_t max_levels = 0;
        for (auto dimension = std::max(extent.width, extent.height); dimension != 0; dimension >>= 1)
        {
            ++max_levels;
        }
        const bool invalid_subresources = mip_levels == 0 || array_layers == 0 || mip_levels > max_levels ||
                                          (samples != VK_SAMPLE_COUNT_1_BIT && mip_levels != 1);
        const bool is_invalid_config =
            extent.width == 0 || extent.height == 0 || format == VK_FORMAT_UNDEFINED || usage == 0 || samples == 0 ||
            (static_cast<std::uint32_t>(samples) & (static_cast<std::uint32_t>(samples) - 1)) != 0;
        if (is_invalid_config || invalid_subresources)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VmaAllocatorInfo allocator_info{};
        vmaGetAllocatorInfo(native(), &allocator_info);
        VkImageFormatProperties supported{};
        const auto support = vkGetPhysicalDeviceImageFormatProperties(
            allocator_info.physicalDevice,
            format,
            VK_IMAGE_TYPE_2D,
            VK_IMAGE_TILING_OPTIMAL,
            usage,
            0,
            &supported
        );
        if (support != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(support));
        }
        const bool is_too_large = extent.width > supported.maxExtent.width ||
                                  extent.height > supported.maxExtent.height || mip_levels > supported.maxMipLevels ||
                                  array_layers > supported.maxArrayLayers;
        if (is_too_large || (supported.sampleCounts & samples) == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = 0;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = mip_levels;
        info.arrayLayers = array_layers;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        auto image = gapi::vk::VmaImage::create(native(), info, memory);
        if (!image)
        {
            return cxx::unexpected(nativeError(image.error()));
        }
        return std::move(*image);
    }

} // namespace lux::render::vulkan
