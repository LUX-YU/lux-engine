#pragma once
/**
 * @file GraphicsPipelineDesc.hpp
 * @brief RHI-neutral graphics-pipeline description an external RenderFeature fills.
 *
 * SmallVector-free, <vulkan/vulkan.h>-free public mirror of the engine-internal
 * GraphicsPipelineTemplate (sinclude). The author fills this with neutral enums +
 * std::span; the engine converts it to GraphicsPipelineTemplate via fromDesc()
 * before registration (PipelineManager). Vulkan HANDLES (pipeline_layout, shader
 * modules) are forward-declared via core/vk_fwd.hpp — no full Vulkan header.
 *
 * Fields the engine DERIVES from shader reflection (rdesc::ShaderInfo) — leave
 * empty for standard pipelines: push_constant_ranges, resource_slot_map,
 * active_sets_mask. Fields the author MUST supply: vertex bindings/attributes,
 * raster/depth/blend state, specialization values.
 */

#include <cstdint>
#include <span>
#include <string_view>

#include <lux/engine/common/ImageEnums.hpp>                 // lux::common::ETextureFormat (vertex attr formats)
#include <lux/engine/render/core/vk_fwd.hpp>                // VkPipelineLayout, VkShaderModule (fwd-declared)
#include <lux/engine/render/core/Geometry.hpp>             // EGeometryType (already public, neutral)
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>  // EDescriptorSetSlot (already public, neutral)
#include <lux/engine/render/pipeline/PipelineStateEnums.hpp>

namespace lux::render
{
    // ── Author-supplied neutral sub-structs ──────────────────────────────

    struct PushConstantRangeDesc
    {
        EShaderStage stage_flags = EShaderStage::ALL_GRAPHICS;
        uint32_t     offset      = 0;
        uint32_t     size        = 0;
    };

    /// Maps a named engine descriptor-set slot to the Vulkan set index this
    /// pipeline actually binds it at. Only needed for NON-standard layouts;
    /// leave the span empty to let the engine fill the identity mapping.
    struct SlotMapEntry
    {
        EDescriptorSetSlot slot;
        uint32_t           set_index;
    };

    struct SpecConstantValue
    {
        EShaderStage stage       = EShaderStage::FRAGMENT;
        uint32_t     constant_id = 0;
        uint32_t     value       = 0;
    };

    struct VertexBindingDesc
    {
        uint32_t         binding    = 0;
        uint32_t         stride     = 0;
        EVertexInputRate input_rate = EVertexInputRate::VERTEX;
    };

    struct VertexAttributeDesc
    {
        uint32_t                   location = 0;
        uint32_t                   binding  = 0;
        lux::common::ETextureFormat format  = lux::common::ETextureFormat::RGBA32_SFLOAT;
        uint32_t                   offset   = 0;
    };

    // ── The neutral pipeline description ──────────────────────────────────

    struct GraphicsPipelineDesc
    {
        EGeometryType    geometry_type        = EGeometryType::MESH;
        VkPipelineLayout pipeline_layout      = nullptr;  // null => engine builds standard layout from ShaderInfo
        uint32_t         descriptor_set_count = 0;
        uint32_t         active_sets_mask     = 0;        // 0 => reflect from ShaderInfo

        // -- author OPTIONAL: leave empty to let reflection fill (standard layouts) --
        std::span<const PushConstantRangeDesc> push_constant_ranges{};  // empty => from ShaderInfo
        std::span<const SlotMapEntry>          resource_slot_map{};     // empty => identity from active_sets_mask

        // -- shaders --
        VkShaderModule   vertex_shader   = nullptr;
        VkShaderModule   fragment_shader = nullptr;
        std::string_view vertex_entry    = "main";
        std::string_view fragment_entry  = "main";
        std::span<const SpecConstantValue> specialization_values{};

        // -- author MUST supply (not derivable from reflection) --
        std::span<const VertexBindingDesc>   vertex_bindings{};
        std::span<const VertexAttributeDesc> vertex_attributes{};

        EPrimitiveTopology topology     = EPrimitiveTopology::TRIANGLE_LIST;
        EPolygonMode       polygon_mode = EPolygonMode::FILL;
        ECullMode          cull_mode    = ECullMode::BACK;
        EFrontFace         front_face   = EFrontFace::COUNTER_CLOCKWISE;

        bool       depth_test_enable  = true;
        bool       depth_write_enable = true;
        ECompareOp depth_compare_op   = ECompareOp::LESS_OR_EQUAL;

        bool  depth_bias_enable   = false;
        float depth_bias_constant = 0.0f;
        float depth_bias_slope    = 0.0f;

        bool         blend_enable            = false;
        EBlendFactor src_color_blend_factor  = EBlendFactor::SRC_ALPHA;
        EBlendFactor dst_color_blend_factor  = EBlendFactor::ONE_MINUS_SRC_ALPHA;
        EBlendOp     color_blend_op          = EBlendOp::ADD;
        EBlendFactor src_alpha_blend_factor  = EBlendFactor::ONE;
        EBlendFactor dst_alpha_blend_factor  = EBlendFactor::ONE_MINUS_SRC_ALPHA;
        EBlendOp     alpha_blend_op          = EBlendOp::ADD;
        EColorComponent color_write_mask     = EColorComponent::RGBA;

        float line_width = 1.0f;
        bool  use_dynamic_viewport = true;
        bool  use_dynamic_scissor  = true;
    };

} // namespace lux::render
