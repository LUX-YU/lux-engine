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
    class DeviceSession;
    struct RasterProgramKey;
    [[nodiscard]] nri::StageBits shaderStages(std::uint32_t stages) noexcept;
    [[nodiscard]] std::vector<nri::DescriptorRangeDesc>
    descriptorRanges(std::span<const ShaderBinding>, bool preserve_registers = false) noexcept;
    [[nodiscard]] RenderResult<nri::Pipeline*>
    compileCompute(const DeviceSession&, nri::PipelineLayout&, const rdesc::ShaderProgramArtifact&) noexcept;
    // Validated stage borrows stay within the caller's immutable shader candidate.
    [[nodiscard]] RenderResult<std::array<const rdesc::ShaderStageArtifact*, 2>>
    rasterShaderStages(const DeviceSession&, const rdesc::ShaderProgramArtifact&, const RasterProgramKey&) noexcept;
} // namespace lux::render::detail
