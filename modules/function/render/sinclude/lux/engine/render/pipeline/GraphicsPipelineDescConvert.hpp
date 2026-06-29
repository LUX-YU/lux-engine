#pragma once
/**
 * @file GraphicsPipelineDescConvert.hpp  (sinclude — internal)
 * @brief Convert the public RHI-neutral GraphicsPipelineDesc into the internal
 *        Vulkan GraphicsPipelineTemplate. This is the "three-tier" conversion
 *        boundary: neutral enums in, Vk enums out. Called by the IRenderContextView
 *        impl's registerGraphics() before PipelineManager::registerGraphicsTemplate.
 *
 * Cost measurement (this file = the entire neutral->Vk conversion table):
 *   11 enum mappers + 1 reuse of convertTextureFormat() for vertex formats.
 */

#include <vulkan/vulkan.h>
#include <lux/engine/render/pipeline/GraphicsPipelineDesc.hpp>      // public neutral
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>  // internal Vk
#include <lux/engine/render/graph/vk_type_converter.hpp>            // convertTextureFormat(ETextureFormat)->VkFormat

namespace lux::render::detail
{
    inline VkPrimitiveTopology toVk(EPrimitiveTopology t) noexcept
    {
        switch (t) {
            case EPrimitiveTopology::POINT_LIST:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case EPrimitiveTopology::LINE_LIST:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case EPrimitiveTopology::LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case EPrimitiveTopology::TRIANGLE_LIST:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            case EPrimitiveTopology::TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case EPrimitiveTopology::TRIANGLE_FAN:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
    inline VkPolygonMode toVk(EPolygonMode m) noexcept
    {
        switch (m) {
            case EPolygonMode::FILL:  return VK_POLYGON_MODE_FILL;
            case EPolygonMode::LINE:  return VK_POLYGON_MODE_LINE;
            case EPolygonMode::POINT: return VK_POLYGON_MODE_POINT;
        }
        return VK_POLYGON_MODE_FILL;
    }
    inline VkCullModeFlags toVk(ECullMode c) noexcept
    {
        return static_cast<VkCullModeFlags>(c); // bit layout matches VK_CULL_MODE_{NONE,FRONT,BACK,FRONT_AND_BACK}_BIT
    }
    inline VkFrontFace toVk(EFrontFace f) noexcept
    {
        return f == EFrontFace::CLOCKWISE ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    inline VkCompareOp toVk(ECompareOp o) noexcept
    {
        switch (o) {
            case ECompareOp::NEVER:            return VK_COMPARE_OP_NEVER;
            case ECompareOp::LESS:             return VK_COMPARE_OP_LESS;
            case ECompareOp::EQUAL:            return VK_COMPARE_OP_EQUAL;
            case ECompareOp::LESS_OR_EQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
            case ECompareOp::GREATER:          return VK_COMPARE_OP_GREATER;
            case ECompareOp::NOT_EQUAL:        return VK_COMPARE_OP_NOT_EQUAL;
            case ECompareOp::GREATER_OR_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case ECompareOp::ALWAYS:           return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    inline VkBlendFactor toVk(EBlendFactor b) noexcept
    {
        switch (b) {
            case EBlendFactor::ZERO:                     return VK_BLEND_FACTOR_ZERO;
            case EBlendFactor::ONE:                      return VK_BLEND_FACTOR_ONE;
            case EBlendFactor::SRC_COLOR:                return VK_BLEND_FACTOR_SRC_COLOR;
            case EBlendFactor::ONE_MINUS_SRC_COLOR:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case EBlendFactor::DST_COLOR:                return VK_BLEND_FACTOR_DST_COLOR;
            case EBlendFactor::ONE_MINUS_DST_COLOR:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case EBlendFactor::SRC_ALPHA:                return VK_BLEND_FACTOR_SRC_ALPHA;
            case EBlendFactor::ONE_MINUS_SRC_ALPHA:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case EBlendFactor::DST_ALPHA:                return VK_BLEND_FACTOR_DST_ALPHA;
            case EBlendFactor::ONE_MINUS_DST_ALPHA:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case EBlendFactor::CONSTANT_COLOR:           return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case EBlendFactor::ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case EBlendFactor::CONSTANT_ALPHA:           return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case EBlendFactor::ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case EBlendFactor::SRC_ALPHA_SATURATE:       return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        }
        return VK_BLEND_FACTOR_ZERO;
    }
    inline VkBlendOp toVk(EBlendOp o) noexcept
    {
        switch (o) {
            case EBlendOp::ADD:              return VK_BLEND_OP_ADD;
            case EBlendOp::SUBTRACT:         return VK_BLEND_OP_SUBTRACT;
            case EBlendOp::REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
            case EBlendOp::MIN:              return VK_BLEND_OP_MIN;
            case EBlendOp::MAX:              return VK_BLEND_OP_MAX;
        }
        return VK_BLEND_OP_ADD;
    }
    inline VkColorComponentFlags toVk(EColorComponent c) noexcept
    {
        return static_cast<VkColorComponentFlags>(c); // R/G/B/A bits match VK_COLOR_COMPONENT_*_BIT
    }
    inline VkShaderStageFlags toVkStages(EShaderStage s) noexcept
    {
        return static_cast<VkShaderStageFlags>(s); // bit layout matches VK_SHADER_STAGE_*_BIT
    }
    inline VkShaderStageFlagBits toVkStageBit(EShaderStage s) noexcept
    {
        return static_cast<VkShaderStageFlagBits>(s);
    }
    inline VkVertexInputRate toVk(EVertexInputRate r) noexcept
    {
        return r == EVertexInputRate::INSTANCE ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }

    /// The single converter the registerGraphics() facade calls.
    inline GraphicsPipelineTemplate fromDesc(const GraphicsPipelineDesc& d)
    {
        GraphicsPipelineTemplate t;
        t.geometry_type        = d.geometry_type;
        t.pipeline_layout      = d.pipeline_layout;
        t.descriptor_set_count = d.descriptor_set_count;
        t.active_sets_mask     = d.active_sets_mask;

        for (const auto& pc : d.push_constant_ranges)
            t.push_constant_ranges.push_back(VkPushConstantRange{ toVkStages(pc.stage_flags), pc.offset, pc.size });
        for (const auto& sm : d.resource_slot_map)
            t.resource_slot_map.push_back({ sm.slot, sm.set_index });

        t.vertex_shader   = d.vertex_shader;
        t.fragment_shader = d.fragment_shader;
        t.vertex_entry    = std::string(d.vertex_entry);
        t.fragment_entry  = std::string(d.fragment_entry);
        for (const auto& sv : d.specialization_values)
            t.specialization_values.push_back({ toVkStageBit(sv.stage), sv.constant_id, sv.value });

        for (const auto& vb : d.vertex_bindings)
            t.vertex_bindings.push_back(VkVertexInputBindingDescription{ vb.binding, vb.stride, toVk(vb.input_rate) });
        for (const auto& va : d.vertex_attributes)
            t.vertex_attributes.push_back(VkVertexInputAttributeDescription{ va.location, va.binding, convertTextureFormat(va.format), va.offset });

        t.topology     = toVk(d.topology);
        t.polygon_mode = toVk(d.polygon_mode);
        t.cull_mode    = toVk(d.cull_mode);
        t.front_face   = toVk(d.front_face);

        t.depth_test_enable  = d.depth_test_enable  ? VK_TRUE : VK_FALSE;
        t.depth_write_enable = d.depth_write_enable ? VK_TRUE : VK_FALSE;
        t.depth_compare_op   = toVk(d.depth_compare_op);
        t.depth_bias_enable  = d.depth_bias_enable  ? VK_TRUE : VK_FALSE;
        t.depth_bias_constant = d.depth_bias_constant;
        t.depth_bias_slope    = d.depth_bias_slope;

        t.blend_enable            = d.blend_enable ? VK_TRUE : VK_FALSE;
        t.src_color_blend_factor  = toVk(d.src_color_blend_factor);
        t.dst_color_blend_factor  = toVk(d.dst_color_blend_factor);
        t.color_blend_op          = toVk(d.color_blend_op);
        t.src_alpha_blend_factor  = toVk(d.src_alpha_blend_factor);
        t.dst_alpha_blend_factor  = toVk(d.dst_alpha_blend_factor);
        t.alpha_blend_op          = toVk(d.alpha_blend_op);
        t.color_write_mask        = toVk(d.color_write_mask);

        t.line_width           = d.line_width;
        t.use_dynamic_viewport = d.use_dynamic_viewport;
        t.use_dynamic_scissor  = d.use_dynamic_scissor;
        return t;
    }

} // namespace lux::render::detail
