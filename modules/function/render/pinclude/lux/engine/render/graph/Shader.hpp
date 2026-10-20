#pragma once

#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <lux/engine/gapi/vk/PhysicalDevice.hpp>
#include <lux/engine/render/device/Device.hpp>
#include <lux/engine/render/graph/Program.hpp>

namespace lux::render::detail
{
    struct VertexFormat final
    {
        VkFormat native = VK_FORMAT_UNDEFINED;
        rdesc::EDataScalar scalar = rdesc::EDataScalar::FLOAT;
        std::uint32_t components = 0;

        [[nodiscard]] std::uint32_t bytes() const noexcept
        {
            return components * 4;
        }
    };

    [[nodiscard]] VertexFormat vertexFormat(rdesc::ETextureFormat) noexcept;
    [[nodiscard]] RenderResult<VkDescriptorSetLayout> createDescriptorLayout(
        VkDevice,
        std::span<const ShaderBinding>,
        std::uint32_t runtime_count
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileCompute(
        VkDevice device,
        const VkPhysicalDeviceLimits& limits,
        VkPipelineLayout layout,
        const rdesc::SpirvArtifact& shader
    ) noexcept;
    [[nodiscard]] RenderResult<VkPipeline> compileGraphics(
        VkDevice device,
        const gapi::vk::PhysicalDevice& physical,
        const EnabledDeviceFeatures& enabled,
        VkPipelineLayout layout,
        const rdesc::SpirvArtifact& shader,
        const GraphicsState& graphics,
        VkFormat color,
        VkFormat depth
    ) noexcept;
} // namespace lux::render::detail
