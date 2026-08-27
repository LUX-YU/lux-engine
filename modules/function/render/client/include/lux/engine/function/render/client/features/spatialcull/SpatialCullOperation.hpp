#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    /// Comm-layer config for SpatialCullFeature (spatial-cell-grid coarse cull).
    /// All fields have sane defaults so a `{}` config works.
    struct LUX_COMM_CONFIG(
        prefix = SpatialCull,
        id = lux.render.spatial_cull.v1,
        display = SpatialCull,
        custom_create = true) SpatialCullCommConfig
    {
        float cell_size{128.0f};     ///< Cell edge length (world units).
        float cull_distance{512.0f}; ///< Cull distance — a cell farther than this from the camera goes to sleep.
    };
    static_assert(std::is_trivially_copyable_v<SpatialCullCommConfig>);

    /// Spatial-cell-grid coarse-cull feature factory. Add it to a scene to opt
    /// into spatial coarse cull (the SpatialCullGrid publishes a per-instance cull
    /// mask); omit it and the scene pays nothing. The render core never references
    /// SpatialCullGrid directly — large-world streaming is just one client of it.

} // namespace lux::render
