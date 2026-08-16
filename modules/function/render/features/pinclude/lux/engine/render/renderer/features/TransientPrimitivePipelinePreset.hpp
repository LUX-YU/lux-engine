#pragma once

#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>

#include <cstdint>
#include <span>

namespace lux::render::detail
{
    /// Common fixed-function state for small CPU-authored transient primitive
    /// streams. Vertex layout and topology remain explicit at each domain call
    /// site; the Vulkan state that must stay identical is owned here once.
    [[nodiscard]] inline GraphicsPipelineTemplate
    makeTransientPrimitivePipelineTemplate(
        std::uint32_t vertex_stride,
        std::span<const VkVertexInputAttributeDescription> attributes,
        VkPrimitiveTopology topology,
        EGeometryType geometry_type,
        bool alpha_blend,
        float line_width = 1.0f)
    {
        GraphicsPipelineTemplate result{};
        result.geometry_type = geometry_type;
        result.vertex_bindings = {
            VkVertexInputBindingDescription{
                0u,
                vertex_stride,
                VK_VERTEX_INPUT_RATE_VERTEX}};
        for (const auto& attribute : attributes)
            result.vertex_attributes.push_back(attribute);
        result.topology = topology;
        result.polygon_mode = VK_POLYGON_MODE_FILL;
        result.cull_mode = VK_CULL_MODE_NONE;
        result.line_width = line_width;
        result.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        result.depth_test_enable = VK_TRUE;
        result.depth_write_enable = VK_FALSE;
        result.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
        result.blend_enable = alpha_blend ? VK_TRUE : VK_FALSE;
        result.src_color_blend_factor = alpha_blend
            ? VK_BLEND_FACTOR_SRC_ALPHA
            : VK_BLEND_FACTOR_ONE;
        result.dst_color_blend_factor = alpha_blend
            ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
            : VK_BLEND_FACTOR_ZERO;
        result.color_blend_op = VK_BLEND_OP_ADD;
        result.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        result.dst_alpha_blend_factor = alpha_blend
            ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
            : VK_BLEND_FACTOR_ZERO;
        result.alpha_blend_op = VK_BLEND_OP_ADD;
        result.color_write_mask =
            VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT |
            VK_COLOR_COMPONENT_A_BIT;
        result.use_dynamic_viewport = true;
        result.use_dynamic_scissor = true;
        return result;
    }
}
