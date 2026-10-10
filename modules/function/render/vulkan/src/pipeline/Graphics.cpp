#include <lux/engine/render/vulkan/pipeline/Graphics.hpp>

#include "Native.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace lux::render::vulkan
{
    namespace
    {
        bool validStencil(const StencilFace& value) noexcept
        {
            return value.fail >= VK_STENCIL_OP_KEEP && value.fail <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
                   value.pass >= VK_STENCIL_OP_KEEP && value.pass <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
                   value.depth_fail >= VK_STENCIL_OP_KEEP && value.depth_fail <= VK_STENCIL_OP_DECREMENT_AND_WRAP &&
                   value.compare >= VK_COMPARE_OP_NEVER && value.compare <= VK_COMPARE_OP_ALWAYS;
        }

        VkStencilOpState stencil(const StencilFace& value) noexcept
        {
            return {
                value.fail,
                value.pass,
                value.depth_fail,
                value.compare,
                value.compare_mask,
                value.write_mask,
                value.reference
            };
        }

        bool supportsImage(
            const VulkanDevice& device,
            VkFormat format,
            VkImageUsageFlags usage,
            VkSampleCountFlagBits samples
        ) noexcept
        {
            VkImageFormatProperties properties{};
            return vkGetPhysicalDeviceImageFormatProperties(
                       device.physical(),
                       format,
                       VK_IMAGE_TYPE_2D,
                       VK_IMAGE_TILING_OPTIMAL,
                       usage,
                       0,
                       &properties
                   ) == VK_SUCCESS &&
                   (properties.sampleCounts & samples) != 0;
        }

        bool hasDepth(VkFormat format) noexcept
        {
            return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                   format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
                   format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        }

        bool hasStencil(VkFormat format) noexcept
        {
            return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
                   format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        }
    } // namespace

    RenderResult<GraphicsPipeline> GraphicsPipeline::create(
        const VulkanDevice& device,
        const ShaderModule& vertex,
        const ShaderModule& fragment,
        const PipelineLayout& layout,
        const GraphicsDescription& description
    ) noexcept
    {
        const bool wrong_owner = vertex.device() != device.native() || fragment.device() != device.native() ||
                                 layout.device() != device.native();
        if (wrong_owner)
        {
            return cxx::unexpected(RenderError{kWrongOwner});
        }
        if (!device.dynamicRendering())
        {
            return cxx::unexpected(RenderError{kUnsupported});
        }
        const auto& limits = device.properties().limits;
        const bool invalid_counts = description.color_formats.size() != description.blends.size() ||
                                    description.color_formats.size() > limits.maxColorAttachments ||
                                    description.vertex_bindings.size() > limits.maxVertexInputBindings ||
                                    description.attributes.size() > limits.maxVertexInputAttributes;
        const bool invalid_raster = description.topology < VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
                                    description.topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                                    (description.cull_mode & ~VK_CULL_MODE_FRONT_AND_BACK) != 0 ||
                                    description.front_face < VK_FRONT_FACE_COUNTER_CLOCKWISE ||
                                    description.front_face > VK_FRONT_FACE_CLOCKWISE;
        const auto samples = static_cast<std::uint32_t>(description.samples);
        const bool invalid_samples = samples == 0 || (samples & (samples - 1)) != 0;
        const bool invalid_depth =
            (description.depth_test && description.depth_format == VK_FORMAT_UNDEFINED) ||
            (description.stencil_test && description.stencil_format == VK_FORMAT_UNDEFINED) ||
            (description.depth_format != VK_FORMAT_UNDEFINED && !hasDepth(description.depth_format)) ||
            (description.stencil_format != VK_FORMAT_UNDEFINED && !hasStencil(description.stencil_format)) ||
            description.depth_compare < VK_COMPARE_OP_NEVER || description.depth_compare > VK_COMPARE_OP_ALWAYS ||
            !validStencil(description.front) || !validStencil(description.back);
        if (invalid_counts || invalid_raster || invalid_samples || invalid_depth)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        for (const auto& binding : description.vertex_bindings)
        {
            const bool invalid_binding =
                binding.binding >= limits.maxVertexInputBindings ||
                binding.stride > limits.maxVertexInputBindingStride ||
                (binding.rate != VK_VERTEX_INPUT_RATE_VERTEX && binding.rate != VK_VERTEX_INPUT_RATE_INSTANCE) ||
                std::any_of(
                    bindings.begin(),
                    bindings.end(),
                    [&](const auto& prior) { return prior.binding == binding.binding; }
                );
            if (invalid_binding)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            bindings.push_back({binding.binding, binding.stride, binding.rate});
        }
        for (const auto& attribute : description.attributes)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.physical(), attribute.format, &properties);
            const bool invalid_attribute = attribute.location >= limits.maxVertexInputAttributes ||
                                           attribute.offset > limits.maxVertexInputAttributeOffset ||
                                           std::none_of(
                                               bindings.begin(),
                                               bindings.end(),
                                               [&](const auto& b) { return b.binding == attribute.binding; }
                                           ) ||
                                           std::any_of(
                                               attributes.begin(),
                                               attributes.end(),
                                               [&](const auto& a) { return a.location == attribute.location; }
                                           );
            if (invalid_attribute)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            if ((properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
            attributes.push_back({attribute.location, attribute.binding, attribute.format, attribute.offset});
        }
        std::vector<VkPipelineColorBlendAttachmentState> blends;
        for (std::size_t i = 0; i < description.color_formats.size(); ++i)
        {
            const auto format = description.color_formats[i];
            const auto& blend = description.blends[i];
            if (!supportsImage(device, format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, description.samples))
            {
                return cxx::unexpected(RenderError{kUnsupported, {i}});
            }
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device.physical(), format, &properties);
            const bool unsupported_blend =
                blend.enabled && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) == 0;
            const bool needs_independent_blend = i != 0 && blend != description.blends[0];
            if (unsupported_blend || needs_independent_blend)
            {
                return cxx::unexpected(RenderError{kUnsupported, {i}});
            }
            const bool invalid_blend =
                (blend.write_mask & ~0xfu) != 0 || blend.color < VK_BLEND_OP_ADD || blend.color > VK_BLEND_OP_MAX ||
                blend.alpha < VK_BLEND_OP_ADD || blend.alpha > VK_BLEND_OP_MAX ||
                blend.source_color < VK_BLEND_FACTOR_ZERO || blend.source_color > VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
                blend.destination_color < VK_BLEND_FACTOR_ZERO ||
                blend.destination_color > VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
                blend.source_alpha < VK_BLEND_FACTOR_ZERO || blend.source_alpha > VK_BLEND_FACTOR_SRC_ALPHA_SATURATE ||
                blend.destination_alpha < VK_BLEND_FACTOR_ZERO ||
                blend.destination_alpha > VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            if (invalid_blend)
            {
                return cxx::unexpected(RenderError{kInvalidArgument, {i}});
            }
            blends.push_back(
                {blend.enabled,
                 blend.source_color,
                 blend.destination_color,
                 blend.color,
                 blend.source_alpha,
                 blend.destination_alpha,
                 blend.alpha,
                 blend.write_mask}
            );
        }
        for (auto format : {description.depth_format, description.stencil_format})
        {
            if (format != VK_FORMAT_UNDEFINED &&
                !supportsImage(device, format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, description.samples))
            {
                return cxx::unexpected(RenderError{kUnsupported});
            }
        }
        const bool incompatible_depth_stencil = description.depth_format != VK_FORMAT_UNDEFINED &&
                                                description.stencil_format != VK_FORMAT_UNDEFINED &&
                                                description.depth_format != description.stencil_format;
        if (incompatible_depth_stencil)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        const std::array stages{
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_VERTEX_BIT,
                vertex.native(),
                "main",
                nullptr
            },
            VkPipelineShaderStageCreateInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                fragment.native(),
                "main",
                nullptr
            }
        };
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
        input.pVertexBindingDescriptions = bindings.data();
        input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = description.topology;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = description.cull_mode;
        raster.frontFace = description.front_face;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = description.samples;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = description.depth_test;
        depth.depthWriteEnable = description.depth_write;
        depth.depthCompareOp = description.depth_compare;
        depth.stencilTestEnable = description.stencil_test;
        depth.front = stencil(description.front);
        depth.back = stencil(description.back);
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = static_cast<std::uint32_t>(blends.size());
        blend.pAttachments = blends.data();
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = static_cast<std::uint32_t>(description.color_formats.size());
        rendering.pColorAttachmentFormats = description.color_formats.data();
        rendering.depthAttachmentFormat = description.depth_format;
        rendering.stencilAttachmentFormat = description.stencil_format;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = static_cast<std::uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout.native();
        VkPipeline pipeline{};
        const auto result = LUX_NATIVE(
            "graphics_pipeline",
            vkCreateGraphicsPipelines(device.native(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline)
        );
        if (result != VK_SUCCESS)
        {
            if (pipeline)
            {
                LUX_DESTROY("graphics_pipeline", vkDestroyPipeline(device.native(), pipeline, nullptr));
            }
            return cxx::unexpected(nativeError(result));
        }
        return GraphicsPipeline{device.native(), pipeline};
    }

    GraphicsPipeline::GraphicsPipeline(VkDevice device, VkPipeline handle) noexcept : device_(device), handle_(handle)
    {
    }

    void GraphicsPipeline::release() noexcept
    {
        if (handle_)
        {
            LUX_DESTROY("graphics_pipeline", vkDestroyPipeline(device_, handle_, nullptr));
        }
    }

    GraphicsPipeline::~GraphicsPipeline() noexcept
    {
        release();
    }

    GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
        : device_(other.device_), handle_(std::exchange(other.handle_, VK_NULL_HANDLE))
    {
    }

    GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
    {
        if (this != &other)
        {
            release();
            device_ = other.device_;
            handle_ = std::exchange(other.handle_, VK_NULL_HANDLE);
        }
        return *this;
    }
} // namespace lux::render::vulkan
