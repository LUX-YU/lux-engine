#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>

#include "Native.hpp"

namespace lux::render::vulkan
{
    RenderResult<ShaderModule> ShaderModule::create(
        const VulkanDevice &device, std::span<const std::uint32_t> spirv
    ) noexcept
    {
        const bool is_invalid_binary = spirv.size() < 5 || spirv[0] != 0x07230203u;
        if (is_invalid_binary)
            return cxx::unexpected(RenderError{kInvalidArgument});
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size_bytes();
        info.pCode = spirv.data();
        VkShaderModule module{};
        const auto result = LUX_NATIVE("shader", vkCreateShaderModule(device.native(), &info, nullptr, &module));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        return ShaderModule{device.native(), module};
    }

    RenderResult<PipelineLayout> PipelineLayout::create(
        const VulkanDevice &device, std::span<const VkDescriptorSetLayout> layouts
    ) noexcept
    {
        if (layouts.size() > device.properties().limits.maxBoundDescriptorSets)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        for (auto layout : layouts)
        {
            if (!layout)
                return cxx::unexpected(RenderError{kInvalidArgument});
        }
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
        info.pSetLayouts = layouts.data();
        VkPipelineLayout layout{};
        const auto result =
            LUX_NATIVE("pipeline_layout", vkCreatePipelineLayout(device.native(), &info, nullptr, &layout));
        if (result != VK_SUCCESS)
            return cxx::unexpected(nativeError(result));
        return PipelineLayout{device.native(), layout};
    }

    RenderResult<ComputePipeline> ComputePipeline::create(
        const VulkanDevice &device, const ShaderModule &shader, const PipelineLayout &layout
    ) noexcept
    {
        const bool is_wrong_owner = shader.device() != device.native() || layout.device() != device.native();
        if (is_wrong_owner)
            return cxx::unexpected(RenderError{kWrongOwner});
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        info.layout = layout.native();
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = shader.native();
        info.stage.pName = "main";
        VkPipeline pipeline{};
        const auto result = LUX_NATIVE(
            "pipeline", vkCreateComputePipelines(device.native(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline)
        );
        if (result != VK_SUCCESS)
        {
            // Vulkan may return partial output handles on pipeline failure.
            if (pipeline)
                LUX_DESTROY("pipeline", vkDestroyPipeline(device.native(), pipeline, nullptr));
            return cxx::unexpected(nativeError(result));
        }
        return ComputePipeline{device.native(), pipeline};
    }
} // namespace lux::render::vulkan
