#include <NRI.h>

#include <Extensions/NRIHelper.h>
#include <lux/engine/render/device/DeviceBackend.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>

#include <algorithm>
#include <limits>
#include <numeric>
#include <vulkan/vulkan.h>

namespace lux::render::detail
{
    RenderResult<void> DeviceSession::validateTexture(const nri::TextureDesc& texture) const noexcept
    {
        const auto& properties = *nri::nriGetFormatProps(texture.format);
        const auto format = properties.isSrgb ? static_cast<nri::Format>(static_cast<std::uint32_t>(texture.format) - 1)
                                              : texture.format;
        // Match v180 DeviceVK::FillCreateInfo, including flags added by the backend.
        VkPhysicalDeviceImageFormatInfo2 input{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        input.format = static_cast<VkFormat>(nri::nriConvertNRIFormatToVK(format));
        switch (texture.type)
        {
        case nri::TextureType::TEXTURE_1D:
            input.type = VK_IMAGE_TYPE_1D;
            break;
        case nri::TextureType::TEXTURE_2D:
            input.type = VK_IMAGE_TYPE_2D;
            break;
        case nri::TextureType::TEXTURE_3D:
            input.type = VK_IMAGE_TYPE_3D;
            break;
        }
        input.tiling = VK_IMAGE_TILING_OPTIMAL;
        input.flags =
            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_ALIAS_BIT;
        input.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const auto add_usage = [&](nri::TextureUsageBits use, VkImageUsageFlags native) noexcept
        {
            if ((texture.usage & use) != 0)
            {
                input.usage |= native;
            }
        };
        add_usage(nri::TextureUsageBits::SHADER_RESOURCE, VK_IMAGE_USAGE_SAMPLED_BIT);
        add_usage(nri::TextureUsageBits::SHADER_RESOURCE_STORAGE, VK_IMAGE_USAGE_STORAGE_BIT);
        add_usage(nri::TextureUsageBits::COLOR_ATTACHMENT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        add_usage(nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        add_usage(
            nri::TextureUsageBits::SHADING_RATE_ATTACHMENT,
            VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
        );
        add_usage(nri::TextureUsageBits::INPUT_ATTACHMENT, VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
        if (properties.blockWidth > 1 && (texture.usage & nri::TextureUsageBits::SHADER_RESOURCE_STORAGE) != 0)
        {
            input.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
        }
        if (texture.layerNum >= 6 && texture.width == texture.height)
        {
            input.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        if (texture.type == nri::TextureType::TEXTURE_3D)
        {
            input.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        }
        if (description().tiers.sampleLocations && properties.isDepth)
        {
            input.flags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
        }
        const auto instance = static_cast<VkInstance>(backend_->vulkan.GetInstanceVK(device()));
        const auto physical = static_cast<VkPhysicalDevice>(backend_->vulkan.GetPhysicalDeviceVK(device()));
        const auto get_proc =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(backend_->vulkan.GetInstanceProcAddrVK(device()));
        const auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            get_proc(instance, "vkGetPhysicalDeviceImageFormatProperties2")
        );
        VkImageFormatProperties2 output{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        const auto status = query(physical, &input, &output);
        if (status == VK_ERROR_FORMAT_NOT_SUPPORTED)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(RenderError{kDeviceFailure, {std::uint64_t(std::uint32_t(status)), 1}});
        }
        const auto& limits = output.imageFormatProperties;
        const bool unsupported = texture.width > limits.maxExtent.width ||
                                 std::max(texture.height, nri::Dim_t{1}) > limits.maxExtent.height ||
                                 std::max(texture.depth, nri::Dim_t{1}) > limits.maxExtent.depth ||
                                 std::max(texture.mipNum, nri::Dim_t{1}) > limits.maxMipLevels ||
                                 std::max(texture.layerNum, nri::Dim_t{1}) > limits.maxArrayLayers ||
                                 (limits.sampleCounts & std::max(texture.sampleNum, nri::Sample_t{1})) == 0;
        if (unsupported)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        return {};
    }

    RenderResult<void> DeviceBackend::initialize(
        DeviceSession& session,
        const DeviceOptions& options,
        const nri::CallbackInterface& diagnostic,
        bool validation,
        std::uint32_t slots
    ) noexcept
    {
        const bool invalid_capacity = slots == 0;
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
        session.device_ = device.release();
        session.nri_core_ = api;
        session.backend_->vulkan = vk;
        session.enabled_features_ = enabled;
        auto* result = &session;
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
        const auto sampler_capacity = std::min(
            {description.descriptorSet.updateAfterSet.samplerMaxNum,
             description.shaderStage.updateAfterSet.descriptorSamplerMaxNum,
             indexing.maxUpdateAfterBindDescriptorsInAllPools / 2}
        );
        const auto image_capacity = std::min(
            {description.descriptorSet.updateAfterSet.textureMaxNum,
             description.shaderStage.updateAfterSet.descriptorTextureMaxNum,
             indexing.maxUpdateAfterBindDescriptorsInAllPools - sampler_capacity}
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
        // for concurrent sharing. Frame-local attachments explicitly select exclusive sharing.
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
        if (options.texture_descriptors)
        {
            if (image_capacity == 0 || sampler_capacity == 0)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            if (auto ready = session.descriptor_table_.initialize(image_capacity, sampler_capacity); !ready)
            {
                return ready;
            }
        }
        return {};
    }

    nri::Result DeviceSession::queryFence(nri::Fence& fence, std::uint64_t& value) const noexcept
    {
        return backend_->vulkan.GetFenceValueCheckedVK(fence, value);
    }

    nri::Result DeviceSession::waitFence(nri::Fence& fence, std::uint64_t value, std::uint64_t timeout, bool& ready)
        const noexcept
    {
        return backend_->vulkan.WaitFenceCheckedVK(fence, value, timeout, ready);
    }

    nri::Result DeviceSession::resetCommandAllocator(nri::CommandAllocator& allocator) const noexcept
    {
        return backend_->vulkan.ResetCommandAllocatorCheckedVK(allocator);
    }

    nri::Result DeviceSession::flushMappedRange(nri::Buffer& buffer, std::uint64_t offset, std::uint64_t size)
        const noexcept
    {
        return backend_->vulkan.FlushBufferRangeVK(buffer, offset, size);
    }

    nri::Result DeviceSession::invalidateMappedRange(nri::Buffer& buffer, std::uint64_t offset, std::uint64_t size)
        const noexcept
    {
        return backend_->vulkan.InvalidateBufferRangeVK(buffer, offset, size);
    }
} // namespace lux::render::detail
