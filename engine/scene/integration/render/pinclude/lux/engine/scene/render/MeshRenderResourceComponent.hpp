#pragma once
#include <lux/engine/context/render/RenderAssetPipeline.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::scene::detail
{
    // Adopted runtime data only. Signals transfer exits; destruction performs no asynchronous work.
    struct MeshRenderResourceComponent final
    {
        engine::render::RenderAssetVersion source;
        render::MeshId mesh;
        rdesc::MeshVisualDescription visual;
    };
} // namespace lux::scene::detail
