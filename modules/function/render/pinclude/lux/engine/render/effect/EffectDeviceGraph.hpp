#pragma once

#include <lux/engine/render/effect/EffectResourceManifest.hpp>
#include <lux/engine/render/effect/LinkedFrameAlgorithms.hpp>
#include <lux/engine/render/resource/ConstantBufferStorage.hpp>

namespace lux::render::detail
{
    // One Effect owns these final tables per device. Device installation attaches the UBO storage.
    struct EffectDeviceGraph final
    {
        std::unique_ptr<const compiled::RenderGraph> graph;
        FrameAlgorithms algorithms;
        runtime_detail::EffectResourceManifest resources;
        std::vector<BoundInlineBlock> inline_blocks;
        std::vector<BoundConstantBuffer> constant_buffers;
    };
}
