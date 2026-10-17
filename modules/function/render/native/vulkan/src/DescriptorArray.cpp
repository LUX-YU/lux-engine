#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "Shader.hpp"
#include "Native.hpp"
#include <algorithm>

namespace lux::render::vulkan
{
    RenderResult<std::uint32_t> DeviceSession::sampledImageCapacity() const noexcept
    {
        if (!device_.textureDescriptors()) return cxx::unexpected(RenderError{kUnsupported});
        VkPhysicalDeviceDescriptorIndexingProperties limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits};
        vkGetPhysicalDeviceProperties2(device_.physical(), &properties);
        const auto maximum = std::min({limits.maxDescriptorSetUpdateAfterBindSamplers,
            limits.maxDescriptorSetUpdateAfterBindSampledImages, limits.maxPerStageDescriptorUpdateAfterBindSamplers,
            limits.maxPerStageDescriptorUpdateAfterBindSampledImages, limits.maxUpdateAfterBindDescriptorsInAllPools});
        if (!maximum) return cxx::unexpected(RenderError{kUnsupported});
        return maximum;
    }

    RenderResult<std::shared_ptr<DescriptorArray>> DeviceSession::sampledImageArray(ShaderBinding binding) noexcept
    {
        const bool is_invalid = binding.kind != EDescriptorKind::SAMPLED_IMAGE_ARRAY || binding.count != 0;
        if (is_invalid) return cxx::unexpected(RenderError{kStorageContract});
        // Cold allocation lookup only; expired allocations are not retained as a permanent resource cache.
        std::erase_if(descriptor_arrays_, [](const auto& value) { return value.expired(); });
        for (const auto& weak : descriptor_arrays_)
            if (auto existing = weak.lock(); existing && existing->binding_ == binding) return existing;
        auto capacity = sampledImageCapacity();
        if (!capacity) return cxx::unexpected(capacity.error());
        auto result = std::shared_ptr<DescriptorArray>(new DescriptorArray(device_, binding, *capacity));
        if (auto ready = result->initialize(); !ready) return cxx::unexpected(ready.error());
        descriptor_arrays_.push_back(result);
        return result;
    }

    RenderResult<void> DescriptorArray::initialize() noexcept
    {
        auto layout = createDescriptorLayout(device_, std::span(&binding_, 1), capacity_, "descriptor_array_layout");
        if (!layout) return cxx::unexpected(layout.error());
        layout_ = *layout;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, capacity_};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        auto result = LUX_NATIVE("descriptor_array_pool", vkCreateDescriptorPool(device_.native(), &pool, nullptr, &pool_));
        if (result != VK_SUCCESS) return cxx::unexpected(nativeError(result));
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout_;
        result = LUX_NATIVE("descriptor_array_set", vkAllocateDescriptorSets(device_.native(), &allocate, &descriptors_));
        return result == VK_SUCCESS ? RenderResult<void>{} : cxx::unexpected(nativeError(result));
    }

    DescriptorArray::~DescriptorArray() noexcept
    {
        if (!slots_.empty()) std::terminate();
        if (pool_) LUX_DESTROY("descriptor_array_pool", vkDestroyDescriptorPool(device_.native(), pool_, nullptr));
        if (layout_) LUX_DESTROY("descriptor_array_layout", vkDestroyDescriptorSetLayout(device_.native(), layout_, nullptr));
    }

    RenderResult<DescriptorArray::Slot> DescriptorArray::acquire() noexcept
    {
        if (slots_.size() == capacity_) return cxx::unexpected(RenderError{kCapacity});
        const auto slot = slots_.emplace();
        // Exhausted generations are never recycled by SlotMap, including at the physical array bound.
        if (slot.index >= capacity_)
        {
            slots_.erase(slot);
            return cxx::unexpected(RenderError{kCapacity});
        }
        return slot;
    }

    RenderResult<void> DescriptorArray::release(Slot slot) noexcept
    {
        if (!slots_.erase(slot)) return cxx::unexpected(RenderError{kWrongOwner});
        return {};
    }
}
