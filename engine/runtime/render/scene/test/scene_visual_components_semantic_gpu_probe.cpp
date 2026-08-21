#include "DeviceRenderFixture.hpp"

#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/HeightFogComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/render/components/3d/WaterSurfaceComponent.hpp>
#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/function/render/client/resources/material/GraphMaterialData.hpp>

#include <lux/engine/function/render/client/genops/FogOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/Grid3DOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LightOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/LinearDepthOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredGBufferOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/DeferredLightingOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/MeshShadowOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ShadowMapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/SkyboxOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/TonemapOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/ViewCameraOperation.ops.hpp>
#include <lux/engine/function/render/client/genops/WaterOperation.ops.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <span>
#include <vector>

#include <entt/entt.hpp>

namespace
{
    using namespace lux::render;

    [[nodiscard]] std::vector<std::uint32_t> loadSpirv(const char* path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return {};
        const auto size = stream.tellg();
        if (size <= 0 || size % sizeof(std::uint32_t) != 0)
            return {};
        std::vector<std::uint32_t> words(
            static_cast<std::size_t>(size) / sizeof(std::uint32_t));
        stream.seekg(0, std::ios::beg);
        stream.read(
            reinterpret_cast<char*>(words.data()),
            static_cast<std::streamsize>(size));
        return stream ? std::move(words) : std::vector<std::uint32_t>{};
    }

    [[nodiscard]] bool expect(bool condition, const char* message)
    {
        if (!condition)
            std::fprintf(
                stderr,
                "scene visual components semantic: %s\n",
                message);
        return condition;
    }

    template <class Config>
    [[nodiscard]] std::pair<FeatureAddedReply, FeatureTypeRegisteredReply>
    attach(
        lux::rendertest::DeviceRenderFixture& fixture,
        RenderSceneId scene,
        const FeatureFactory& factory,
        const Config& config)
    {
        const auto registered = fixture.awaitControl(
            fixture.control().registerFeatureType(factory));
        const auto added = fixture.awaitControl(
            fixture.control().addFeature(
                scene,
                registered.feature_type_id,
                config));
        return {added, registered};
    }

    [[nodiscard]] std::size_t changedPixels(
        std::span<const std::uint8_t> before,
        std::span<const std::uint8_t> after,
        std::uint8_t tolerance = 8u)
    {
        const auto pixels = std::min(before.size(), after.size()) / 4u;
        std::size_t changed = 0u;
        for (std::size_t index = 0u; index < pixels; ++index)
        {
            bool differs = false;
            for (std::size_t channel = 0u; channel < 3u; ++channel)
            {
                const auto a = before[index * 4u + channel];
                const auto b = after[index * 4u + channel];
                differs = differs ||
                    (a > b ? a - b : b - a) > tolerance;
            }
            changed += differs ? 1u : 0u;
        }
        return changed;
    }

    [[nodiscard]] std::size_t bluePixels(
        std::span<const std::uint8_t> pixels)
    {
        std::size_t count = 0u;
        for (std::size_t index = 0u; index + 3u < pixels.size(); index += 4u)
        {
            const auto blue = pixels[index + 0u];
            const auto green = pixels[index + 1u];
            const auto red = pixels[index + 2u];
            count += blue > red + 24u && blue > green + 24u ? 1u : 0u;
        }
        return count;
    }

    void setIdentityCamera(
        lux::rendertest::DeviceRenderFixture& fixture,
        const lux::rendertest::DeviceRenderFixture::SceneView& scene,
        const ViewCameraOperationIds& operations)
    {
        constexpr float identity[16]{
            1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f};
        RenderLargePosition3D origin{};
        viewCameraUpdate(
            ViewCameraProxy{fixture.session(), operations},
            scene.scene_id,
            scene.view,
            identity,
            identity,
            origin,
            1024.0f);
    }
}

int main()
{
    using namespace lux::render;

    std::atomic<int> validation_errors{0};
    lux::rendertest::DeviceRenderFixture::Options options{};
    options.enable_validation = true;
    options.validation_errors = &validation_errors;

    int result = 0;
    {
        lux::rendertest::DeviceRenderFixture fixture(
            128u,
            128u,
            "scene_visual_components_semantic_gpu_probe",
            options);
        if (!fixture.ok())
        {
            std::printf("SKIP: Vulkan device unavailable\n");
            return 0;
        }

        RenderControlSession::CreateSceneConfig scene_config{};
        scene_config.name = "SceneVisualComponentsSemantic";
        scene_config.scene_origin_page[0] = 1'000'000;
        scene_config.scene_origin_page[1] = -2'000'000;
        scene_config.scene_origin_page[2] = 3'000'000;
        const auto scene = fixture.makeSceneWithView(
            scene_config,
            "SceneVisualComponentsSemanticView");

        const auto [camera_feature, camera_registration] = attach(
            fixture,
            scene.scene_id,
            kViewCameraFeatureFactory,
            ViewCameraCommTag{});
        const auto camera_ops = ViewCameraOperationIds::fromOps(
            camera_registration.ops,
            camera_registration.op_count);
        result |= !expect(
            camera_feature.feature.isValid() && camera_ops.valid(),
            "view camera feature attaches");
        setIdentityCamera(fixture, scene, camera_ops);

        const auto [light_feature, light_registration] = attach(
            fixture,
            scene.scene_id,
            kLightFeatureFactory,
            LightCommTag{});
        const auto light_ops = LightOperationIds::fromOps(
            light_registration.ops,
            light_registration.op_count);
        result |= !expect(
            light_feature.feature.isValid() && light_ops.valid(),
            "Light feature attaches before deferred consumers");

        const auto gbuffer_fragment = loadSpirv(
            LUX_SCENE_VISUAL_COMPONENTS_GBUFFER_SPV);
        const auto forward_fragment = loadSpirv(
            LUX_SCENE_VISUAL_COMPONENTS_FORWARD_SPV);
        result |= !expect(
            !gbuffer_fragment.empty() && !forward_fragment.empty(),
            "deterministic semantic shaders are available");
        lux::rdesc::ShaderInfo shader_info{};
        shader_info.entry_points.push_back({
            "main",
            lux::rdesc::EShaderType::FRAGMENT});
        const auto shader_info_bytes =
            lux::rdesc::ShaderInfo::serialize(shader_info);
        const auto gbuffer_shader = fixture.awaitControl(
            fixture.control().compileShader(
                std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(
                        gbuffer_fragment.data()),
                    gbuffer_fragment.size() * sizeof(std::uint32_t)},
                shader_info_bytes));
        const auto forward_shader = fixture.awaitControl(
            fixture.control().compileShader(
                std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(
                        forward_fragment.data()),
                    forward_fragment.size() * sizeof(std::uint32_t)},
                shader_info_bytes));
        result |= !expect(
            gbuffer_shader.status == 0u &&
                gbuffer_shader.shader.isValid() &&
                forward_shader.status == 0u &&
                forward_shader.shader.isValid(),
            "deterministic semantic shaders compile");

        const auto [material_feature, material_registration] = attach(
            fixture,
            scene.scene_id,
            kMaterialFeatureFactory,
            MaterialCommTag{});
        const auto [mesh_feature, mesh_registration] = attach(
            fixture,
            scene.scene_id,
            kMeshStackFeatureFactory,
            MeshStackCommTag{});
        ShadowMapCommConfig shadow_map_config{};
        shadow_map_config.atlas_page_resolution = 256u;
        shadow_map_config.atlas_page_count = 1u;
        const auto [shadow_map_feature, shadow_map_registration] = attach(
            fixture,
            scene.scene_id,
            kShadowMapFeatureFactory,
            shadow_map_config);
        const auto [mesh_shadow_feature, mesh_shadow_registration] = attach(
            fixture,
            scene.scene_id,
            kMeshShadowFeatureFactory,
            MeshShadowCommConfig{});
        const auto [depth_feature, depth_registration] = attach(
            fixture,
            scene.scene_id,
            kLinearDepthFeatureFactory,
            LinearDepthCommConfig{});
        DeferredGBufferCommConfig gbuffer_config{};
        gbuffer_config.gbuffer_graph_fragment_shader =
            gbuffer_shader.shader;
        const auto [gbuffer_feature, gbuffer_registration] = attach(
            fixture,
            scene.scene_id,
            kDeferredGBufferFeatureFactory,
            gbuffer_config);
        result |= !expect(
            material_feature.feature.isValid() &&
                material_registration.feature_type_id != 0u &&
                mesh_feature.feature.isValid() &&
                mesh_registration.feature_type_id != 0u &&
                shadow_map_feature.feature.isValid() &&
                shadow_map_registration.feature_type_id != 0u &&
                mesh_shadow_feature.feature.isValid() &&
                mesh_shadow_registration.feature_type_id != 0u &&
                depth_feature.feature.isValid() &&
                depth_registration.feature_type_id != 0u &&
                gbuffer_feature.feature.isValid() &&
                gbuffer_registration.feature_type_id != 0u,
            "mesh depth producer attaches");
        if (result != 0)
        {
            std::fprintf(
                stderr,
                "material=%d mesh=%d gbuffer=%d errors=[%s] [%s] [%s]\n",
                material_feature.feature.isValid() ? 1 : 0,
                mesh_feature.feature.isValid() ? 1 : 0,
                gbuffer_feature.feature.isValid() ? 1 : 0,
                formatRenderError(
                    renderErrorRegistry(), material_feature.error).c_str(),
                formatRenderError(
                    renderErrorRegistry(), mesh_feature.error).c_str(),
                formatRenderError(
                    renderErrorRegistry(), gbuffer_feature.error).c_str());
        }

        DeferredLightingCommConfig lighting_config{};
        lighting_config.read_mode = ELightingReadMode::SAMPLED;
        lighting_config.enable_clustered = 0u;
        const auto [lighting_feature, lighting_registration] = attach(
            fixture,
            scene.scene_id,
            kDeferredLightingFeatureFactory,
            lighting_config);
        result |= !expect(
            lighting_feature.feature.isValid() &&
                lighting_registration.feature_type_id != 0u,
            "deferred lighting produces LitColor for Fog");

        const auto material_ops = MaterialOperationIds::fromOps(
            material_registration.ops,
            material_registration.op_count);
        const auto mesh_ops = MeshStackOperationIds::fromOps(
            mesh_registration.ops,
            mesh_registration.op_count);
        lux::render::GraphMaterialData graph_material{};
        auto material_submit = uploadGraphMaterial(
            MaterialUploadClient{
                fixture.uploadClientForTest(),
                material_ops},
            graph_material,
            gbuffer_shader.shader,
            forward_shader.shader);
        result |= !expect(
            material_submit.has_value(),
            "semantic material upload is admitted");
        if (!material_submit)
            return result;
        const auto material = fixture.awaitUpload(
            std::move(*material_submit));
        result |= !expect(
            material.status == 0u && !material.handle.isNull(),
            "semantic material becomes resident");

        lux::rdesc::Mesh triangle;
        triangle.vertices.resize(3u);
        triangle.vertices[0u].position = {-0.85f, -0.75f, 0.5f};
        triangle.vertices[1u].position = {0.85f, -0.75f, 0.5f};
        triangle.vertices[2u].position = {0.0f, 0.85f, 0.5f};
        for (auto& vertex : triangle.vertices)
        {
            vertex.normal = {0.0f, 0.0f, -1.0f};
            vertex.tangent = {1.0f, 0.0f, 0.0f};
            vertex.bitangent = {0.0f, 1.0f, 0.0f};
        }
        triangle.indices = {0u, 1u, 2u};
        auto mesh_submit = uploadMesh(
            MeshStackUploadClient{
                fixture.uploadClientForTest(),
                mesh_ops},
            triangle);
        result |= !expect(
            mesh_submit.has_value(),
            "semantic mesh upload is admitted");
        if (!mesh_submit)
            return result;
        const auto mesh = fixture.awaitUpload(std::move(*mesh_submit));
        result |= !expect(
            mesh.status == 0u && !mesh.handle.isNull(),
            "semantic mesh becomes resident");
        if (result != 0)
            return result;

        AddMeshInstancePayload mesh_instance{};
        mesh_instance.scene_id = scene.scene_id;
        mesh_instance.mesh = mesh.handle;
        mesh_instance.material = material.handle;
        mesh_instance.transform.basis_local[0u] = 1.0f;
        mesh_instance.transform.basis_local[5u] = 1.0f;
        mesh_instance.transform.basis_local[10u] = 1.0f;
        const auto mesh_object = fixture.await(
            MeshStackProxy{fixture.session(), mesh_ops}.addMeshInstance(
                mesh_instance));
        result |= !expect(
            mesh_object.status == MeshInstanceCreateStatus::Ok &&
                static_cast<bool>(mesh_object.object),
            "semantic depth object is created");
        MeshStackProxy{fixture.session(), mesh_ops}.makeInstanceVisibleForView({
            scene.scene_id,
            scene.view,
            mesh_object.object});

        const auto [grid_feature, grid_registration] = attach(
            fixture,
            scene.scene_id,
            kGrid3DFeatureFactory,
            Grid3DCommConfig{});
        result |= !expect(
            grid_feature.feature.isValid() &&
                grid_registration.feature_type_id != 0u,
            "depth-writing grid feature attaches");

        const auto [sky_feature, sky_registration] = attach(
            fixture,
            scene.scene_id,
            kSkyboxFeatureFactory,
            SkyboxCommConfig{});
        const auto sky_ops = SkyboxOperationIds::fromOps(
            sky_registration.ops,
            sky_registration.op_count);
        result |= !expect(
            sky_feature.feature.isValid() && sky_ops.valid(),
            "skybox feature attaches");

        const auto [fog_feature, fog_registration] = attach(
            fixture,
            scene.scene_id,
            kFogFeatureFactory,
            FogCommConfig{});
        const auto fog_ops = FogOperationIds::fromOps(
            fog_registration.ops,
            fog_registration.op_count);
        result |= !expect(
            fog_feature.feature.isValid() && fog_ops.valid(),
            "fog feature attaches");

        const auto [water_feature, water_registration] = attach(
            fixture,
            scene.scene_id,
            kWaterFeatureFactory,
            WaterCommConfig{});
        const auto water_ops = WaterOperationIds::fromOps(
            water_registration.ops,
            water_registration.op_count);
        result |= !expect(
            water_feature.feature.isValid() && water_ops.valid(),
            "water feature attaches at a non-zero scene origin page");
        const auto [tonemap_feature, tonemap_registration] = attach(
            fixture,
            scene.scene_id,
            kTonemapFeatureFactory,
            TonemapCommConfig{});
        result |= !expect(
            tonemap_feature.feature.isValid() &&
                tonemap_registration.feature_type_id != 0u,
            "tonemap publishes the HDR environment chain to SceneColor");
        if (result != 0)
            return result;

        std::vector<std::byte> sky_pixels(8u * 4u * 4u);
        for (std::size_t index = 0u; index < sky_pixels.size(); index += 4u)
        {
            sky_pixels[index + 0u] = std::byte{24u};
            sky_pixels[index + 1u] = std::byte{64u};
            sky_pixels[index + 2u] = std::byte{224u};
            sky_pixels[index + 3u] = std::byte{255u};
        }
        auto sky_submit = fixture.uploadClientForTest().tryCreateTexture2DCopy(
            sky_pixels,
            8,
            4,
            4,
            EPixelFormat::RGBA8_UNORM,
            false);
        result |= !expect(
            sky_submit.has_value(),
            "deterministic sky texture upload is admitted");
        if (!sky_submit)
            return result;
        const auto sky_texture = fixture.awaitUpload(std::move(*sky_submit));
        result |= !expect(
            sky_texture.status == 0u && !sky_texture.handle.isNull(),
            "deterministic sky texture becomes resident");
        if (result != 0)
            return result;

        lux::ecs::Registry environment_registry;
        const auto sky_entity = environment_registry.create();
        auto& sky_component =
            environment_registry.emplace<lux::ecs::SkyboxComponent>(
                sky_entity);
        sky_component.equirect_texture_id = uuids::uuid::from_string(
            "5f0eb59b-e65b-4f4e-99ba-e52bc5f47c01").value();
        sky_component.rotation_radians = 0.75f;
        sky_component.intensity = 1.25f;

        const auto fog_entity = environment_registry.create();
        environment_registry.emplace<lux::ecs::HeightFogComponent>(
            fog_entity);

        const auto sun_entity = environment_registry.create();
        auto& sun_component = environment_registry.emplace<
            lux::ecs::DirectionalLightComponent>(sun_entity);
        sun_component.direction =
            Eigen::Vector3f{0.0f, -1.0f, -0.25f}.normalized();
        sun_component.color = Eigen::Vector3f{1.0f, 0.92f, 0.75f};
        sun_component.intensity = 4.0f;

        const auto water_entity = environment_registry.create();
        auto& water_component = environment_registry.emplace<
            lux::ecs::WaterSurfaceComponent>(water_entity);
        water_component.half_extent = Eigen::Vector2f{12.0f, 12.0f};

        SkyboxSetEquirectPayload sky{};
        sky.scene_id = scene.scene_id;
        sky.feature = sky_feature.feature;
        sky.texture = sky_texture.handle;
        sky.rotation_radians = sky_component.rotation_radians;
        sky.intensity = sky_component.intensity;
        SkyboxProxy{fixture.session(), sky_ops}.setEquirect(sky);

        FogSetParamsPayload fog{};
        fog.scene_id = scene.scene_id;
        fog.feature = fog_feature.feature;
        FogProxy{fixture.session(), fog_ops}.setParams(fog);
        fixture.flush(4);
        const auto before_patch = fixture.readback(scene);
        result |= !expect(
            bluePixels(before_patch) > 128u * 128u / 2u,
            "initial sky occupies the deterministic sky region");

        const auto sky_id_before = sky_component.equirect_texture_id;
        const float sky_rotation_before = sky_component.rotation_radians;
        const float sky_intensity_before = sky_component.intensity;
        environment_registry.patch<lux::ecs::HeightFogComponent>(
            fog_entity,
            [](lux::ecs::HeightFogComponent& component)
            {
                component.enabled = true;
                component.color = Eigen::Vector3f{0.92f, 0.12f, 0.08f};
                component.density = 0.25f;
                component.maximum_opacity = 0.85f;
            });
        const auto& applied_fog =
            environment_registry.get<lux::ecs::HeightFogComponent>(
                fog_entity);
        const auto& preserved_sky =
            environment_registry.get<lux::ecs::SkyboxComponent>(
                sky_entity);
        result |= !expect(
            applied_fog.enabled &&
                std::abs(applied_fog.density - 0.25f) < 0.0001f &&
                std::abs(applied_fog.maximum_opacity - 0.85f) <
                    0.0001f,
            "registry.patch adopts the Fog component atomically");
        result |= !expect(
            preserved_sky.equirect_texture_id == sky_id_before &&
                preserved_sky.rotation_radians == sky_rotation_before &&
                preserved_sky.intensity == sky_intensity_before,
            "Fog-only registry.patch preserves the independent sky component");

        fog.color[0u] = applied_fog.color.x();
        fog.color[1u] = applied_fog.color.y();
        fog.color[2u] = applied_fog.color.z();
        fog.enabled = applied_fog.enabled ? 1u : 0u;
        fog.density = applied_fog.density;
        fog.start_distance = applied_fog.start_distance;
        fog.reference_height = applied_fog.reference_height;
        fog.height_falloff = applied_fog.height_falloff;
        fog.maximum_opacity = applied_fog.maximum_opacity;
        FogProxy{fixture.session(), fog_ops}.setParams(fog);

        WaterSetEnvironmentPayload water_environment{};
        water_environment.scene_id = scene.scene_id;
        water_environment.feature = water_feature.feature;
        water_environment.fog_color[0u] = applied_fog.color.x();
        water_environment.fog_color[1u] = applied_fog.color.y();
        water_environment.fog_color[2u] = applied_fog.color.z();
        water_environment.fog_density = applied_fog.density;
        water_environment.fog_start_distance = applied_fog.start_distance;
        water_environment.fog_reference_height =
            applied_fog.reference_height;
        water_environment.fog_height_falloff =
            applied_fog.height_falloff;
        water_environment.fog_maximum_opacity =
            applied_fog.maximum_opacity;
        water_environment.fog_enabled = applied_fog.enabled ? 1u : 0u;
        WaterProxy{fixture.session(), water_ops}.setEnvironment(
            water_environment);

        WaterSurfaceDesc surface{};
        surface.transform.basis_local[0] = 1.0f;
        surface.transform.basis_local[5] = 1.0f;
        surface.transform.basis_local[10] = 1.0f;
        surface.half_extent[0] = water_component.half_extent.x();
        surface.half_extent[1] = water_component.half_extent.y();
        surface.transition_milliseconds = 0u;
        const auto water_surface = fixture.await(
            WaterProxy{fixture.session(), water_ops}.createSurface({
                scene.scene_id,
                surface}));
        result |= !expect(
            water_surface.status == 0u &&
                !water_surface.handle.isNull(),
            "water surface is accepted at the large absolute origin");

        DirectionalLightDesc sun{};
        sun.direction = sun_component.direction;
        sun.color = sun_component.color;
        sun.intensity = sun_component.intensity;
        const auto sun_reply = fixture.await(lightCreate(
            LightProxy{fixture.session(), light_ops},
            scene.scene_id,
            LightDescriptor{sun}));
        result |= !expect(
            sun_reply.status == 0u &&
                !sun_reply.handle.isNull(),
            "Sun resource is accepted at the large absolute origin");

        fixture.flush(6);
        const auto after_patch = fixture.readback(scene);
        const auto changed = changedPixels(before_patch, after_patch);
        result |= !expect(
            changed > 128u * 128u / 4u,
            "Fog/Water environment changes a stable pixel region");

        const auto sky_stats = fixture.awaitControl(
            SkyboxControlClient{fixture.control(), sky_ops}.stats({
                scene.scene_id}));
        result |= !expect(
            sky_stats.active_mode != 0u &&
                std::abs(sky_stats.rotation_radians -
                    preserved_sky.rotation_radians) < 0.0001f &&
                std::abs(sky_stats.intensity - preserved_sky.intensity) <
                    0.0001f &&
                sky_stats.draws != 0u,
            "sky feature preserves texture mode, rotation and intensity");
        const auto water_stats = fixture.awaitControl(
            WaterControlClient{fixture.control(), water_ops}.stats({
                scene.scene_id}));
        result |= !expect(
            water_stats.resident_surfaces == 1u &&
                water_stats.visible_patches != 0u,
            "water feature reports one visible large-origin surface");
        const auto light_stats = fixture.awaitControl(
            LightControlClient{fixture.control(), light_ops}.stats({
                scene.scene_id}));
        result |= !expect(
            light_stats.directional_lights == 1u,
            "Sun feature reports one directional light");

        fixture.control().destroyTexture(sky_texture.handle);
        fixture.flush(4);
    }

    result |= !expect(
        validation_errors.load(std::memory_order_relaxed) == 0,
        "Vulkan validation reports zero errors");
    return result == 0 ? 0 : 1;
}
