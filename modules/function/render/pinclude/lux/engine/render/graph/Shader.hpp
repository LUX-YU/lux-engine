#pragma once

#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <NRI.h>
#include <array>
#include <lux/engine/render/graph/Work.hpp>

namespace lux::render::detail
{
    struct VertexFormat final
    {
        nri::Format native = nri::Format::UNKNOWN;
        rdesc::EDataScalar scalar = rdesc::EDataScalar::FLOAT;
        std::uint32_t components = 0;
        std::uint32_t component_bytes = 4; // Storage width; normalized attributes still feed float Shader values.

        [[nodiscard]] std::uint32_t bytes() const noexcept
        {
            return components * component_bytes;
        }
    };

    [[nodiscard]] VertexFormat vertexFormat(rdesc::ETextureFormat) noexcept;
    struct EnabledDeviceFeatures;
    [[nodiscard]] nri::StageBits shaderStages(std::uint32_t stages) noexcept;
    [[nodiscard]] std::vector<nri::DescriptorRangeDesc> descriptorRanges(
        std::span<const ShaderBinding>,
        std::uint32_t image_count,
        std::uint32_t sampler_count
    ) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*>
    compileCompute(nri::Device&, const nri::CoreInterface&, const EnabledDeviceFeatures&, nri::PipelineLayout&, const rdesc::SpirvArtifact&) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*> compileGraphics(
        nri::Device&,
        const nri::CoreInterface&,
        const EnabledDeviceFeatures&,
        std::span<const nri::FormatSupportBits>,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout&,
        const rdesc::SpirvArtifact&,
        const GraphicsState&,
        std::span<const nri::Format> colors,
        nri::Format depth,
        std::uint32_t samples
    ) noexcept;
} // namespace lux::render::detail
