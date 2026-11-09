#pragma once

#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/MeshShadows.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/capabilities/ColorEncoding.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <lux/engine/render/extensions/CapabilityRegistry.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <lux/engine/render/extensions/FrameDataMethod.hpp>
#include <lux/engine/render/capabilities/CameraInputHandler.hpp>
#include <lux/engine/render/capabilities/UiInputHandler.hpp>
#include <lux/engine/render/storage/DirectLightingCommandHandler.hpp>
#include <lux/engine/render/storage/DirectLightingStorage.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshCommandHandler.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/RasterMaterialCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMaterialStorage.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceCommandHandler.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/storage/SampledTexture2DCommandHandler.hpp>
#include <lux/engine/render/storage/SampledTexture2DStorage.hpp>

namespace lux::render::detail
{
    template <std::uint32_t Dependency> using LightBindings = std::tuple<
        CommandMethod<Lighting3dCommands::CreateLights, &DirectLightingCommandHandler::createLights, Dependency>,
        CommandMethod<Lighting3dCommands::ReleaseLight, &DirectLightingCommandHandler::removeLight, Dependency>>;

    using MeshBindings = std::tuple<
        CommandMethod<Mesh3dCommands::UploadMesh, &IndexedTriangleMeshCommandHandler::createMesh, 0>,
        CommandMethod<Mesh3dCommands::ReleaseMesh, &IndexedTriangleMeshCommandHandler::releaseMesh, 0>>;

    using MaterialBindings = std::tuple<
        CommandMethod<MaterialCommands::Create, &RasterMaterialCommandHandler::createMaterial, 2>,
        CommandMethod<MaterialCommands::Update, &RasterMaterialCommandHandler::updateMaterial, 2>,
        CommandMethod<MaterialCommands::Release, &RasterMaterialCommandHandler::releaseMaterial, 2>>;

    using InstanceBindings = std::tuple<
        CommandMethod<Mesh3dCommands::CreateInstance, &RasterMeshInstanceCommandHandler::createInstance, 3>,
        CommandMethod<Mesh3dCommands::CreateInstances, &RasterMeshInstanceCommandHandler::createInstances, 3>,
        CommandMethod<Mesh3dCommands::ReleaseInstance, &RasterMeshInstanceCommandHandler::releaseInstance, 3>>;

    template <std::uint32_t Dependency> using ImageBindings = std::tuple<
        CommandMethod<ImageCommands::Upload, &SampledTexture2DCommandHandler::createTexture, Dependency>,
        CommandMethod<ImageCommands::CloneRegion, &SampledTexture2DCommandHandler::createPatchedTexture, Dependency>,
        CommandMethod<ImageCommands::Release, &SampledTexture2DCommandHandler::releaseTexture, Dependency>>;

    using RasterStorages = std::tuple<
        SampledTexture2DStorage, IndexedTriangleMeshStorage, RasterMaterialStorage, RasterMeshInstanceStorage>;
    using RasterBindings = decltype(std::tuple_cat(
        std::declval<ImageBindings<1>>(), std::declval<MeshBindings>(),
        std::declval<MaterialBindings>(), std::declval<InstanceBindings>()
    ));

    // Composition declarations, never graph Feature entries or live instances.
    struct RasterSurface final
    {
        using Capability = Mesh3dCapability;
        using FrameBindings = std::tuple<
            FrameDataMethod<TransformData, &RasterMeshInstanceCommandHandler::updateTransforms, 3>,
            FrameDataMethod<MeshInstanceUpdates, &RasterMeshInstanceCommandHandler::updateInstances, 3>,
            FrameDataMethod<CameraInput, &CameraInputHandler::apply>>;
        using Features = std::tuple<GBufferFeature, MeshVisibilityFeature>;
        using Config = RasterSurfaceConfiguration;
        static constexpr FeatureName name{"lux.render.raster_surface"};
        using Storages = RasterStorages;
        using Bindings = RasterBindings;
        static CapabilityImplementation implementation() noexcept;
    };

    struct DeferredLight final
    {
        using Capability = Lighting3dCapability;
        using FrameBindings = std::tuple<
            FrameDataMethod<LightUpdates, &DirectLightingCommandHandler::updateLights, 0>,
            FrameDataMethod<CameraInput, &CameraInputHandler::apply>>;
        using Features = std::tuple<
            DeferredLightingFeature, TonemapFeature, ClusteredLightingFeature,
            ShadowMapFeature, MeshShadowFeature, MeshVisibilityFeature>;
        using Config = DeferredLightingConfiguration;
        static constexpr FeatureName name{"lux.render.deferred_lighting"};
        using Storages = std::tuple<DirectLightingStorage>;
        using Bindings = LightBindings<0>;
        static CapabilityImplementation implementation() noexcept;
    };

    struct UiScene final
    {
        using Capability = ImguiCapability;
        using FrameBindings = std::tuple<FrameDataMethod<ImguiFrameData, &UiInputHandler::draw, 0, "ui", "draw">>;
        using Features = std::tuple<UiRasterFeature>;
        using Config = UiConfiguration;
        static constexpr FeatureName name{"lux.render.scene.ui"};
        using Storages = std::tuple<SampledTexture2DStorage>;
        using Bindings = ImageBindings<0>;
        static CapabilityImplementation implementation() noexcept;
    };

    using BuiltinImplementations =
        std::tuple<RasterSurface, DeferredLight, UiScene>;
} // namespace lux::render::detail
