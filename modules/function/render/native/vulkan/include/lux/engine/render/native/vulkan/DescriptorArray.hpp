#pragma once

#include <lux/cxx/container/SlotMap.hpp>
#include <lux/engine/gapi/vk/Sampler.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <memory>

namespace lux::render::vulkan
{
    class DeviceSession;
    class DescriptorArray;
    struct DescriptorSlotTag;

    // Owns one allocated descriptor slot; the Session-owned array outlives all active and retired slots.
    // Allocation, writes and final range destruction occur on the Native owner thread.
    class DescriptorRange final
    {
    public:
        DescriptorRange() noexcept = default;
        ~DescriptorRange() noexcept;
        DescriptorRange(DescriptorRange&& other) noexcept;
        DescriptorRange& operator=(DescriptorRange&& other) noexcept;
        DescriptorRange(const DescriptorRange&) = delete;
        DescriptorRange& operator=(const DescriptorRange&) = delete;

        [[nodiscard]] std::uint32_t index() const noexcept
        {
            return slot_.index;
        }

        // Only write before publication; retaining an in-flight range does not permit mutation.
        void write(VkImageView image, VkSampler sampler) noexcept;

    private:
        friend class DescriptorArray;
        DescriptorRange(DescriptorArray& array, cxx::SlotKey<DescriptorSlotTag> slot) noexcept;
        DescriptorArray* array_ = nullptr;
        cxx::SlotKey<DescriptorSlotTag> slot_{};
    };

    // Session-owned Native allocation, not a business Storage. A slot is returned only after its actual GPU use ends.
    class DescriptorArray final
    {
    public:
        ~DescriptorArray() noexcept;
        DescriptorArray(const DescriptorArray&) = delete;
        DescriptorArray& operator=(const DescriptorArray&) = delete;
        [[nodiscard]] RenderResult<DescriptorRange> allocate() noexcept;

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
        friend class DescriptorRange;
        using Slot = cxx::SlotKey<DescriptorSlotTag>;
        void release(Slot slot) noexcept;
        void write(Slot slot, VkImageView image, VkSampler sampler) noexcept;

        struct Entry final
        {
        };

        explicit DescriptorArray(VkDevice device, std::uint32_t capacity) noexcept
            : device_(device), capacity_(capacity)
        {
        }

        [[nodiscard]] RenderResult<void> initialize() noexcept;
        [[nodiscard]] RenderResult<void> initializeDefault(DeviceSession&) noexcept;
        VkDevice device_;
        std::uint32_t capacity_;
        cxx::SlotMap<Entry, DescriptorSlotTag> slots_;
        VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
        VkDescriptorPool pool_ = VK_NULL_HANDLE;
        VkDescriptorSet descriptors_ = VK_NULL_HANDLE;
        std::unique_ptr<Image> white_image_;
        VkImageView white_view_ = VK_NULL_HANDLE;
        gapi::vk::Sampler white_sampler_;
        Slot white_slot_{};
    };
} // namespace lux::render::vulkan
