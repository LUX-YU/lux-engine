#pragma once

#include <lux/engine/scene/render/visibility.h>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <cstdint>
#include <string>

namespace lux::scene
{
    // Persistent recipe selection. Feature IDs are discovered per Render connection, never serialized.
    struct RenderSystemConfiguration final
    {
        RenderSystemConfiguration() noexcept = default;
        std::string recipe = "lux.scene.clustered";
        std::uint32_t recipe_version = 1;
        bool shadows = true;
        std::uint32_t shadow_resolution = 1024;
        std::uint32_t shadow_filter_radius = 1;
        float shadow_distance = 100;
        std::uint32_t cluster_index_capacity = 65536;
    };

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC bool
    validRenderConfiguration(const RenderSystemConfiguration&) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_RENDER_PUBLIC serialization::PortableValueCodec renderConfigurationCodec() noexcept;
} // namespace lux::scene
