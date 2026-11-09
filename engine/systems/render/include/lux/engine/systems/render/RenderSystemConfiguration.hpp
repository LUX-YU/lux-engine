#pragma once

#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine::render
{
    // Persistent demand and explicit implementation choice. Session-local selection IDs are resolved on open.
    // Algorithm-specific configuration belongs to the selected Render implementation's versioned codec.
    struct RenderSystemConfiguration final
    {
        RenderSystemConfiguration() noexcept = default;
        lux::render::CapabilityCompositionDescription composition;
        // Static output contract; actual pixel dimensions belong to each attached Target.
        lux::render::EffectOutputDesc output{rdesc::ETextureFormat::RGBA8_UNORM};
        bool operator==(const RenderSystemConfiguration&) const noexcept = default;
    };

    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC bool
    validRenderConfiguration(const RenderSystemConfiguration&) noexcept;

    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC serialization::PortableValueCodec renderConfigurationCodec() noexcept;
} // namespace lux::engine::render
