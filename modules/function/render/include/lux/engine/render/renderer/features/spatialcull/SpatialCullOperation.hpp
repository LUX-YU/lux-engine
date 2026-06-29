#pragma once
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    struct FeatureFactory;

    /// Comm-layer config for SpatialCullFeature (spatial-cell-grid coarse cull).
    /// All fields have sane defaults so a `{}` config works.
    struct SpatialCullCommConfig
    {
        float cell_size{128.0f};      ///< cell 边长(世界单位)
        float cull_distance{512.0f};  ///< 剔除距离(cell 超此距相机 → 休眠)
    };
    static_assert(std::is_trivially_copyable_v<SpatialCullCommConfig>);

    /// Spatial-cell-grid coarse-cull feature factory. Add it to a scene to opt
    /// into spatial coarse cull (the SpatialCullGrid publishes a per-instance cull
    /// mask); omit it and the scene pays nothing. The render core never references
    /// SpatialCullGrid directly — large-world streaming is just one client of it.
    extern LUX_FUNCTION_PUBLIC const FeatureFactory kSpatialCullFeatureFactory;

} // namespace lux::render
