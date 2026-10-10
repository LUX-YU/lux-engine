#pragma once

#include <lux/engine/render/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    class Buffer;
    class ImageView;
    class Sampler;

    class DescriptorSetLayout
    {
    public:
        [[nodiscard]] static RenderResult<DescriptorSetLayout> create(
            const VulkanDevice& device,
            std::span<const VkDescriptorSetLayoutBinding> bindings
        ) noexcept;
        ~DescriptorSetLayout() noexcept;
        DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
        DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
        DescriptorSetLayout(const DescriptorSetLayout&) = delete;
        DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

        [[nodiscard]] VkDescriptorSetLayout native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        DescriptorSetLayout(VkDevice device, VkDescriptorSetLayout handle) noexcept;
        void release() noexcept;

        VkDevice device_;
        VkDescriptorSetLayout handle_;
    };

    class DescriptorPool
    {
    public:
        [[nodiscard]] static RenderResult<DescriptorPool> create(
            const VulkanDevice& device,
            std::uint32_t max_sets,
            std::span<const VkDescriptorPoolSize> sizes
        ) noexcept;
        ~DescriptorPool() noexcept;
        DescriptorPool(DescriptorPool&& other) noexcept;
        DescriptorPool& operator=(DescriptorPool&& other) noexcept;
        DescriptorPool(const DescriptorPool&) = delete;
        DescriptorPool& operator=(const DescriptorPool&) = delete;

        [[nodiscard]] VkDescriptorPool native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

        // Sets borrow this pool. No individual free/reset; pool destruction
        // releases them after their last GPU use. Layout/native writes obey Vulkan contracts.
        [[nodiscard]] RenderResult<VkDescriptorSet> allocate(const DescriptorSetLayout& layout) noexcept;

        // set must borrow this pool and binding must be a storage-buffer binding
        // in its layout. Caller excludes concurrent/pending descriptor use.
        [[nodiscard]] RenderResult<void> writeStorageBuffer(
            VkDescriptorSet set,
            std::uint32_t binding,
            const Buffer& buffer,
            VkDeviceSize offset,
            VkDeviceSize size
        ) noexcept;

        [[nodiscard]] RenderResult<void> writeBuffer(
            VkDescriptorSet set,
            std::uint32_t binding,
            std::uint32_t element,
            VkDescriptorType type,
            const Buffer& buffer,
            VkDeviceSize offset,
            VkDeviceSize size
        ) noexcept;

        [[nodiscard]] RenderResult<void> writeImage(
            VkDescriptorSet set,
            std::uint32_t binding,
            std::uint32_t element,
            VkDescriptorType type,
            const ImageView& image,
            VkImageLayout layout,
            const Sampler* sampler = nullptr
        ) noexcept;

        [[nodiscard]] RenderResult<void> writeSampler(
            VkDescriptorSet set,
            std::uint32_t binding,
            std::uint32_t element,
            const Sampler& sampler
        ) noexcept;

    private:
        DescriptorPool(const VulkanDevice& device, VkDescriptorPool handle, std::uint32_t remaining) noexcept;
        void release() noexcept;

        VkDevice device_;
        VkDescriptorPool handle_;
        VkDeviceSize storage_alignment_;
        std::uint32_t storage_range_;
        VkDeviceSize uniform_alignment_;
        std::uint32_t uniform_range_;
        std::uint32_t remaining_sets_;
    };

} // namespace lux::render::vulkan
