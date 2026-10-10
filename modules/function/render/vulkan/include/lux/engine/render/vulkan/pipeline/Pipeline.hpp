#pragma once

#include <lux/engine/render/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    class ShaderModule
    {
    public:
        [[nodiscard]] static RenderResult<ShaderModule> create(
            const VulkanDevice &device, std::span<const std::uint32_t> spirv
        ) noexcept;
        ~ShaderModule() noexcept;
        ShaderModule(ShaderModule &&other) noexcept;
        ShaderModule &operator=(ShaderModule &&other) noexcept;
        ShaderModule(const ShaderModule &) = delete;
        ShaderModule &operator=(const ShaderModule &) = delete;

        [[nodiscard]] VkShaderModule native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        ShaderModule(VkDevice device, VkShaderModule handle) noexcept;
        void release() noexcept;

        VkDevice device_;
        VkShaderModule handle_;
    };

    class PipelineLayout
    {
    public:
        [[nodiscard]] static RenderResult<PipelineLayout> create(
            const VulkanDevice &device, std::span<const VkDescriptorSetLayout> layouts
        ) noexcept;
        ~PipelineLayout() noexcept;
        PipelineLayout(PipelineLayout &&other) noexcept;
        PipelineLayout &operator=(PipelineLayout &&other) noexcept;
        PipelineLayout(const PipelineLayout &) = delete;
        PipelineLayout &operator=(const PipelineLayout &) = delete;

        [[nodiscard]] VkPipelineLayout native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        PipelineLayout(VkDevice device, VkPipelineLayout handle) noexcept;
        void release() noexcept;

        VkDevice device_;
        VkPipelineLayout handle_;
    };

    class ComputePipeline
    {
    public:
        [[nodiscard]] static RenderResult<ComputePipeline> create(
            const VulkanDevice &device, const ShaderModule &shader, const PipelineLayout &layout
        ) noexcept;
        ~ComputePipeline() noexcept;
        ComputePipeline(ComputePipeline &&other) noexcept;
        ComputePipeline &operator=(ComputePipeline &&other) noexcept;
        ComputePipeline(const ComputePipeline &) = delete;
        ComputePipeline &operator=(const ComputePipeline &) = delete;

        [[nodiscard]] VkPipeline native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        ComputePipeline(VkDevice device, VkPipeline handle) noexcept;
        void release() noexcept;

        VkDevice device_;
        VkPipeline handle_;
    };

} // namespace lux::render::vulkan
