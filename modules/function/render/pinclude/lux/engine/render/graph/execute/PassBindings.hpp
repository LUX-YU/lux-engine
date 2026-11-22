#pragma once

#include <lux/engine/render/graph/execute/BindingState.hpp>

namespace lux::render::detail
{
    inline void
    setPassScissor(
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        const nri::Rect& value,
        BindingState& state
    ) noexcept
    {
        const bool changed = !state.scissor || state.scissor->x != value.x || state.scissor->y != value.y ||
                             state.scissor->width != value.width || state.scissor->height != value.height;
        if (changed)
        {
            core.CmdSetScissors(command, &value, 1);
            state.scissor = value;
        }
    }

    inline void
    setPassDepthBias(
        const nri::CoreInterface& core,
        nri::CommandBuffer& command,
        const nri::DepthBiasDesc& value,
        BindingState& state
    ) noexcept
    {
        const bool changed = !state.depth_bias || state.depth_bias->constant != value.constant ||
                             state.depth_bias->clamp != value.clamp || state.depth_bias->slope != value.slope;
        if (changed)
        {
            core.CmdSetDepthBias(command, value);
            state.depth_bias = value;
        }
    }
}
