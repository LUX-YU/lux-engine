#pragma once

#include <lux/engine/render/vulkan/pipeline/Pipeline.hpp>
#include <vector>

namespace lux::render::vulkan
{
    struct VertexBinding
    {
        std::uint32_t binding{}, stride{};
        VkVertexInputRate rate{VK_VERTEX_INPUT_RATE_VERTEX};
        bool operator==(const VertexBinding&) const noexcept = default;
    };

    struct VertexAttribute
    {
        std::uint32_t location{}, binding{};
        VkFormat format{VK_FORMAT_UNDEFINED};
        std::uint32_t offset{};
        bool operator==(const VertexAttribute&) const noexcept = default;
    };

    struct StencilFace
    {
        VkStencilOp fail{VK_STENCIL_OP_KEEP}, pass{VK_STENCIL_OP_KEEP}, depth_fail{VK_STENCIL_OP_KEEP};
        VkCompareOp compare{VK_COMPARE_OP_ALWAYS};
        std::uint32_t compare_mask{0xff}, write_mask{0xff}, reference{};
        bool operator==(const StencilFace&) const noexcept = default;
    };

    struct ColorBlend
    {
        bool enabled{false};
        VkBlendFactor source_color{VK_BLEND_FACTOR_ONE}, destination_color{VK_BLEND_FACTOR_ZERO};
        VkBlendOp color{VK_BLEND_OP_ADD};
        VkBlendFactor source_alpha{VK_BLEND_FACTOR_ONE}, destination_alpha{VK_BLEND_FACTOR_ZERO};
        VkBlendOp alpha{VK_BLEND_OP_ADD};
        VkColorComponentFlags write_mask{0xf};
        bool operator==(const ColorBlend&) const noexcept = default;
    };

    struct GraphicsDescription
    {
        std::vector<VertexBinding> vertex_bindings;
        std::vector<VertexAttribute> attributes;
        std::vector<VkFormat> color_formats;
        std::vector<ColorBlend> blends;
        VkFormat depth_format{VK_FORMAT_UNDEFINED}, stencil_format{VK_FORMAT_UNDEFINED};
        VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
        VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkCullModeFlags cull_mode{VK_CULL_MODE_NONE};
        VkFrontFace front_face{VK_FRONT_FACE_COUNTER_CLOCKWISE};
        bool depth_test{false}, depth_write{false}, stencil_test{false};
        VkCompareOp depth_compare{VK_COMPARE_OP_LESS};
        StencilFace front, back;
        bool operator==(const GraphicsDescription&) const noexcept = default;
    };

    // Native mechanism only: immutable fixed graphics state, dynamic viewport/scissor.
    class GraphicsPipeline
    {
    public:
        [[nodiscard]] static RenderResult<GraphicsPipeline> create(
            const VulkanDevice& device,
            const ShaderModule& vertex,
            const ShaderModule& fragment,
            const PipelineLayout& layout,
            const GraphicsDescription& description
        ) noexcept;
        ~GraphicsPipeline() noexcept;
        GraphicsPipeline(GraphicsPipeline&& other) noexcept;
        GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;
        GraphicsPipeline(const GraphicsPipeline&) = delete;
        GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

        [[nodiscard]] VkPipeline native() const noexcept
        {
            return handle_;
        }

        [[nodiscard]] VkDevice device() const noexcept
        {
            return device_;
        }

    private:
        GraphicsPipeline(VkDevice device, VkPipeline handle) noexcept;
        void release() noexcept;
        VkDevice device_;
        VkPipeline handle_;
    };
} // namespace lux::render::vulkan
