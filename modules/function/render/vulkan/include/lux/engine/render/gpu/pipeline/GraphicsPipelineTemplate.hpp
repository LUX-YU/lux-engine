#pragma once
#include <lux/engine/render/gpu/pipeline/Geometry.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <utility>

#include <lux/cxx/container/SmallVector.hpp>

#include <lux/engine/function/render/client/core/PipelineHandle.hpp>   // Graphics/ComputePipelineHandle (moved to public, SDK P1)

namespace lux::render
{
    struct GraphicsPipelineTemplate
    {
        // ========== Geometry type tag ==========
        // Used by PipelineManager to index templates by geometry category.
        EGeometryType geometry_type = EGeometryType::MESH;

        /// Debug name (prefix for the layout debug name on the reflected-layout
        /// path; may be empty).
        std::string      debug_name{};

        /// Explicitly specify the layout for certain sets — for sets that are
        /// FEATURE-OWNED and SHARED ACROSS PIPELINES: their shape is declared by
        /// the feature (e.g. the GPU-driven cull set, the clustered cluster set),
        /// and multiple pipelines each use only a subset of its bindings, so the
        /// layout can't be inferred by reflecting any single pipeline (a
        /// subset layout is incompatible with a set allocated for the full shape
        /// → VUID-00358). Sets not listed here still go through contract routing /
        /// reflection-based construction. Only takes effect on the reflected-layout
        /// path.
        lux::cxx::SmallVector<std::pair<uint32_t, VkDescriptorSetLayout>, 4> explicit_set_layouts;

        VkPipelineLayout pipeline_layout    = VK_NULL_HANDLE;
        uint32_t         descriptor_set_count = 0;

        // Mask of active descriptor sets used by this pipeline.
        uint32_t         active_sets_mask = 0;

        /// Push constant ranges reflected from the pipeline's shaders.
        /// Used at record time to determine the correct stageFlags for vkCmdPushConstants.
        lux::cxx::SmallVector<VkPushConstantRange, 2> push_constant_ranges;

        /// Explicit mapping from named resource type (EDescriptorSetSlot) to the
        /// Vulkan descriptor set slot index actually used by this pipeline.
        ///
        /// For standard pipelines following the global convention
        /// (Scene=0, Instance=1, ...), this is the identity mapping and is
        /// populated automatically by PipelineManager::registerGraphicsTemplate.
        ///
        /// Custom pipelines can set non-standard entries (e.g. Texture at slot 1)
        /// before registering; the automatic population is skipped when non-empty.
        ///
        /// RGVulkanRecorder reads this map in Step 3b to call
        /// vkCmdBindDescriptorSets with exactly the right (slot, set) pairs.
        lux::cxx::SmallVector<std::pair<EDescriptorSetSlot, uint32_t>, kDescriptorSetCount> resource_slot_map;

        // Shader modules and entry point names
        VkShaderModule vertex_shader   = VK_NULL_HANDLE;
        VkShaderModule fragment_shader = VK_NULL_HANDLE;
        std::string    vertex_entry    = "main";
        std::string    fragment_entry  = "main";

        struct ShaderSpecializationValue
        {
            VkShaderStageFlagBits stage{VK_SHADER_STAGE_FRAGMENT_BIT};
            uint32_t constant_id{0};
            uint32_t value{0};
        };
        lux::cxx::SmallVector<ShaderSpecializationValue, 4> specialization_values;

        // Vertex input descriptions
        lux::cxx::SmallVector<VkVertexInputBindingDescription, 4>   vertex_bindings;
        lux::cxx::SmallVector<VkVertexInputAttributeDescription, 8> vertex_attributes;

        // Topology and rasterization state
        VkPrimitiveTopology topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPolygonMode       polygon_mode = VK_POLYGON_MODE_FILL;
        VkCullModeFlags     cull_mode    = VK_CULL_MODE_BACK_BIT;
        VkFrontFace         front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // Depth test state
        VkBool32    depth_test_enable   = VK_TRUE;
        VkBool32    depth_write_enable  = VK_TRUE;
        VkCompareOp depth_compare_op    = VK_COMPARE_OP_LESS_OR_EQUAL;

        // Depth bias (shadow mapping)
        VkBool32    depth_bias_enable   = VK_FALSE;
        float       depth_bias_constant = 0.0f;
        float       depth_bias_slope    = 0.0f;

        // Color blending state (applied to all color attachments)
        VkBool32 blend_enable              = VK_FALSE;
        VkBlendFactor src_color_blend_factor = VK_BLEND_FACTOR_SRC_ALPHA;
        VkBlendFactor dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkBlendOp     color_blend_op        = VK_BLEND_OP_ADD;
        VkBlendFactor src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        VkBlendFactor dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkBlendOp     alpha_blend_op         = VK_BLEND_OP_ADD;

        VkColorComponentFlags color_write_mask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        // Line width (requires wideLines device feature for values != 1.0)
        float line_width = 1.0f;

        // Dynamic states
        bool use_dynamic_viewport = true;
        bool use_dynamic_scissor  = true;

        // ── Local-read merged-scope remaps ─────────────────
        // For pipelines drawing inside a KHR_dynamic_rendering_local_read
        // merged scope. EMPTY = plain pipeline, nothing chained (today's
        // behaviour). When set, sizes must equal the scope's color attachment
        // count and MATCH the LocalReadBoundary command-buffer state at draw
        // time (identical arrays on both sides — the planner's per-pass maps
        // are the single source both consume).
        /// scope color slot → fragment output location (VK_ATTACHMENT_UNUSED
        /// = this pipeline does not write that slot).
        std::vector<uint32_t> lr_color_locations{};
        /// scope color slot → input attachment index (VK_ATTACHMENT_UNUSED =
        /// not read by this pipeline).
        std::vector<uint32_t> lr_input_indices{};
        /// Input attachment index for the scope depth (VK_ATTACHMENT_UNUSED = none).
        uint32_t lr_depth_input_index = VK_ATTACHMENT_UNUSED;
    };
} // namespace lux::render
