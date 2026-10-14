#pragma once

#include <lux/engine/render/native/vulkan/Program.hpp>
#include <lux/engine/render/native/vulkan/device/Device.hpp>

namespace lux::render::vulkan
{
    [[nodiscard]] RenderResult<VkPipeline> compileGraphics(
        const VulkanDevice& device, VkPipelineLayout layout, const ShaderArtifact& shader
    ) noexcept;
}
