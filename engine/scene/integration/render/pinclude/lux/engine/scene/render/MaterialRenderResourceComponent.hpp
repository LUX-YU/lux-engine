#pragma once
#include <lux/engine/context/render/RenderAssetPipeline.hpp>
#include <lux/engine/description/Visual.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>

namespace lux::scene::detail
{
    // Adopted runtime data only. Signals transfer exits; destruction performs no asynchronous work.
    struct MaterialRenderResourceComponent final
    {
        engine::render::RenderAssetVersion source;
        render::MaterialId material;
    };
} // namespace lux::scene::detail
