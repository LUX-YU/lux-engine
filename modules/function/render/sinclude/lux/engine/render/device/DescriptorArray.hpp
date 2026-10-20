#pragma once

#include <lux/engine/render/visibility.h>

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/gapi/vk/Sampler.hpp>
#include <lux/engine/render/device/Device.hpp>
#include <memory>

namespace lux::render::detail
{
    class DeviceSession;
    class DescriptorArray;
    struct DescriptorSlotTag;

    // Session-owned Native allocation, not a business Storage. A slot is returned only after its actual GPU use ends.
    class LUX_RENDER_PUBLIC DescriptorArray final
    {
    public:
        using Slot = cxx::SlotKey<DescriptorSlotTag>;
        void release(Slot slot) noexcept;
        void write(Slot slot, VkImageView image, VkSampler sampler) noexcept;
        ~DescriptorArray() noexcept;
        DescriptorArray(const DescriptorArray&) = delete;
        DescriptorArray& operator=(const DescriptorArray&) = delete;
        [[nodiscard]] RenderResult<cxx::SlotKey<DescriptorSlotTag>> allocate() noexcept;

        [[nodiscard]] VkDescriptorSetLayout layout() const noexcept
        {
            return layout_;
        }

        [[nodiscard]] VkDescriptorSet descriptors() const noexcept
        {
            return descriptors_;
        }

        [[nodiscard]] std::uint32_t capacity() const noexcept
        {
            return capacity_;
        }

        [[nodiscard]] std::size_t occupied() const noexcept
        {
            return slots_.size();
        }

    private:
        friend class DeviceSession;

        struct Entry final
        {
        };

        explicit DescriptorArray(VkDevice device, VmaAllocator allocator, std::uint32_t capacity) noexcept
            : device_(device), allocator_(allocator), capacity_(capacity)
        {
        }

        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> initializeDefault(DeviceSession&) noexcept;
        VkDevice device_;
        VmaAllocator allocator_;
        std::uint32_t capacity_;
        cxx::SlotMap<Entry, DescriptorSlotTag> slots_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        VkDescriptorSet descriptors_ = VK_NULL_HANDLE;
        gapi::vk::VmaImage white_image_;
        VkImageView white_view_ = VK_NULL_HANDLE;
        gapi::vk::Sampler white_sampler_;
        Slot white_slot_{};
    };
} // namespace lux::render::detail
