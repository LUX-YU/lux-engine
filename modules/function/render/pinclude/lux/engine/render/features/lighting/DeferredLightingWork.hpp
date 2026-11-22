#pragma once

#include <lux/engine/render/extensions/graph/Work.hpp>

namespace lux::render
{
    class EffectCompileContext;
}

namespace lux::render::detail
{
    struct DeferredLightingInputs final
    {
        std::array<ResourceVersion, 4> surface, lights;
        std::optional<std::array<ResourceVersion, 4>> shadows, clusters;
    };

    [[nodiscard]] RenderResult<RasterWork>
    deferredLightingWork(EffectCompileContext&, const DeferredLightingInputs&) noexcept;
}
