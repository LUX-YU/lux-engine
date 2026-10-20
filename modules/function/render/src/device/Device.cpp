#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>

#include <algorithm>
#include <limits>
#include <numeric>
#include <vulkan/vulkan.h>

namespace lux::render::detail
{
    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const DeviceOptions& options,
        const nri::CallbackInterface& diagnostic,
        bool validation,
        std::uint32_t slots,
        std::uint32_t frames_in_flight
    ) noexcept
    {
        const bool invalid_capacity = slots == 0 || frames_in_flight == 0 || frames_in_flight > 3;
        if (invalid_capacity || !diagnostic.MessageCallback)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::uint32_t count = 0;
        auto status = nri::nriEnumerateAdapters(nullptr, count);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        if (count == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::vector<nri::AdapterDesc> adapters(count);
        status = nri::nriEnumerateAdapters(adapters.data(), count);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        adapters.resize(count);
        auto adapter_index = options.physical_device_index;
        if (adapter_index == std::numeric_limits<std::uint32_t>::max())
        {
            // NRI enumerates adapters in preference order; an explicit index is never silently replaced.
            adapter_index = 0;
        }
        if (adapter_index >= adapters.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto& adapter = adapters[adapter_index];
        if (adapter.queueNum[static_cast<unsigned>(nri::QueueType::GRAPHICS)] == 0)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::array types{nri::QueueType::GRAPHICS, nri::QueueType::GRAPHICS, nri::QueueType::GRAPHICS};
        std::array<std::uint32_t, 3> indices{};
        std::array<std::uint32_t, 3> requested{1, 0, 0};
        if (options.multiple_queues)
        {
            for (unsigned role = 1; role < types.size(); ++role)
            {
                const auto dedicated = role == 1 ? nri::QueueType::COMPUTE : nri::QueueType::COPY;
                if (options.prefer_dedicated_queues && adapter.queueNum[static_cast<unsigned>(dedicated)] != 0)
                {
                    types[role] = dedicated;
                }
                const auto type = static_cast<unsigned>(types[role]);
                indices[role] = requested[type] < adapter.queueNum[type] ? requested[type]++ : 0;
            }
        }
        std::vector<nri::QueueFamilyDesc> families;
        for (unsigned type = 0; type < requested.size(); ++type)
        {
            if (requested[type] != 0)
            {
                families.push_back({nullptr, requested[type], static_cast<nri::QueueType>(type)});
            }
        }
        nri::DeviceCreationDesc creation{};
        creation.graphicsAPI = nri::GraphicsAPI::VK;
        creation.adapterDesc = &adapter;
        creation.queueFamilies = families.data();
        creation.queueFamilyNum = static_cast<std::uint32_t>(families.size());
        creation.callbackInterface = diagnostic;
        creation.enableNRIValidation = validation;
        creation.enableGraphicsAPIValidation = validation;
        creation.disableVKRayTracing = true;
        nri::Device* created = nullptr;
        status = nri::nriCreateDevice(creation, created);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        std::unique_ptr<nri::Device, decltype(&nri::nriDestroyDevice)> device(created, &nri::nriDestroyDevice);
        nri::CoreInterface api{};
        nri::WrapperVKInterface vk{};
        status = nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &api);
        if (status == nri::Result::SUCCESS)
        {
            status = nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperVKInterface), &vk);
        }
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        const auto instance = static_cast<VkInstance>(vk.GetInstanceVK(*device));
        const auto physical = static_cast<VkPhysicalDevice>(vk.GetPhysicalDeviceVK(*device));
        const auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk.GetInstanceProcAddrVK(*device));
        const auto query_features =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get_proc(instance, "vkGetPhysicalDeviceFeatures2"));
        const auto query_families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties")
        );
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &features12};
        query_features(physical, &features);
        const auto& description = api.GetDeviceDesc(*device);
        // Pinned NRI enables this queried feature chain. These are actual backend facts, not requested options.
        const bool bindless = description.tiers.bindless != 0 && features12.runtimeDescriptorArray &&
                              features12.descriptorBindingPartiallyBound &&
                              features12.descriptorBindingSampledImageUpdateAfterBind &&
                              features12.descriptorBindingUpdateUnusedWhilePending &&
                              features12.shaderSampledImageArrayNonUniformIndexing;
        const EnabledDeviceFeatures enabled{
            description.features.layerBasedMultiview,
            description.shaderFeatures.inputAttachments,
            description.features.textureCompressionBC,
            bindless,
            features12.bufferDeviceAddress != 0
        };
        const bool unsupported = (options.texture_descriptors && !enabled.texture_descriptors) ||
                                 (options.texture_compression_bc && !enabled.texture_compression_bc) ||
                                 (options.buffer_device_address && !enabled.buffer_device_address) ||
                                 (options.multiview && !enabled.multiview) ||
                                 (options.local_read && !enabled.local_read);
        if (unsupported)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::uint32_t family_count = 0;
        query_families(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_properties(family_count);
        query_families(physical, &family_count, queue_properties.data());
        auto result = std::unique_ptr<DeviceSession>(new DeviceSession(std::move(device), api, vk, enabled));
        result->paged_instances_ = options.buffer_device_address;
        result->frame_count_ = frames_in_flight;
        const auto query_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_proc(instance, "vkGetPhysicalDeviceProperties"));
        VkPhysicalDeviceProperties properties{};
        query_properties(physical, &properties);
        result->vertex_stride_limit_ = properties.limits.maxVertexInputBindingStride;
        result->vertex_offset_limit_ = properties.limits.maxVertexInputAttributeOffset;
        const auto query_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_proc(instance, "vkGetPhysicalDeviceProperties2"));
        VkPhysicalDeviceVulkan12Properties indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &indexing};
        query_properties2(physical, &properties2);
        result->bindless_capacity_ = std::min(
            {description.descriptorSet.updateAfterSet.samplerMaxNum,
             description.descriptorSet.updateAfterSet.textureMaxNum,
             description.shaderStage.updateAfterSet.descriptorSamplerMaxNum,
             description.shaderStage.updateAfterSet.descriptorTextureMaxNum,
             indexing.maxUpdateAfterBindDescriptorsInAllPools / 2}
        );
        result->upload_alignment_ = std::lcm<std::uint64_t>(
            properties.limits.nonCoherentAtomSize,
            description.memoryAlignment.uploadBufferTextureSlice
        );
        for (std::size_t i = 1; i < result->format_support_.size(); ++i)
        {
            result->format_support_[i] = api.GetFormatSupport(result->device(), static_cast<nri::Format>(i));
        }
        // Get every requested queue before creating buffers. NRI uses the discovered family set
        // for concurrent buffer sharing. Texture ownership transfers remain explicit.
        std::array<nri::Queue*, 3> queues{};
        for (std::size_t role = 0; role < queues.size(); ++role)
        {
            status = api.GetQueue(result->device(), types[role], indices[role], queues[role]);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        result->physical_queues_.reserve(3);
        for (std::size_t role = 0; role < queues.size(); ++role)
        {
            const auto handle = api.GetQueueNativeObject(queues[role]);
            const auto found = std::ranges::find_if(
                result->physical_queues_,
                [&](const auto& queue) { return api.GetQueueNativeObject(&queue->native()) == handle; }
            );
            const auto index = static_cast<std::uint8_t>(found - result->physical_queues_.begin());
            if (found == result->physical_queues_.end())
            {
                const auto family = vk.GetQueueFamilyIndexVK(*queues[role]);
                const auto& properties = queue_properties[family];
                const auto granularity = properties.minImageTransferGranularity;
                const QueueProperties facts{
                    types[role],
                    family,
                    {granularity.width, granularity.height, granularity.depth},
                    properties.timestampValidBits
                };
                auto queue = SubmissionQueue::create(*result, *queues[role], facts, slots);
                if (!queue)
                {
                    return cxx::unexpected(queue.error());
                }
                result->physical_queues_.push_back(std::move(*queue));
            }
            result->queue_roles_[role] = index;
        }
        return result;
    }
} // namespace lux::render::detail
