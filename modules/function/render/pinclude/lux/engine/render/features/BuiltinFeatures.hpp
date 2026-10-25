#pragma once

#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
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
#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>
#include <lux/engine/render/storage/DirectLightingStorage.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/RasterMaterialTableCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    // Built-in dependency declarations only. These types are never instantiated.
    struct MaterialMeshRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MaterialMeshRaster"};
        using Storages = std::tuple<
            SampledTexture2DStorage,
            IndexedTriangleMeshStorage,
            RasterMaterialTableStorage,
            RasterMeshInstanceStorage>;
        using Commands = decltype(std::tuple_cat(
            std::declval<SampledTexture2DCommandHandler::Bindings>(),
            std::declval<IndexedTriangleMeshCommandHandler::Bindings>(),
            std::declval<RasterMaterialTableCommandHandler::Bindings>(),
            std::declval<RasterMeshInstanceCommandHandler::Bindings>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshDepthRaster final
    {
        using Config = MeshFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshDepthRaster"};
        using Storages = std::tuple<IndexedTriangleMeshStorage, RasterMaterialTableStorage, RasterMeshInstanceStorage>;
        using Commands = decltype(std::tuple_cat(
            std::declval<IndexedTriangleMeshCommandHandler::Bindings>(),
            std::declval<RasterMaterialTableCommandHandler::Bindings>(),
            std::declval<RasterMeshInstanceCommandHandler::Bindings>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct HzbFeature final
    {
        using Config = HzbFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.Hzb"};
        using Storages = std::tuple<>;
        using Commands = std::tuple<>;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct GBufferFeature final
    {
        using Config = GBufferFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.GBufferFeature"};
        using Storages = std::tuple<
            SampledTexture2DStorage,
            IndexedTriangleMeshStorage,
            RasterMaterialTableStorage,
            RasterMeshInstanceStorage>;
        using Commands = decltype(std::tuple_cat(
            std::declval<SampledTexture2DCommandHandler::Bindings>(),
            std::declval<IndexedTriangleMeshCommandHandler::Bindings>(),
            std::declval<RasterMaterialTableCommandHandler::Bindings>(),
            std::declval<RasterMeshInstanceCommandHandler::Bindings>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct DeferredLightingFeature final
    {
        using Config = DeferredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.DeferredLightingFeature"};
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ShadowMapFeature final
    {
        using Config = ShadowMapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ShadowMapFeature"};
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ClusteredLightingFeature final
    {
        using Config = ClusteredLightingFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.ClusteredLightingFeature"};
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshVisibilityFeature final
    {
        using Config = MeshVisibilityFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshVisibilityFeature"};
        using Storages = std::tuple<IndexedTriangleMeshStorage, RasterMaterialTableStorage, RasterMeshInstanceStorage>;
        using Commands = decltype(std::tuple_cat(
            std::declval<IndexedTriangleMeshCommandHandler::Bindings>(),
            std::declval<RasterMaterialTableCommandHandler::Bindings>(),
            std::declval<RasterMeshInstanceCommandHandler::Bindings>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshShadowFeature final
    {
        using Config = MeshShadowFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.MeshShadowFeature"};
        using Storages = std::tuple<
            SampledTexture2DStorage,
            IndexedTriangleMeshStorage,
            RasterMaterialTableStorage,
            RasterMeshInstanceStorage>;
        using Commands = decltype(std::tuple_cat(
            std::declval<SampledTexture2DCommandHandler::Bindings>(),
            std::declval<IndexedTriangleMeshCommandHandler::Bindings>(),
            std::declval<RasterMaterialTableCommandHandler::Bindings>(),
            std::declval<RasterMeshInstanceCommandHandler::Bindings>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct TonemapFeature final
    {
        using Config = TonemapFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.TonemapFeature"};
        using Storages = std::tuple<>;
        using Commands = std::tuple<>;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct UiRasterFeature final
    {
        using Config = UiRasterFeatureConfig;
        static constexpr FeatureName name{"lux.render.feature.UiRasterFeature"};
        using Storages = std::tuple<SampledTexture2DStorage>;
        using Commands = SampledTexture2DCommandHandler::Bindings;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    // An installable asset capability has no rendering pass or per-install object.
    // Its dependencies are the same factories/commands used by the raster compositions.
    struct RasterAssets final
    {
        using Config = void;
        static constexpr FeatureName name{"lux.render.assets.raster"};
        using Storages = MaterialMeshRaster::Storages;
        using Commands = MaterialMeshRaster::Commands;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
        static const CapabilitySupport& capabilities() noexcept;
    };

    struct ClusteredScene final
    {
        using Config = LitSceneConfiguration;
        static constexpr FeatureName name{"lux.render.scene.clustered"};
        using Storages = decltype(std::tuple_cat(
            std::declval<MaterialMeshRaster::Storages>(),
            std::declval<DeferredLightingFeature::Storages>()
        ));
        using Commands = decltype(std::tuple_cat(
            std::declval<MaterialMeshRaster::Commands>(),
            std::declval<DeferredLightingFeature::Commands>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
        static const CapabilitySupport& capabilities() noexcept;
    };

    struct DeferredScene final
    {
        using Config = LitSceneConfiguration;
        static constexpr FeatureName name{"lux.render.scene.deferred"};
        using Storages = decltype(std::tuple_cat(
            std::declval<MaterialMeshRaster::Storages>(),
            std::declval<DeferredLightingFeature::Storages>()
        ));
        using Commands = decltype(std::tuple_cat(
            std::declval<MaterialMeshRaster::Commands>(),
            std::declval<DeferredLightingFeature::Commands>()
        ));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
        static const CapabilitySupport& capabilities() noexcept;
    };

    struct UiScene final
    {
        using Config = UiConfiguration;
        static constexpr FeatureName name{"lux.render.scene.ui"};
        using Storages = UiRasterFeature::Storages;
        using Commands = UiRasterFeature::Commands;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
        static const CapabilitySupport& capabilities() noexcept;
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
        UiRasterFeature,
        RasterAssets,
        ClusteredScene,
        DeferredScene,
        UiScene>;
} // namespace lux::render::detail
