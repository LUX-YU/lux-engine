#include <lux/engine/render/vulkan/descriptor/Descriptors.hpp>
#include <lux/engine/render/vulkan/memory/Memory.hpp>

#include "Native.hpp"

namespace lux::render::vulkan
{
    RenderResult<DescriptorSetLayout> DescriptorSetLayout::create(
        const VulkanDevice &device, std::span<const VkDescriptorSetLayoutBinding> bindings
    ) noexcept
    {
        for (std::size_t index = 0; index < bindings.size(); ++index)
        {
            const auto &binding = bindings[index];
            const bool is_empty = binding.descriptorCount == 0 || binding.stageFlags == 0;
            if (is_empty)
                return cxx::unexpected(RenderError{kInvalidArgument});
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                if (bindings[previous].binding == binding.binding)
                    return cxx::unexpected(RenderError{kInvalidArgument});
            }
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        VkDescriptorSetLayoutSupport supported{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
        vkGetDescriptorSetLayoutSupport(device.native(), &info, &supported);
        if (!supported.supported)
            return cxx::unexpected(RenderError{kUnsupported});
        VkDescriptorSetLayout layout{};
        const auto result =
            LUX_NATIVE("descriptor_layout", vkCreateDescriptorSetLayout(device.native(), &info, nullptr, &layout));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        return DescriptorSetLayout{device.native(), layout};
    }

    RenderResult<DescriptorPool> DescriptorPool::create(
        const VulkanDevice &device, std::uint32_t max_sets, std::span<const VkDescriptorPoolSize> sizes
    ) noexcept
    {
        if (max_sets == 0)
            return cxx::unexpected(RenderError{kInvalidArgument});
        for (const auto &size : sizes)
        {
            if (size.descriptorCount == 0)
                return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = max_sets;
        info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        info.pPoolSizes = sizes.data();
        VkDescriptorPool pool{};
        const auto result =
            LUX_NATIVE("descriptor_pool", vkCreateDescriptorPool(device.native(), &info, nullptr, &pool));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        return DescriptorPool{device, pool, max_sets};
    }

    RenderResult<VkDescriptorSet> DescriptorPool::allocate(const DescriptorSetLayout &layout) noexcept
    {
        if (layout.device() != device_)
            return cxx::unexpected(RenderError{kWrongOwner});
        if (remaining_sets_ == 0)
            return cxx::unexpected(RenderError{kCapacity});
        const auto native_layout = layout.native();
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = handle_;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &native_layout;
        VkDescriptorSet set{};
        const auto result = LUX_NATIVE("descriptor_set", vkAllocateDescriptorSets(device_, &info, &set));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        --remaining_sets_;
        return set;
    }

    RenderResult<void> DescriptorPool::writeStorageBuffer(
        VkDescriptorSet set, std::uint32_t binding, const Buffer &buffer, VkDeviceSize offset, VkDeviceSize size
    ) noexcept
    {
        if (buffer.device() != device_)
            return cxx::unexpected(RenderError{kWrongOwner});
        const bool is_invalid_range = size == 0 || offset > buffer.size() || size > buffer.size() - offset;
        const bool is_invalid_descriptor = !set || is_invalid_range || size > storage_range_ ||
                                           offset % storage_alignment_ != 0 ||
                                           (buffer.usage() & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0;
        if (is_invalid_descriptor)
            return cxx::unexpected(RenderError{kInvalidArgument});
        const VkDescriptorBufferInfo buffer_info{buffer.native(), offset, size};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        return {};
    }
} // namespace lux::render::vulkan
