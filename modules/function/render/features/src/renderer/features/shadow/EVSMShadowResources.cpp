#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/FrameServices.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>
#include <lux/engine/render/renderer/features/shadow/EVSMShadowResources.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    namespace
    {
        struct EvsmAtlasCandidate
        {
            VmaImage image;
            ImageViewOwner view;
        };

        Expected<EvsmAtlasCandidate> createEvsmAtlas(
            const EVSMShadowResources::CreateInfo& info,
            VkImageUsageFlags usage
        ) noexcept
        {
            VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            image_info.extent = {info.atlas_page_resolution, info.atlas_page_resolution, 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = info.atlas_page_count;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            auto image = VmaImage::create(info.device.vmaAllocator(), image_info, allocation_info);
            if (!image)
            {
                return lux::cxx::unexpected(image.error());
            }
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image->image();
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            view_info.format = image_info.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.atlas_page_count};
            auto view = ImageViewOwner::create(info.device.logicalDevice(), view_info);
            if (!view)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(view.error()));
            }
            return EvsmAtlasCandidate{std::move(*image), std::move(*view)};
        }
    } // namespace

    EVSMShadowResources::CreateResult EVSMShadowResources::create(const CreateInfo& info) noexcept
    {
        const bool is_invalid_frames = info.frames_in_flight == 0 || info.frames_in_flight > kMaxFramesInFlight;
        const bool is_invalid_extent = info.atlas_page_resolution == 0 || info.atlas_page_count == 0;
        const bool is_invalid_sets =
            info.domain_sets.size() != info.frames_in_flight ||
            std::ranges::any_of(info.domain_sets, [](auto set) { return set == VK_NULL_HANDLE; });
        const bool is_invalid_offset =
            info.domain_binding_offset > UINT32_MAX - static_cast<uint32_t>(ELightSetBindings::SHADOW_EVSM_CONFIG);
        const bool is_invalid_configuration =
            is_invalid_frames || is_invalid_extent || is_invalid_sets || is_invalid_offset;
        if (is_invalid_configuration)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }

        DomainWriteTarget domain;
        if (auto accepted = domain.set(info.domain_sets, info.domain_binding_offset); !accepted)
        {
            return lux::cxx::unexpected(accepted.error());
        }

        auto moment = createEvsmAtlas(info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (!moment)
        {
            return lux::cxx::unexpected(moment.error());
        }
        auto scratch = createEvsmAtlas(info, 0);
        if (!scratch)
        {
            return lux::cxx::unexpected(scratch.error());
        }
        std::vector<VmaBuffer> buffers;
        buffers.reserve(info.frames_in_flight);
        for (uint32_t fi = 0; fi < info.frames_in_flight; ++fi)
        {
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = sizeof(ConfigGPU);
            buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocation_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaBuffer::Allocation allocation{info.device.vmaAllocator()};
            VmaAllocationInfo mapping{};
            const auto allocated = vmaCreateBuffer(
                allocation.allocator,
                &buffer_info,
                &allocation_info,
                &allocation.buffer,
                &allocation.allocation,
                &mapping
            );
            if (allocated != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(allocated));
            }
            auto buffer = VmaBuffer::adopt(allocation);
            if (!mapping.pMappedData)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            std::memcpy(mapping.pMappedData, &info.config, sizeof(ConfigGPU));
            const auto flushed = vmaFlushAllocation(allocation.allocator, allocation.allocation, 0, sizeof(ConfigGPU));
            if (flushed != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(flushed));
            }
            buffers.push_back(std::move(buffer));
        }
        auto sampler = info.descriptors.sampler(SamplerDesc::linearClamp());
        if (!sampler)
        {
            return lux::cxx::unexpected(sampler.error());
        }

        // No fallible native work remains. Transfer complete candidates to the original retirement owner.
        const auto adopt_atlas = [&info](EvsmAtlasCandidate& candidate)
        {
            const auto allocation = candidate.image.release();
            return Atlas{
                TFifOwnedAllocated<VkImage>{info.retirement, allocation.image, allocation.allocation},
                TFifOwned<VkImageView>{&info.retirement, candidate.view.release()}
            };
        };
        std::vector<ConfigBuffer> config;
        config.reserve(buffers.size());
        for (auto& buffer : buffers)
        {
            const auto allocation = buffer.release();
            config.emplace_back(info.retirement, allocation.buffer, allocation.allocation);
        }
        return std::unique_ptr<EVSMShadowResources>(new EVSMShadowResources(
            info,
            std::move(domain),
            adopt_atlas(*moment),
            adopt_atlas(*scratch),
            *sampler,
            std::move(config)
        ));
    }

    void EVSMShadowResources::bindDescriptors() const noexcept
    {
        const VkDescriptorImageInfo image{sampler_, blurredView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for (uint32_t fi = 0; fi < framesInFlight(); ++fi)
        {
            const VkDescriptorBufferInfo buffer{configUBO(fi), 0, sizeof(ConfigGPU)};
            std::array<VkWriteDescriptorSet, 2> writes{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = domain_.setFor(fi);
            writes[0].dstBinding = domain_.binding(static_cast<uint32_t>(ELightSetBindings::SHADOW_ATLAS_EVSM));
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &image;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = domain_.setFor(fi);
            writes[1].dstBinding = domain_.binding(static_cast<uint32_t>(ELightSetBindings::SHADOW_EVSM_CONFIG));
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &buffer;
            vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }

    EVSMShadowResources::EVSMShadowResources(
        const CreateInfo& info,
        DomainWriteTarget domain,
        Atlas moment,
        Atlas scratch,
        VkSampler sampler,
        std::vector<ConfigBuffer> config
    ) noexcept
        : moment_(std::move(moment)), scratch_(std::move(scratch)), sampler_(sampler), config_ubos_(std::move(config)),
          atlas_page_resolution_(info.atlas_page_resolution), atlas_page_count_(info.atlas_page_count),
          device_(info.device.logicalDevice()), domain_(std::move(domain))
    {
    }
} // namespace lux::render
