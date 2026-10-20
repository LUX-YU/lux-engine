#pragma once

#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <NRI.h>
#include <array>
#include <lux/engine/render/graph/Program.hpp>

namespace lux::render::detail
{
    struct VertexFormat final
    {
        nri::Format native = nri::Format::UNKNOWN;
        rdesc::EDataScalar scalar = rdesc::EDataScalar::FLOAT;
        std::uint32_t components = 0;

        [[nodiscard]] std::uint32_t bytes() const noexcept
        {
            return components * 4;
        }
    };

    [[nodiscard]] VertexFormat vertexFormat(rdesc::ETextureFormat) noexcept;
    struct EnabledDeviceFeatures;
    [[nodiscard]] nri::StageBits shaderStages(std::uint32_t stages) noexcept;
    [[nodiscard]] std::vector<nri::DescriptorRangeDesc> descriptorRanges(
        std::span<const ShaderBinding>,
        std::uint32_t runtime_count
    ) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*>
    compileCompute(nri::Device&, const nri::CoreInterface&, nri::PipelineLayout&, const rdesc::SpirvArtifact&) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*> compileGraphics(
        nri::Device&,
        const nri::CoreInterface&,
        const EnabledDeviceFeatures&,
        std::span<const nri::FormatSupportBits>,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout&,
        const rdesc::SpirvArtifact&,
        const GraphicsState&,
        nri::Format color,
        nri::Format depth
    ) noexcept;
} // namespace lux::render::detail
