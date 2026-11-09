#include <lux/engine/render/graph/RasterProgram.hpp>
#include <lux/engine/render/graph/Shader.hpp>
#include <lux/engine/render/device/Error.hpp>

#include <algorithm>

namespace lux::render::detail
{
    RenderResult<nri::Pipeline*> compileRasterProgram(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout& layout,
        const RasterProgramKey& program
    ) noexcept
    {
        const auto& graphics = program.state;
        const auto& colors = program.colors;
        const auto depth = program.depth;
        const auto samples = program.samples;
        const auto& limits = nri_core.GetDeviceDesc(device);
        auto stages = rasterShaderStages(program.shader, graphics, limits, enabled, colors.size(), depth);
        if (!stages)
        {
            return cxx::unexpected(stages.error());
        }
        const bool too_many_inputs = graphics.vertex_bindings.size() > limits.shaderStage.vertex.streamMaxNum ||
                                     graphics.vertex_attributes.size() > limits.shaderStage.vertex.attributeMaxNum ||
                                     graphics.vertex_bindings.size() > UINT8_MAX ||
                                     graphics.vertex_attributes.size() > UINT8_MAX;
        if (too_many_inputs)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        std::vector<nri::VertexStreamDesc> streams;
        std::vector<nri::VertexAttributeDesc> attributes;
        for (const auto& binding : graphics.vertex_bindings)
        {
            const bool invalid =
                !binding.stride || binding.stride > vertex_limits[0] || binding.stride > UINT16_MAX ||
                binding.binding >= limits.shaderStage.vertex.streamMaxNum || binding.binding > UINT16_MAX ||
                binding.rate > EVertexRate::INSTANCE ||
                std::ranges::any_of(streams, [&](const auto& other) { return other.bindingSlot == binding.binding; });
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            streams.push_back(
                {static_cast<std::uint16_t>(binding.binding),
                 binding.rate == EVertexRate::VERTEX ? nri::VertexStreamStepRate::PER_VERTEX
                                                     : nri::VertexStreamStepRate::PER_INSTANCE,
                 static_cast<std::uint16_t>(binding.stride)}
            );
        }
        for (const auto& field : graphics.vertex_attributes)
        {
            const auto format = vertexFormat(field.format);
            const auto binding = std::ranges::find(streams, field.binding, &nri::VertexStreamDesc::bindingSlot);
            const bool invalid =
                !format.components || field.location >= limits.shaderStage.vertex.attributeMaxNum ||
                field.offset > vertex_limits[1] || binding == streams.end() ||
                std::ranges::any_of(attributes, [&](const auto& other) { return other.vk.location == field.location; });
            if (invalid)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (std::uint64_t(field.offset) + format.bytes() > binding->stride)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if ((formats[static_cast<std::size_t>(format.native)] & nri::FormatSupportBits::VERTEX_BUFFER) == 0)
            {
                return cxx::unexpected(RenderError{kDeviceUnsupported});
            }
            attributes.push_back(
                {{},
                 {field.location},
                 field.offset,
                 format.native,
                 static_cast<std::uint16_t>(binding - streams.begin())}
            );
        }
        const auto& code = *stages;
        std::array<nri::ShaderDesc, 2> native_stages{};
        const auto stage_count = code[1] ? 2u : 1u;
        for (std::size_t i = 0; i < stage_count; ++i)
        {
            native_stages[i] = {
                i == 0 ? nri::StageBits::VERTEX_SHADER : nri::StageBits::FRAGMENT_SHADER,
                code[i]->words.data(),
                code[i]->words.size() * sizeof(std::uint32_t),
                code[i]->entry.c_str()
            };
        }
        const nri::VertexInputDesc input{
            attributes.data(),
            static_cast<std::uint8_t>(attributes.size()),
            streams.data(),
            static_cast<std::uint8_t>(streams.size())
        };
        constexpr std::array blend_factors{
            nri::BlendFactor::ZERO,
            nri::BlendFactor::ONE,
            nri::BlendFactor::SRC_ALPHA,
            nri::BlendFactor::ONE_MINUS_SRC_ALPHA
        };
        const auto& blend = graphics.blend;
        const bool invalid_blend = blend.source_color > EBlendFactor::ONE_MINUS_SOURCE_ALPHA ||
                                   blend.destination_color > EBlendFactor::ONE_MINUS_SOURCE_ALPHA ||
                                   blend.source_alpha > EBlendFactor::ONE_MINUS_SOURCE_ALPHA ||
                                   blend.destination_alpha > EBlendFactor::ONE_MINUS_SOURCE_ALPHA;
        if (invalid_blend)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::vector<nri::ColorAttachmentDesc> attachments;
        for (auto color : colors)
        {
            nri::ColorAttachmentDesc attachment{};
            attachment.format = color;
            attachment.colorWriteMask = nri::ColorWriteBits::RGBA;
            attachment.blendEnabled = blend.enabled;
            attachment.colorBlend = {
                blend_factors[static_cast<unsigned>(blend.source_color)],
                blend_factors[static_cast<unsigned>(blend.destination_color)],
                nri::BlendOp::ADD
            };
            attachment.alphaBlend = {
                blend_factors[static_cast<unsigned>(blend.source_alpha)],
                blend_factors[static_cast<unsigned>(blend.destination_alpha)],
                nri::BlendOp::ADD
            };
            attachments.push_back(attachment);
        }
        constexpr std::array comparisons{
            nri::CompareOp::NEVER,
            nri::CompareOp::LESS,
            nri::CompareOp::EQUAL,
            nri::CompareOp::LESS_EQUAL,
            nri::CompareOp::GREATER,
            nri::CompareOp::NOT_EQUAL,
            nri::CompareOp::GREATER_EQUAL,
            nri::CompareOp::ALWAYS
        };
        nri::GraphicsPipelineDesc description{};
        description.pipelineLayout = &layout;
        description.vertexInput = streams.empty() ? nullptr : &input;
        description.inputAssembly.topology = nri::Topology::TRIANGLE_LIST;
        constexpr std::array cull_modes{nri::CullMode::NONE, nri::CullMode::FRONT, nri::CullMode::BACK};
        description.rasterization.frontCounterClockwise = graphics.front_counter_clockwise;
        description.rasterization.cullMode = cull_modes[static_cast<std::size_t>(graphics.cull_mode)];
        description.rasterization.depthBias = {graphics.depth_bias_constant, 0, graphics.depth_bias_slope};
        const nri::MultisampleDesc multisample{UINT32_MAX, static_cast<nri::Sample_t>(samples)};
        description.multisample = samples > 1 ? &multisample : nullptr;
        description.outputMerger.colors = attachments.data();
        description.outputMerger.colorNum = static_cast<std::uint32_t>(attachments.size());
        description.outputMerger.depthStencilFormat = depth;
        description.outputMerger.depth.compareOp = graphics.depth_test
                                                       ? comparisons[static_cast<unsigned>(graphics.depth_compare)]
                                                   : graphics.depth_write ? nri::CompareOp::ALWAYS
                                                                          : nri::CompareOp::NONE;
        description.outputMerger.depth.write = graphics.depth_write;
        description.shaders = native_stages.data();
        description.shaderNum = stage_count;
        nri::Pipeline* pipeline = nullptr;
        const auto result = nri_core.CreateGraphicsPipeline(device, description, pipeline);
        if (result != nri::Result::SUCCESS)
        {
            nri_core.DestroyPipeline(pipeline);
            return cxx::unexpected(nativeError(result));
        }
        return pipeline;
    }
} // namespace lux::render::detail
