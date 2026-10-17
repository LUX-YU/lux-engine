#pragma once

#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] RenderResult<VkDescriptorSetLayout> createDescriptorLayout(
        const VulkanDevice&, std::span<const ShaderBinding>, std::uint32_t runtime_count, const char* fault_label
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileCompute(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileGraphics(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader
    ) noexcept;
}
