#pragma once

#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/extensions/graph/PassRecordInput.hpp>

namespace lux::render::detail
{
    inline void bindPassDescriptors(
        const nri::CoreInterface& core,
        std::span<nri::DescriptorSet* const, 3> shared_descriptors,
        nri::CommandBuffer& command,
        const PassDescriptorBindings& bindings,
        std::span<nri::DescriptorSet* const> private_sets,
        BindingState& state
    ) noexcept
    {
        const auto bind = [&](std::uint32_t set, nri::DescriptorSet* descriptors) noexcept
        {
            if (state.descriptors[set] != descriptors)
            {
                core.CmdSetDescriptorSet(command, {set, descriptors, state.point});
                state.descriptors[set] = descriptors;
            }
        };
        for (std::size_t kind = 0; kind < bindings.shared_sets.size(); ++kind)
        {
            const auto set = bindings.shared_sets[kind];
            if (set != UINT32_MAX) bind(set, shared_descriptors[kind]);
        }
        for (std::uint32_t i = 0; i < private_sets.size(); ++i) bind(bindings.private_set + i, private_sets[i]);
    }
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
