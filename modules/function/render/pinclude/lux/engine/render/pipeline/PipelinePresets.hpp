#pragma once
/**
 * @file PipelinePresets.hpp
 * @brief Factory helpers that build fully-configured GraphicsPipelineTemplate
 *        instances for common render configurations.
 *
 * Eliminates repeated ~20-field initialisations across every feature's
 * registerPipelines() and RenderPipelineSetup::registerPipelineTemplates().
 *
 * Usage pattern:
 *
 *   auto tmpl = makeOpaqueMeshTemplate();
 *   tmpl.descriptor_set_count = 5;
 *   tmpl.vertex_shader   = vert.module;
 *   tmpl.fragment_shader = frag.module;
 *   handle = pm.registerGraphicsTemplate(tmpl, infos);
 */

#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <vulkan/vulkan.h>

// NOTE: feature-domain presets that hard-code a feature's vertex layout
// (point-cloud / trajectory / gizmo) do NOT live here — they would force this
// generic pipeline header to #include feature-domain headers ("core stays
// domain-free"). They live with their owning feature instead:
//   - makePointCloudTemplate → features/point_cloud/PointCloudPipelinePreset.hpp
//   - trajectory / gizmo presets → file-local in the owning feature .cpp

namespace lux::render
{

// ---------------------------------------------------------------------------
// Vertex layout helpers
// ---------------------------------------------------------------------------

/// @brief Fill standard binding + attribute descriptions for lux::rdesc::Vertex.
inline void fillMeshVertexInput(
    lux::cxx::SmallVector<VkVertexInputBindingDescription,  4>& bindings,
    lux::cxx::SmallVector<VkVertexInputAttributeDescription, 8>& attributes)
{
    VkVertexInputBindingDescription b{};
    b.binding   = 0;
    b.stride    = static_cast<uint32_t>(sizeof(lux::rdesc::Vertex));
    b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings = { b };

    attributes = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(lux::rdesc::Vertex, position)},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(lux::rdesc::Vertex, normal)},
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(lux::rdesc::Vertex, tangent)},
        VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(lux::rdesc::Vertex, uv)},
        VkVertexInputAttributeDescription{4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(lux::rdesc::Vertex, bitangent)},
    };
}

// ---------------------------------------------------------------------------
// Pipeline template presets
// ---------------------------------------------------------------------------

/**
 * @brief Opaque forward mesh preset.
 *
 * Rasterisation: triangle-list, fill, back-face cull, CCW winding.
 * Depth:  test=TRUE, write=TRUE, compare=LESS_OR_EQUAL.
 * Blend:  disabled (opaque; src=ONE, dst=ZERO).
 * Vertex: standard lux::rdesc::Vertex layout.
 *
 * Still to set: vertex_shader, fragment_shader, descriptor_set_count.
 */
inline GraphicsPipelineTemplate makeOpaqueMeshTemplate()
{
    GraphicsPipelineTemplate tmpl{};
    fillMeshVertexInput(tmpl.vertex_bindings, tmpl.vertex_attributes);
    tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
    tmpl.cull_mode          = VK_CULL_MODE_BACK_BIT;
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

/**
 * @brief Depth-only prepass preset.
 *
 * Inherits opaque settings; overrides:
 *   depth_compare=LESS (strict), color_write_mask=0 (no color output).
 */
inline GraphicsPipelineTemplate makeDepthPrepassTemplate()
{
    GraphicsPipelineTemplate tmpl = makeOpaqueMeshTemplate();
    tmpl.depth_compare_op = VK_COMPARE_OP_LESS;
    tmpl.blend_enable     = VK_FALSE;
    tmpl.color_write_mask = 0;
    return tmpl;
}

/**
 * @brief Infinite-grid overlay preset.
 *
 * No vertex input (procedural fullscreen pass).
 * Rasterisation: triangle-list, fill, cull=NONE, CCW.
 * Depth:  test=TRUE, write=FALSE, compare=LESS_OR_EQUAL.
 * Blend:  alpha (src_alpha / 1-src_alpha).
 */
inline GraphicsPipelineTemplate makeGridTemplate()
{
    GraphicsPipelineTemplate tmpl{};
    // no vertex input
    tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
    tmpl.cull_mode          = VK_CULL_MODE_NONE;
    tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    tmpl.depth_test_enable   = VK_TRUE;
    tmpl.depth_write_enable  = VK_FALSE;
    tmpl.depth_compare_op    = VK_COMPARE_OP_LESS_OR_EQUAL;
    tmpl.blend_enable        = VK_TRUE;
    tmpl.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
    tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.color_blend_op      = VK_BLEND_OP_ADD;
    tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
    tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.alpha_blend_op      = VK_BLEND_OP_ADD;
    tmpl.color_write_mask    = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    tmpl.use_dynamic_viewport = true;
    tmpl.use_dynamic_scissor  = true;
    return tmpl;
}

/**
 * @brief Transparent (alpha-blended) mesh preset.
 *
 * Same vertex layout as opaque forward mesh.
 * Depth: test=TRUE, write=FALSE (do not occlude later fragments).
 * Blend: standard alpha — SRC_ALPHA / ONE_MINUS_SRC_ALPHA.
 */
inline GraphicsPipelineTemplate makeTransparentMeshTemplate()
{
    GraphicsPipelineTemplate tmpl = makeOpaqueMeshTemplate();
    tmpl.depth_write_enable     = VK_FALSE;
    tmpl.blend_enable           = VK_TRUE;
    tmpl.src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
    tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.color_blend_op         = VK_BLEND_OP_ADD;
    tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
    tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    tmpl.alpha_blend_op         = VK_BLEND_OP_ADD;
    return tmpl;
}

/**
 * @brief Octree debug wireframe preset.
 *
 * Renders AABB wireframes for octree nodes via LINE_LIST topology.
 * Vertex format: OctreeWireframeVertex (vec3 position + uint depth_level).
 *
 * Depth:  test=TRUE (LESS_OR_EQUAL), write=FALSE — overlay on existing geometry.
 * Blend:  disabled (fully opaque lines).
 * Cull:   NONE (lines have no face).
 */
inline GraphicsPipelineTemplate makeOctreeWireframeTemplate()
{
    GraphicsPipelineTemplate tmpl{};

    // Vertex input: OctreeWireframeVertex — 16 bytes
    VkVertexInputBindingDescription b{};
    b.binding   = 0;
    b.stride    = sizeof(float) * 3 + sizeof(uint32_t); // 16 bytes
    b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    tmpl.vertex_bindings = { b };

    tmpl.vertex_attributes = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT,         sizeof(float) * 3},
    };

    tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
    tmpl.cull_mode          = VK_CULL_MODE_NONE;
    tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    tmpl.depth_test_enable   = VK_TRUE;
    tmpl.depth_write_enable  = VK_FALSE;
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

/**
 * @brief Bounding-box selection wireframe preset.
 *
 * Instance-rate vertex input: GpuAABBEntry (32 bytes per AABB).
 *   loc 0 — vec3  inMin     (offset  0)
 *   loc 1 — uint  inColorId (offset 12)
 *   loc 2 — vec3  inMax     (offset 16)
 *   loc 3 — uint  inActive  (offset 28)
 *
 * The vertex shader uses gl_VertexIndex (0..23) to procedurally generate
 * the 24 line-list edge endpoints from the per-instance AABB data.
 * Inactive entries (inActive==0) produce degenerate (clipped) vertices.
 *
 * Depth:  test=TRUE (LESS_OR_EQUAL), write=FALSE.
 * Blend:  disabled.
 * Cull:   NONE.
 */
inline GraphicsPipelineTemplate makeBBoxWireframeTemplate()
{
    GraphicsPipelineTemplate tmpl{};

    // One per-instance binding: GpuAABBEntry — 32 bytes, one per AABB
    VkVertexInputBindingDescription b{};
    b.binding   = 0;
    b.stride    = 32; // sizeof(GpuAABBEntry)
    b.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    tmpl.vertex_bindings = { b };

    tmpl.vertex_attributes = {
        VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,  0},  // inMin
        VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32_UINT,         12},  // inColorId
        VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32_SFLOAT, 16},  // inMax
        VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32_UINT,         28},  // inActive
    };

    tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    tmpl.polygon_mode       = VK_POLYGON_MODE_FILL;
    tmpl.cull_mode          = VK_CULL_MODE_NONE;
    tmpl.front_face         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    tmpl.depth_test_enable   = VK_TRUE;
    tmpl.depth_write_enable  = VK_FALSE;
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
