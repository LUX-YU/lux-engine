#pragma once

#include <lux/engine/render/graph/Work.hpp>

namespace lux::render
{
    class EffectCompileContext;
}

namespace lux::render::detail
{
    struct DeferredLightingInputs final
    {
        std::array<ResourceVersion, 4> surface, lights;
        ResourceVersion view;
        std::optional<std::array<ResourceVersion, 4>> shadows, clusters;
    };

    [[nodiscard]] RenderResult<RasterWork>
    deferredLightingWork(EffectCompileContext&, const DeferredLightingInputs&) noexcept;
}
