#pragma once

#include <lux/engine/render/capabilities/CapabilityComposition.hpp>

namespace lux::render
{
    // Ordinary authoring data for the supplied professional capabilities. Applications
    // may edit/replace every contribution and connection before resolving the names.
    [[nodiscard]] LUX_RENDER_PUBLIC CapabilityCompositionDescription
    deferredRenderComposition(bool clustered = true, bool shadows = true) noexcept;
} // namespace lux::render
