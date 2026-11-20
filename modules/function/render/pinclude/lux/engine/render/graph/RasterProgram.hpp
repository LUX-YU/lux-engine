#pragma once

#include <NRI.h>
#include <array>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <span>
#include <utility>

namespace lux::render::detail
{
    struct EnabledDeviceFeatures;

    // Full fixed-function state, after native layout and shader identity have been resolved.
    struct RasterProgramKey final
    {
        GraphicsState state;
        std::vector<nri::Format> colors;
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint32_t samples = 1;
        [[nodiscard]] bool matches(const RasterProgramKey& other) const noexcept
        {
            const bool same_state = samples == other.samples && depth == other.depth &&
                colors == other.colors && state == other.state;
            return same_state;
        }
    };

    // Any returned native handle belongs to the caller, including on failure.
    // The native layout is borrowed from the same cold placement used to relocate the shader.
    [[nodiscard]] RenderResult<void> createRasterProgram(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const nri::DeviceDesc& limits,
        const EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::PipelineLayout& layout,
        const rdesc::ShaderProgramArtifact& shader,
        const RasterProgramKey& program,
        nri::Pipeline*& pipeline
    ) noexcept;
} // namespace lux::render::detail
