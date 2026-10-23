#pragma once

#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/HzbFeature.hpp>
#include <lux/engine/render/features/MaterialMeshRaster.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>
#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
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
        using Descriptor = MaterialMeshRasterDescriptor;
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
        using Descriptor = MeshDepthRasterDescriptor;
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
        using Descriptor = HzbFeatureDescriptor;
        using Storages = std::tuple<>;
        using Commands = std::tuple<>;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct GBufferFeature final
    {
        using Descriptor = GBufferFeatureDescriptor;
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
        using Descriptor = DeferredLightingFeatureDescriptor;
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ShadowMapFeature final
    {
        using Descriptor = ShadowMapFeatureDescriptor;
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct ClusteredLightingFeature final
    {
        using Descriptor = ClusteredLightingFeatureDescriptor;
        using Storages = std::tuple<DirectLightingStorage>;
        using Commands = decltype(std::tuple_cat(std::declval<DirectLightingCommandHandler::Bindings>()));
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct MeshVisibilityFeature final
    {
        using Descriptor = MeshVisibilityFeatureDescriptor;
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
        using Descriptor = MeshShadowFeatureDescriptor;
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
        using Descriptor = TonemapFeatureDescriptor;
        using Storages = std::tuple<>;
        using Commands = std::tuple<>;
        static const FeatureGraphOps& graph(bool paged_instances) noexcept;
    };

    struct UiRasterFeature final
    {
        using Descriptor = UiRasterFeatureDescriptor;
        using Storages = std::tuple<SampledTexture2DStorage>;
        using Commands = SampledTexture2DCommandHandler::Bindings;
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
