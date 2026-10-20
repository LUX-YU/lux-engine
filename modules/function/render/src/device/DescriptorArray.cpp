#include <algorithm>
#include <lux/engine/gapi/vk/Descriptor.hpp>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/LayoutPlan.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <utility>

namespace lux::render::detail
{
    RenderResult<std::uint32_t> DeviceSession::sampledImageCapacity() const noexcept
    {
        if (!enabled_features_.texture_descriptors)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const auto& limits = physical_device_.descriptorIndexingProperties();
        const auto maximum = std::min(
            {limits.maxDescriptorSetUpdateAfterBindSamplers,
             limits.maxDescriptorSetUpdateAfterBindSampledImages,
             limits.maxPerStageDescriptorUpdateAfterBindSamplers,
             limits.maxPerStageDescriptorUpdateAfterBindSampledImages,
             limits.maxUpdateAfterBindDescriptorsInAllPools}
        );
        if (!maximum)
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        return maximum;
    }

    RenderResult<DescriptorArray*> DeviceSession::sampledImageArray() noexcept
    {
        if (sampled_images_)
        {
            return sampled_images_.get();
        }
        auto capacity = sampledImageCapacity();
        if (!capacity)
        {
            return cxx::unexpected(capacity.error());
        }
        auto result =
            std::unique_ptr<DescriptorArray>(new DescriptorArray(nativeDevice(), allocator_.native(), *capacity));
        if (auto ready = result->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (auto ready = result->initializeDefault(*this); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        sampled_images_ = std::move(result);
        return sampled_images_.get();
    }

    RenderResult<void> DescriptorArray::initialize() noexcept
    {
        auto layout = createDescriptorLayout(device_, std::span(&sampledImageTableBinding(), 1), capacity_);
        if (!layout)
        {
            return cxx::unexpected(layout.error());
        }
        layout_ = *layout;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity_};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        gapi::vk::DescriptorPool handle;
        auto result = handle.tryCreate(device_, pool);
        if (result != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(result));
        }
        pool_ = handle.handle(); // The array owns the complete pool and its allocated set.
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout_;
        result = vkAllocateDescriptorSets(device_, &allocate, &descriptors_);
        return result == VK_SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(result));
    }

    DescriptorArray::~DescriptorArray() noexcept
    {
        if (!white_slot_.isNull())
        {
            slots_.erase(white_slot_);
        }
        if (!slots_.empty())
        {
            std::terminate();
        }
        if (pool_)
        {
            vkDestroyDescriptorPool(device_, pool_, nullptr);
        }
        if (layout_)
        {
            vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
        }
        if (white_view_)
        {
            vkDestroyImageView(device_, white_view_, nullptr);
        }
        if (white_sampler_)
        {
            white_sampler_.release(device_);
        }
        white_image_.release(allocator_);
    }

    RenderResult<void> DescriptorArray::initializeDefault(DeviceSession& session) noexcept
    {
        auto image = session.allocator().createImage(
            {{1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
        );
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        white_image_ = std::move(*image);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = white_image_.handle();
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = white_image_.format();
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto status = vkCreateImageView(device_, &view, nullptr, &white_view_);
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        status = white_sampler_.tryCreate(device_, sampler);
        if (status != VK_SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        white_slot_ = slots_.emplace(); // Slot zero is reserved for this Session's fallback texture.
        write(white_slot_, white_view_, white_sampler_);
        auto batch = session.queue().begin();
        if (!batch)
        {
            return cxx::unexpected(batch.error());
        }
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = white_image_.handle();
        barrier.subresourceRange = view.subresourceRange;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch->native(), &dependency);
        const VkClearColorValue white{{1, 1, 1, 1}};
        vkCmdClearColorImage(batch->native(), barrier.image, barrier.newLayout, &white, 1, &barrier.subresourceRange);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(batch->native(), &dependency);
        return std::move(*batch).submit();
    }

    RenderResult<cxx::SlotKey<DescriptorSlotTag>> DescriptorArray::allocate() noexcept
    {
        if (slots_.size() == capacity_)
        {
            return cxx::unexpected(RenderError{kCapacity});
        }
        const auto slot = slots_.emplace();
        // Exhausted generations are never recycled by SlotMap, including at the physical array bound.
        if (slot.index >= capacity_)
        {
            slots_.erase(slot);
            return cxx::unexpected(RenderError{kCapacity});
        }
        return slot;
    }

    void DescriptorArray::write(Slot slot, VkImageView image, VkSampler sampler) noexcept
    {
        const VkDescriptorImageInfo info{sampler, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = descriptors_;
        write.dstBinding = sampledImageTableBinding().binding;
        write.dstArrayElement = slot.index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    void DescriptorArray::release(Slot slot) noexcept
    {
        slots_.erase(slot);
    }
} // namespace lux::render::detail
