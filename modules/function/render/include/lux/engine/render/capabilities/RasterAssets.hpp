#pragma once

#include <string_view>

namespace lux::render
{
    // Stable input schemas. Select an implementation through ListCapabilities, install it once
    // in the existing business scope, and use the returned input destinations for uploads.
    inline constexpr std::string_view kRasterAssetsCapability = "lux.render.capability.raster_assets";
    inline constexpr std::string_view kIndexedMeshInput = "lux.render.input.indexed_triangle_mesh";
    inline constexpr std::string_view kSampledTextureInput = "lux.render.input.sampled_texture_2d";
    inline constexpr std::string_view kRasterMaterialInput = "lux.render.input.raster_material";
    inline constexpr std::string_view kRasterInstancesInput = "lux.render.input.raster_instances";
    inline constexpr std::string_view kDirectLightsInput = "lux.render.input.direct_lights";
} // namespace lux::render
