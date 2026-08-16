#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp>

#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>

#include <algorithm>
#include <vector>

namespace lux::render
{

namespace
{
[[nodiscard]] VkShaderStageFlags shaderStage(const lux::rdesc::ShaderInfo& info) noexcept
{
    if (info.entry_points.empty())
        return 0;

    switch (info.entry_points[0].stage)
    {
    case lux::rdesc::EShaderType::VERTEX:   return VK_SHADER_STAGE_VERTEX_BIT;
    case lux::rdesc::EShaderType::FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
    case lux::rdesc::EShaderType::COMPUTE:  return VK_SHADER_STAGE_COMPUTE_BIT;
    default:                                return 0;
    }
}
} // namespace

Expected<VkPipelineLayout> buildStandardGraphicsPipelineLayout(
    RenderContext& ctx,
    uint32_t descriptor_set_count,
    std::span<const lux::rdesc::ShaderInfo* const> shader_infos,
    std::string_view debug_name)
{
    std::vector<VkDescriptorSetLayout> set_layouts;
    set_layouts.reserve(descriptor_set_count);

    for (uint32_t set = 0; set < descriptor_set_count; ++set)
    {
        const VkDescriptorSetLayout layout = ctx.descriptorLayouts().getLayout(set);
        if (layout == VK_NULL_HANDLE)
            return renderFailure<err::internal::InvalidArgument>();
        set_layouts.push_back(layout);
    }

    std::vector<VkPushConstantRange> push_constants;
    for (const auto* info : shader_infos)
    {
        if (!info)
            continue;

        const VkShaderStageFlags stage = shaderStage(*info);
        for (const auto& pc : info->push_constants)
        {
            bool merged = false;
            for (auto& range : push_constants)
            {
                if (range.offset != pc.offset)
                    continue;

                range.size = std::max(range.size, pc.size);
                range.stageFlags |= stage;
                merged = true;
                break;
            }

            if (!merged)
                push_constants.push_back({stage, pc.offset, pc.size});
        }
    }

    return ctx.pipelineLayoutService().getOrCreate({
        .set_layouts = set_layouts,
        .push_constants = push_constants,
        .debug_name = std::string(debug_name),
    });
}

} // namespace lux::render

