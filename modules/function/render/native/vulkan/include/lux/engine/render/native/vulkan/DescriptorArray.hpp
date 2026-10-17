#pragma once

#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <lux/engine/render/data/ShaderInterface.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <memory>

namespace lux::render::vulkan
{
    class DeviceSession;
    struct DescriptorSlotTag;

    // Shared Native allocation, not a business Storage. A slot is returned only after its actual GPU use ends.
    class DescriptorArray final
    {
    public:
        using Slot = cxx::SlotKey<DescriptorSlotTag>;
        ~DescriptorArray() noexcept;
        DescriptorArray(const DescriptorArray&) = delete;
        DescriptorArray& operator=(const DescriptorArray&) = delete;
        [[nodiscard]] RenderResult<Slot> acquire() noexcept;
        [[nodiscard]] RenderResult<void> release(Slot slot) noexcept;
        // Caller owns a live, unpublished slot; physical placement belongs to this Native table.
        void write(Slot slot, VkImageView image, VkSampler sampler) noexcept;
        [[nodiscard]] VkDescriptorSetLayout layout() const noexcept { return layout_; }
        [[nodiscard]] VkDescriptorSet descriptors() const noexcept { return descriptors_; }
        [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] std::size_t occupied() const noexcept { return slots_.size(); }

    private:
        friend class DeviceSession;
        struct Entry final {};
        explicit DescriptorArray(const VulkanDevice& device, ShaderBinding binding, std::uint32_t capacity) noexcept
            : device_(device), binding_(binding), capacity_(capacity) {}
        [[nodiscard]] RenderResult<void> initialize() noexcept;
        const VulkanDevice& device_;
        ShaderBinding binding_;
        std::uint32_t capacity_;
        cxx::SlotMap<Entry, DescriptorSlotTag> slots_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        VkDescriptorSet descriptors_ = VK_NULL_HANDLE;
    };
}
