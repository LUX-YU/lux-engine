#include "DeviceRenderFixture.hpp"
#include "support/QualificationRenderRuntime.hpp"

#include <lux/engine/function/render/client/core/RenderFeatureMetaModule.hpp>
#include <lux/engine/function/render/client/genops/ForwardMeshOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/scene/Builtin3DRenderIntegration.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneRenderSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/VisualSchema.hpp>
#include <lux/engine/task/TaskExecutor.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace
{
    inline constexpr int Skip = 77;

    template <class Asset>
    [[nodiscard]] std::shared_ptr<const Asset> decode(lux::asset::AssetVfsView vfs, lux::asset::AssetId id)
    {
        const auto blob = vfs.open(id);
        if (!blob)
        {
            return {};
        }
        const auto result = lux::asset::TAssetSerDeser<Asset>::decode(
            id,
            blob->bytes,
            lux::asset::AssetDecodeLimits{blob->bytes.size(), 128U * 1024U * 1024U, 16U}
        );
        return result ? *result : std::shared_ptr<const Asset>{};
    }

    template <class Type>
    [[nodiscard]] Type worldId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }

    [[nodiscard]] lux::asset::AssetId assetId(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return lux::asset::AssetId(bytes);
    }

    [[nodiscard]] std::shared_ptr<const lux::world::WorldDescription> makeWorld()
    {
        lux::world::WorldDescriptionBuilder builder;
        if (!builder.setIdentity(
                worldId<lux::world::WorldBundleId>(1U),
                worldId<lux::world::WorldBundleGeneration>(2U),
                "l1-l3-render-scene") ||
            !builder.setPartitioner({lux::world::worldPartitionerId("test.none"), 1U}, 0U))
        {
            return {};
        }
        auto result = std::move(builder).build();
        return result ? std::make_shared<lux::world::WorldDescription>(std::move(*result)) : nullptr;
    }

    [[nodiscard]] std::shared_ptr<const lux::simulation::SimulationDescription> makeSimulation()
    {
        lux::simulation::SimulationDescriptionBuilder builder;
        auto result = std::move(builder).build();
        return result ? std::make_shared<lux::simulation::SimulationDescription>(std::move(*result)) : nullptr;
    }

    [[nodiscard]] std::shared_ptr<const lux::scene::SceneDescription> makeSceneDescription(
        const lux::scene::SceneMetaManager& meta,
        std::span<const lux::render::FeatureTypeId> feature_types
    )
    {
        using namespace lux;
        const auto registration = scene::builtinRenderSystemRegistration();
        scene::RenderSystemConfiguration configuration;
        for (const auto type : feature_types)
        {
            const auto* feature = meta.getRenderFeatureMeta(type);
            if (feature == nullptr || !feature->scene_configurable)
            {
                return {};
            }
            configuration.features.push_back({
                type,
                std::vector<std::byte>(feature->default_configuration.begin(), feature->default_configuration.end())
            });
        }
        std::vector<std::byte> encoded;
        if (!registration.configuration.encode(&configuration, encoded))
        {
            return {};
        }
        scene::SceneDescriptionBuilder builder;
        builder.setWorld(assetId(1U));
        builder.setSimulation(assetId(2U));
        constexpr system::SystemInstanceId RenderInstance{1U};
        if (!builder.addSystem(
                RenderInstance,
                "render",
                registration.type,
                registration.description->version,
                registration.description->configuration_schema_name,
                registration.description->configuration_schema_version,
                encoded) ||
            !builder.bindRequirement(RenderInstance, "render_runtime", "host.render"))
        {
            return {};
        }
        auto result = std::move(builder).build();
        return result ? std::make_shared<scene::SceneDescription>(std::move(*result)) : nullptr;
    }

    struct ReflectionScope final
    {
        ~ReflectionScope()
        {
            lux::meta::ReflectionRegistry::destroyRegistry();
        }
    };
}

int main()
{
    using namespace lux;
    using namespace lux::asset;
    using namespace lux::render;

    const auto pak = inspectPak(LUX_MODEL_QUALIFICATION_PAK);
    auto provider = PakAssetProvider::loadFromFile(LUX_MODEL_QUALIFICATION_PAK);
    if (!pak || !provider)
    {
        return 2;
    }
    auto vfs = std::make_shared<AssetVfs>();
    if (vfs->mount({"/Game", *provider, 0}) == kInvalidMountId)
    {
        return 3;
    }
    AssetId model_id;
    for (const auto& entry : pak->entries)
    {
        if (entry.magic_number == ModelAsset::primary_magic)
        {
            model_id = entry.id;
        }
    }
    const auto asset_view = vfs->view();
    const auto model = decode<ModelAsset>(asset_view, model_id);
    if (!model || model->data().primitives.empty())
    {
        return 4;
    }
    const auto& primitive = model->data().primitives.front();
    const auto mesh_asset = decode<MeshAsset>(asset_view, primitive.mesh);
    const auto material_asset = decode<MaterialAsset>(asset_view, primitive.material);
    if (!mesh_asset || !material_asset)
    {
        return 5;
    }

    std::atomic_int validation_errors{};
    rendertest::DeviceRenderFixture fixture(
        256U,
        256U,
        "l1_l3_render_sync_3d_qualification",
        {.enable_validation = true, .validation_errors = &validation_errors}
    );
    if (!fixture.ok())
    {
        std::puts("SKIP: Vulkan device or validation layer unavailable");
        return Skip;
    }

    render::initializeBuiltinRenderFeatureMeta();
    scene::initializeBuiltinRenderSystemMeta();
    meta::ReflectionRegistry::initRegistry();
    ReflectionScope reflection_scope;
    std::vector<simulation::ecs::ComponentSchema> schemas;
    const auto transform_schemas = simulation::ecs::transformComponentSchemas();
    const auto visual_schemas = simulation::ecs::visualComponentSchemas();
    schemas.insert(schemas.end(), transform_schemas.begin(), transform_schemas.end());
    schemas.insert(schemas.end(), visual_schemas.begin(), visual_schemas.end());
    const auto render_schemas = scene::sceneRenderComponentSchemas();
    schemas.insert(schemas.end(), render_schemas.begin(), render_schemas.end());
    auto component_set = simulation::ecs::ComponentSchemaSet::build(std::move(schemas));
    if (!component_set)
    {
        return 6;
    }
    const auto feature_registrations = render::builtinRenderFeatureRegistrations();
    const auto feature_bindings = scene::builtinRenderFeatureSceneBindings();
    auto meta_manager = scene::SceneMetaManager::build({
        std::move(*component_set),
        simulation::SimulationSystemRegistry{},
        {scene::builtinRenderSystemRegistration()},
        {feature_registrations.begin(), feature_registrations.end()},
        {feature_bindings.begin(), feature_bindings.end()}
    });
    if (!meta_manager)
    {
        return 6;
    }

    const std::array<const FeatureFactory*, 8U> factories{
        &kViewCameraFeatureFactory,
        &kMaterialFeatureFactory,
        &kMeshStackFeatureFactory,
        &kRenderClusterFeatureFactory,
        &kLightFeatureFactory,
        &kShadowMapFeatureFactory,
        &kMeshShadowFeatureFactory,
        &kForwardMeshFeatureFactory
    };
    scene::qualification::QualificationRenderRuntime runtime({fixture, *meta_manager, factories});
    auto host_lease = runtime.acquire();
    if (!host_lease)
    {
        return 7;
    }
    const auto runtime_provider = scene::makeSceneCapabilityProvider<scene::RenderRuntime>(
        "host.render",
        "lux.render.runtime",
        runtime
    );
    const std::array selected_features{
        featureId("lux.render.view_camera.v1"),
        featureId("lux.render.material.v1"),
        featureId("lux.render.mesh_stack.v1"),
        featureId("lux.render.cluster.v1"),
        featureId("lux.render.light.v1"),
        featureId("lux.render.shadow_map.v1"),
        featureId("lux.render.mesh_shadow.v1"),
        featureId("lux.render.forward_mesh.v1")
    };
    auto scene_description = makeSceneDescription(*meta_manager, selected_features);
    auto world = makeWorld();
    auto simulation_description = makeSimulation();
    if (!scene_description || !world || !simulation_description)
    {
        return 8;
    }
    auto scene_instance = scene::Scene::create({
        scene_description,
        world,
        simulation_description,
        *meta_manager,
        std::span(&runtime_provider, 1U)
    });
    if (!scene_instance)
    {
        std::printf(
            "Scene install failed: build=%u system=%u subject=%llu\n",
            static_cast<unsigned>(scene_instance.error().code),
            static_cast<unsigned>(scene_instance.error().scene_system.code),
            static_cast<unsigned long long>(scene_instance.error().scene_system.subject_hash)
        );
        return 9;
    }
    auto& scene_value = **scene_instance;
    const auto* render_system = scene_value.findSceneSystem<scene::RenderSystem>();
    if (render_system == nullptr || !render_system->renderSceneId().isValid())
    {
        return 10;
    }
    const auto render_scene_id = render_system->renderSceneId();
    const auto view_reply = fixture.awaitControl(host_lease->control().addView(
        render_scene_id,
        {fixture.width(), fixture.height()},
        "L1L3View"
    ));
    const auto target_reply = fixture.awaitControl(host_lease->control().createOffscreenRenderTarget({
        fixture.width(), fixture.height()
    }));
    fixture.awaitControl(host_lease->control().setActiveScene(render_scene_id, true));
    host_lease->control().setLayer(target_reply.target, 0U, render_scene_id, view_reply.view);
    const rendertest::DeviceRenderFixture::SceneView render_scene{
        render_scene_id,
        view_reply.view,
        target_reply.target
    };
    fixture.flush();

    const auto& catalog = host_lease->features();
    const auto opsFor = [&catalog]<class Operations>(FeatureTypeId type) {
        return catalog.ops<Operations>(catalog.nameOfType(type));
    };
    const auto camera_ops = opsFor.template operator()<ViewCameraOperationIds>(
        featureId("lux.render.view_camera.v1")
    );
    const auto material_ops = opsFor.template operator()<MaterialOperationIds>(
        featureId("lux.render.material.v1")
    );
    const auto mesh_ops = opsFor.template operator()<MeshStackOperationIds>(
        featureId("lux.render.mesh_stack.v1")
    );
    if (!camera_ops.valid() || !material_ops.valid() || !mesh_ops.valid())
    {
        return 11;
    }

    const auto compile_shader = [&](const std::vector<std::uint32_t>& spirv, const rdesc::ShaderInfo& info) {
        const auto info_bytes = rdesc::ShaderInfo::serialize(info);
        return fixture.awaitControl(host_lease->control().compileShader(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(spirv.data()), spirv.size() * sizeof(std::uint32_t)
            },
            info_bytes
        ));
    };
    const auto gbuffer_shader = compile_shader(material_asset->data().gbuffer_spirv, material_asset->data().gbuffer_info);
    const auto forward_shader = compile_shader(material_asset->data().forward_spirv, material_asset->data().forward_info);
    if (gbuffer_shader.status != 0U || forward_shader.status != 0U)
    {
        return 12;
    }

    GraphMaterialData graph_material{};
    graph_material.param_count = material_asset->data().parameter_count;
    for (std::uint32_t parameter{}; parameter < graph_material.param_count; ++parameter)
    {
        std::copy_n(material_asset->data().parameter_defaults[parameter].begin(), 4U, graph_material.params[parameter]);
    }
    auto material_request = uploadGraphMaterial(
        MaterialUploadClient{host_lease->upload(), material_ops},
        graph_material,
        gbuffer_shader.shader,
        forward_shader.shader,
        static_cast<std::uint32_t>(material_asset->data().alpha_mode),
        material_asset->data().double_sided
    );
    auto mesh_request = uploadMesh(MeshStackUploadClient{host_lease->upload(), mesh_ops}, mesh_asset->data());
    if (!material_request || !mesh_request)
    {
        return 13;
    }
    const auto material = fixture.awaitUpload(std::move(*material_request));
    const auto mesh = fixture.awaitUpload(std::move(*mesh_request));
    if (material.status != 0U || material.handle.isNull() || mesh.status != 0U || mesh.handle.isNull())
    {
        return 14;
    }

    auto& registry = scene_value.registry();
    const auto mesh_entity = registry.create();
    registry.emplace<simulation::ecs::Mesh3D>(
        mesh_entity,
        simulation::ecs::Mesh3D{rdesc::MeshVisualDescription{mesh_asset->id(), material_asset->id()}}
    );
    registry.emplace<scene::ResolvedMeshResources>(
        mesh_entity,
        scene::ResolvedMeshResources{mesh_asset->id(), material_asset->id(), mesh.handle, material.handle}
    );
    simulation::ecs::WorldTransform3D mesh_transform{};
    mesh_transform.value.translation().z() = -2.0;
    registry.emplace<simulation::ecs::WorldTransform3D>(mesh_entity, mesh_transform);

    const auto light_entity = registry.create();
    simulation::ecs::Light3D light{};
    light.value.type = rdesc::ELightType::DIRECTIONAL;
    light.value.intensity = 3.0F;
    registry.emplace<simulation::ecs::Light3D>(light_entity, light);
    simulation::ecs::WorldTransform3D light_transform{};
    light_transform.value.linear() = Eigen::AngleAxisd{-0.55, Eigen::Vector3d::UnitX()}.toRotationMatrix();
    registry.emplace<simulation::ecs::WorldTransform3D>(light_entity, light_transform);

    auto executor = task::TaskExecutor::create(task::TaskExecutorConfig{0U, 64U});
    if (!executor || !scene_value.simulation().execute(*executor) || !scene_value.executeStablePoint())
    {
        return 15;
    }
    if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies() ||
        !scene_value.executePresentation() || !fixture.session().waitAndPumpReplies() ||
        !fixture.session().beginFrame())
    {
        return 16;
    }

    const float view[16] = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F
    };
    const float projection[16] = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, -1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, -1.001001F, -1.0F,
        0.0F, 0.0F, -0.1001001F, 0.0F
    };
    const float eye[3]{};
    viewCameraUpdateTransient(
        ViewCameraProxy{fixture.session(), camera_ops},
        render_scene_id,
        render_scene.view,
        view,
        projection,
        eye
    );
    if (!fixture.session().trySubmitFrame() || !fixture.session().waitAndPumpReplies())
    {
        return 17;
    }
    for (std::uint32_t step{}; step < 8U; ++step)
    {
        registry.patch<simulation::ecs::WorldTransform3D>(mesh_entity, [step](auto& transform) {
            transform.value.linear() = Eigen::AngleAxisd{
                static_cast<double>(step + 1U) * 0.05,
                Eigen::Vector3d::UnitY()
            }.toRotationMatrix();
            transform.value.translation().z() = -2.0;
        });
        if (!scene_value.simulation().execute(*executor) || !scene_value.executeStablePoint() ||
            !scene_value.executePresentation() || !fixture.session().waitAndPumpReplies() ||
            !fixture.session().beginFrame() || !fixture.session().trySubmitFrame() ||
            !fixture.session().waitAndPumpReplies())
        {
            return 18;
        }
    }
    if (!fixture.session().beginFrame())
    {
        return 19;
    }
    fixture.flush(8U);
    const auto pixels = fixture.readback(render_scene);
    if (fixture.lastReadback().status != 0U)
    {
        return 19;
    }
    std::size_t lit_pixels{};
    for (std::size_t offset{}; offset + 3U < pixels.size(); offset += 4U)
    {
        if ((pixels[offset] | pixels[offset + 1U] | pixels[offset + 2U]) != 0U)
        {
            ++lit_pixels;
        }
    }
    std::printf(
        "scene_system_path=1,lit_pixels=%zu,mesh_handle=%u:%u,material_handle=%u:%u,validation_errors=%d\n",
        lit_pixels,
        mesh.handle.index,
        mesh.handle.gen,
        material.handle.index,
        material.handle.gen,
        validation_errors.load()
    );
    scene_instance->reset();
    return lit_pixels >= 64U && validation_errors.load() == 0 ? 0 : 20;
}
