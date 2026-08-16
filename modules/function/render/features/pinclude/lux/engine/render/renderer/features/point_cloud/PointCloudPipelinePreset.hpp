#pragma once
/**
 * @file PointCloudPipelinePreset.hpp
 * @brief Point-cloud graphics-pipeline preset.
 *
 * Lives with the point-cloud feature (not in pipeline/PipelinePresets.hpp): the
 * preset hard-codes the GpuPointVertex layout, so it BELONGS to the point-cloud
 * domain. Keeping it here stops the generic pipeline layer from #including a
 * feature-domain header ("core stays domain-free"). Shared by the 5 PC feature
 * variants (Simple/LOD/Splatting/Transient/GPUDriven).
 */

#include <lux/engine/render/gpu/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp> // GpuPointVertex
#include <vulkan/vulkan.h>

namespace lux::render
{
    /**
     * @brief Point-cloud render preset.
     *
     * Vertex: GpuPointVertex layout (xyz + packed_attr).
     * Topology: POINT_LIST, no cull, depth write on, no blend.
     * Still to set: vertex_shader, fragment_shader, descriptor_set_count.
     */
    inline GraphicsPipelineTemplate makePointCloudTemplate()
    {
        GraphicsPipelineTemplate tmpl{};
        tmpl.geometry_type = EGeometryType::POINT_CLOUD;

        VkVertexInputBindingDescription pcb{};
        pcb.binding   = 0;
        pcb.stride    = static_cast<uint32_t>(sizeof(GpuPointVertex));
        pcb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        tmpl.vertex_bindings = { pcb };

        tmpl.vertex_attributes = {
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuPointVertex, x)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT,          offsetof(GpuPointVertex, packed_attr)},
        };

        tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
        tmpl.cull_mode          = VK_CULL_MODE_NONE;
        tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        tmpl.depth_test_enable   = VK_TRUE;
        tmpl.depth_write_enable  = VK_TRUE;
        tmpl.depth_compare_op    = VK_COMPARE_OP_LESS_OR_EQUAL;
        tmpl.blend_enable        = VK_FALSE;
        tmpl.src_color_blend_factor = VK_BLEND_FACTOR_ONE;
        tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ZERO;
        tmpl.color_blend_op      = VK_BLEND_OP_ADD;
        tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ZERO;
        tmpl.alpha_blend_op      = VK_BLEND_OP_ADD;
        tmpl.color_write_mask    = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tmpl.use_dynamic_viewport = true;
        tmpl.use_dynamic_scissor  = true;
        return tmpl;
    }

} // namespace lux::render
