#pragma once

#include <NRI.h>
#include <array>
#include <lux/engine/description/ShaderProgramArtifact.hpp>
#include <lux/engine/render/extensions/graph/Draw.hpp>
#include <span>
#include <utility>

namespace lux::render::detail
{
    class DeviceSession;

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

    [[nodiscard]] RenderResult<nri::Pipeline*>
    createRasterProgram(
        const DeviceSession&, nri::PipelineLayout&, const rdesc::ShaderProgramArtifact&, const RasterProgramKey&
    ) noexcept;
} // namespace lux::render::detail
