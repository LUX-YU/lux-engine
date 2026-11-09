#pragma once

#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/HzbFeature.hpp>
#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/features/ShadowMapFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/render/features/UiRasterFeature.hpp>

namespace lux::render::detail
{
    // Built-in dependency declarations only. These types are never instantiated.
    struct MaterialMeshRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MaterialMeshRaster"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshDepthRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshDepthRaster"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct HzbFeature final
    {
        using Config = HzbFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.Hzb"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct GBufferFeature final
    {
        using Config = GBufferFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.GBufferFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct DeferredLightingFeature final
    {
        using Config = DeferredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.DeferredLightingFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ShadowMapFeature final
    {
        using Config = ShadowMapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ShadowMapFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ClusteredLightingFeature final
    {
        using Config = ClusteredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ClusteredLightingFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshVisibilityFeature final
    {
        using Config = MeshVisibilityFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshVisibilityFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshShadowFeature final
    {
        using Config = MeshShadowFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshShadowFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct TonemapFeature final
    {
        using Config = TonemapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.TonemapFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct UiRasterFeature final
    {
        using Config = UiRasterFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.UiRasterFeature"};
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    using BuiltinFeatures = std::tuple<
        MaterialMeshRaster,
        MeshDepthRaster,
        HzbFeature,
        GBufferFeature,
        DeferredLightingFeature,
        ShadowMapFeature,
        ClusteredLightingFeature,
        MeshVisibilityFeature,
        MeshShadowFeature,
        TonemapFeature,
        UiRasterFeature>;
} // namespace lux::render::detail
