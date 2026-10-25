#pragma once

#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/serialization/PortableValueCodec.hpp>

namespace lux::scene
{
    // Persistent demand and explicit implementation choice. Session-local selection IDs are resolved on open.
    // Algorithm-specific configuration belongs to the selected Render implementation's versioned codec.
    struct RenderSystemConfiguration final
    {
        RenderSystemConfiguration() noexcept = default;
        std::string capability;
        std::uint32_t capability_version = 1;
        std::string implementation;
        std::uint32_t implementation_version = 1;
        render::SerializedRenderConfiguration configuration;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC bool
    validRenderConfiguration(const RenderSystemConfiguration&) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC serialization::PortableValueCodec renderConfigurationCodec() noexcept;
} // namespace lux::scene
