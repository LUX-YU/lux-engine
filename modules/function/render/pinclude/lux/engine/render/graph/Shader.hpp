#pragma once

#include <lux/engine/render/graph/ShaderInterface.hpp>

#include <NRI.h>
#include <array>
#include <lux/engine/render/extensions/graph/Work.hpp>

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
    [[nodiscard]] std::vector<nri::DescriptorRangeDesc> descriptorRanges(std::span<const ShaderBinding>) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*>
    compileCompute(
        nri::Device&,
        const nri::CoreInterface&,
        const EnabledDeviceFeatures&,
        nri::PipelineLayout&,
        const rdesc::SpirvArtifact&
    ) noexcept;
    // Validated stage borrows stay within the caller's immutable shader candidate.
    [[nodiscard]] RenderResult<std::array<const rdesc::SpirvStage*, 2>>
    rasterShaderStages(
        const rdesc::SpirvArtifact& shader,
        const GraphicsState& graphics,
        const nri::DeviceDesc& limits,
        const EnabledDeviceFeatures& enabled,
        std::size_t color_count,
        nri::Format depth
    ) noexcept;
} // namespace lux::render::detail
