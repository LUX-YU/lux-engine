#pragma once

#include "ShaderInterface.hpp"

#include <lux/engine/gapi/vk/PhysicalDevice.hpp>
#include <lux/engine/render/graph/Program.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

namespace lux::render::vulkan
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
} // namespace lux::render::vulkan
