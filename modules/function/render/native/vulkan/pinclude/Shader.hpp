#pragma once

#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    struct VertexFormat final
    {
        VkFormat native = VK_FORMAT_UNDEFINED;
        rdesc::EDataScalar scalar = rdesc::EDataScalar::FLOAT;
        std::uint32_t components = 0;
        [[nodiscard]] std::uint32_t bytes() const noexcept { return components * 4; }
    };
    [[nodiscard]] VertexFormat vertexFormat(rdesc::ETextureFormat) noexcept;
    [[nodiscard]] RenderResult<VkDescriptorSetLayout> createDescriptorLayout(
        const VulkanDevice&, std::span<const ShaderBinding>, std::uint32_t runtime_count, const char* fault_label
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileCompute(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileGraphics(
        const VulkanDevice& device, VkPipelineLayout layout, const SpirvArtifact& shader,
        const GraphicsState& graphics, VkFormat color, VkFormat depth
    ) noexcept;
}
