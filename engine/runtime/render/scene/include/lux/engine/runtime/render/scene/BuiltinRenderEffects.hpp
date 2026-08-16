#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/runtime/extensions/RenderEffects.hpp>
#include <lux/engine/runtime/render/scene/visibility.h>

namespace lux::runtime
{
    /// Register the engine-owned Grid3D effect recipe. The generic
    /// contribution host deliberately has no dependency on built-in render
    /// feature implementations.
    [[nodiscard]] LUX_RUNTIME_RENDER_SCENE_PUBLIC lux::cxx::expected<
        void,
        ERenderEffectCatalogError>
    addGrid3DRenderEffect(RenderEffectCatalog& catalog);
}
