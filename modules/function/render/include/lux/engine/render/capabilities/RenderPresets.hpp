#pragma once

#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/commands/EffectCommands.hpp>

namespace lux::render
{
    // Typed author functions consume selected catalog identities and produce the sole CreateEffect recipe.
    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<CreateEffectRequest>
    makeDeferredRenderEffect(
        EffectOutputDesc,
        const CapabilityImplementationInfo& surface,
        const CapabilityImplementationInfo& lighting,
        const RasterSurfaceConfiguration& = {},
        const DeferredLightingConfiguration& = {}
    ) noexcept;

    // Persistent preset settings. Resolve the selected capabilities and pass the decoded settings to
    // makeDeferredRenderEffect to build the graph recipe.
    [[nodiscard]] LUX_RENDER_PUBLIC CapabilityCompositionDescription
    deferredRenderComposition(bool clustered = true, bool shadows = true) noexcept;
} // namespace lux::render
