#pragma once

#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/lighting/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/mesh/HzbFeature.hpp>
#include <lux/engine/render/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>
#include <lux/engine/render/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/features/postprocess/TonemapFeature.hpp>

namespace lux::render::detail
{
    // Built-in dependency declarations only. These types are never instantiated.
    struct MaterialMeshRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MaterialMeshRaster"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct MeshDepthRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshDepthRaster"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct HzbFeature final
    {
        using Config = HzbFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.Hzb"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct GBufferFeature final
    {
        using Config = GBufferFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.GBufferFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct DeferredLightingFeature final
    {
        using Config = DeferredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.DeferredLightingFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct ShadowMapFeature final
    {
        using Config = ShadowMapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ShadowMapFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct ClusteredLightingFeature final
    {
        using Config = ClusteredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ClusteredLightingFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct MeshVisibilityFeature final
    {
        using Config = MeshVisibilityFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshVisibilityFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct MeshShadowFeature final
    {
        using Config = MeshShadowFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshShadowFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
    };

    struct TonemapFeature final
    {
        using Config = TonemapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.TonemapFeature"};
        struct Ports final
        {
            static RenderResult<void> describe(const Config*, std::string_view, FeaturePorts&) noexcept;
        };

        static RenderResult<void> build(const Config*, std::string_view, FeatureBuildContext&) noexcept;
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
        TonemapFeature
    >;
} // namespace lux::render::detail
