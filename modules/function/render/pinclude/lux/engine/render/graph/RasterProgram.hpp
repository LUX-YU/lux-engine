#pragma once

#include <NRI.h>
#include <array>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <span>

namespace lux::render::detail
{
    struct EnabledDeviceFeatures;

    // Exact pipeline identity after layout relocation. Shader includes entry points and reflected ABI.
    // Device/table capacities are fixed by the Device; resource IDs/addresses are not PSO identity.
    struct RasterProgramKey final
    {
        rdesc::ShaderProgramArtifact shader;
        GraphicsState state;
        std::vector<nri::Format> colors;
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint32_t samples = 1;
        bool operator==(const RasterProgramKey&) const noexcept = default;
    };

    // Creates one physical pipeline from an immutable candidate; the caller adopts its ownership.
    // The native layout is borrowed from the same cold placement used to relocate the shader.
    [[nodiscard]] RenderResult<nri::Pipeline*> compileRasterProgram(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout& layout,
        const RasterProgramKey& program
    ) noexcept;
} // namespace lux::render::detail
