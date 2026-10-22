#include "../../material/test/MaterialTest.hpp"
#include "FullscreenVertex.hpp"
#include "GraphShadowSampler.hpp"
#include "SceneCapture.hpp"
#include "SceneComparison.hpp"
#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshShadowFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/ShadowMapFeature.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>

struct SamplerProbe
{
    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        GraphImageDesc color{64, 64};
        color.extent.target_relative = true;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        ports.resources.push_back(
            {"depths", EFeaturePort::OUTPUT, GraphImageDesc{64, 64, 1, 2, rdesc::ETextureFormat::D32_SFLOAT}}
        );
        return {};
    }

    static RenderResult<void> build(const void*, std::string_view key, FeatureBuildContext& context) noexcept
    {
        auto depth = context.resource("depths");
        // A culled sampler input leaves a hole before live inputs. Attach must not instantiate it.
        (void)context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS});
        for (std::uint32_t layer = 0; layer < 2; ++layer)
        {
            auto cleared = context.graph.addTransferPass(
                featureKey(FeatureName{"test.SamplerProbe"}, key, "clear" + std::to_string(layer)),
                TransferWork{{ClearImage{{depth, GraphImageRange{2, 0, 1, layer, 1}}, {}, layer ? .75F : .25F}}}
            );
            if (!cleared)
            {
                return lux::cxx::unexpected(cleared.error());
            }
            depth = cleared->written.front();
        }
        rdesc::SpirvArtifact shader;
        shader.stages = {
            {rdesc::EShaderType::VERTEX, "main", {std::begin(kFullscreenVertex), std::end(kFullscreenVertex)}},
            {rdesc::EShaderType::FRAGMENT, "main", {std::begin(kGraphShadowSampler), std::end(kGraphShadowSampler)}}
        };
        CHECK(rdesc::ShaderInfo::deserialize(kGraphShadowSamplerReflection, shader.interface));
        shader.interface.entry_points.push_back({"main", rdesc::EShaderType::VERTEX});
        RasterWork work;
        work.interface.program = take(context.graph.addProgram(std::move(shader)));
        work.interface.resources = {{"depths", {depth}}};
        auto ordinary = context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE});
        auto comparison =
            context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS_EQUAL});
        work.interface.samplers = {{"ordinary", {ordinary}}, {"comparison", {comparison}, kGraphInvalidIndex, true}};
        work.colors.push_back({{context.resource("color")}});
        auto drawn =
            context.graph.addRasterPass(featureKey(FeatureName{"test.SamplerProbe"}, key, "sample"), std::move(work));
        if (!drawn)
        {
            return lux::cxx::unexpected(drawn.error());
        }
        if (auto published = context.publish("depths", depth); !published)
        {
            return published;
        }
        return context.publish("color", drawn->written.front());
    }
};

namespace lux::render
{
    template <>
    RenderResult<void> RenderServer::prepareFeature<SamplerProbe>(RegistrationBatch& batch, std::string_view) noexcept
    {
        static const FeatureDescriptor descriptor{
            FeatureName{"test.SamplerProbe"},
            {},
            "Ordinary and comparison array samplers",
            SamplerProbe::describe
        };
        static const FeatureGraphOps graph{SamplerProbe::build};
        batch.addFeature({&descriptor, &graph});
        return {};
    }
} // namespace lux::render

static void verifySamplers(RenderServer& server, const RenderServerConfig& config)
{
    const auto probe = take(server.registerFeature<SamplerProbe>());
    const auto capture = take(server.registerFeature<CaptureFeature>());
    CommandList bootstrap;
    bootstrap.commands.push_back({std::string(ListCommands::name), take(server.commandId<ListCommands>())});
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server.run()); });
    const auto commands = call<ListCommands>(*client, bootstrap, {});
    CapturedPixels pixels;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    CreateEffectRequest request{target};
    request.definition.features.push_back({probe, "samplers", {}});
    request.definition.features.push_back({capture, "pixels", take(ConfigValue::make(CaptureConfig{&pixels}))});
    request.definition.connections.push_back({{0, "color"}, {1, "color"}});
    request.definition.outputs = {{{0, "color"}, GraphOutputRequirements{target.format, 1}}, {{1, "pixels"}, {}}};
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto output = call<CreateTarget>(*client, commands, {target}).target;
    const auto attachment = call<Attach>(*client, commands, {scene, view, output, effect, {}}).attachment;
    const auto deadline = Clock::now() + 10s;
    while (call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) < 4)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    detach(*client, commands, attachment);
    while (!pixels.ready.load(std::memory_order_acquire))
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    unsigned rendered = 0;
    for (const auto& frame : pixels.frames)
    {
        const auto* pixel = frame.data() + 32 * pixels.pitch + 32 * 4;
        if (pixel[3] == std::byte{})
        {
            continue;
        }
        CHECK(pixel[0] == std::byte{0} && pixel[1] == std::byte{255} && pixel[2] == std::byte{64});
        ++rendered;
    }
    CHECK(rendered);
    call<RemoveEffect>(*client, commands, {effect});
    call<RemoveScene>(*client, commands, {scene});
    server.stop();
    thread.join();
    CHECK(server.stats().validation_errors == 0);
    std::puts("comparison/ordinary sampler and two array layers through Attach without bindless: passed");
}

struct Parameters
{
    MeshViewParameters mesh;
    DeferredLightingParameters lighting;
    TonemapParameters tone;
    std::array<float, 3> padding{};
    ShadowCamera shadow;
    ClusteredLightingParameters cluster;
};

static rdesc::ShaderBlock parameterLayout()
{
    auto result = meshViewParameterBlock();
    result.size = sizeof(Parameters);
    for (auto field : deferredLightingParameterBlock().fields)
    {
        field.offset += offsetof(Parameters, lighting);
        result.fields.push_back(std::move(field));
    }
    auto field = tonemapParameterBlock().fields.front();
    field.offset += offsetof(Parameters, tone);
    result.fields.push_back(std::move(field));
    for (auto field : clusteredLightingParameterBlock().fields)
    {
        field.offset += offsetof(Parameters, cluster);
        result.fields.push_back(std::move(field));
    }
    return result;
}

static std::vector<std::byte> parameterBytes(const Parameters& value)
{
    auto bytes = std::as_bytes(std::span(&value, 1));
    return {bytes.begin(), bytes.end()};
}

static rdesc::MaterialProgramSet material(bool masked = false, bool unlit = false)
{
    using namespace lux::material;
    MaterialGraph graph;
    graph.shading_model = unlit ? rdesc::ELightingTechnique::UNLIT : rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
    graph.render_state.double_sided = true;
    graph.render_state.alpha_mode = masked ? rdesc::EAlphaMode::MASK : rdesc::EAlphaMode::OPAQUE_SURFACE;
    auto output = material_test::add(graph, MaterialOutputSurface{});
    graph.param_slots = {{"albedo", EValueType::VEC4, {.7F, .7F, .7F, 1}}};
    auto base = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
    CHECK(material_test::connect(graph, base, 0, output, 0));
    if (masked)
    {
        auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        auto alpha = material_test::add(graph, MaterialSwizzle{EValueType::VEC2, EValueType::FLOAT, {0, 0, 0, 0}});
        CHECK(material_test::connect(graph, uv, 0, alpha, 0));
        CHECK(material_test::connect(graph, alpha, 0, output, 1));
    }
    constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER, rdesc::EMaterialPurpose::SHADOW};
    auto result = compileMaterial(graph, purposes);
    CHECK(result);
    return std::move(result->programs);
}

static std::array<double, 3> project(const MeshVisibilityView& view, std::array<double, 3> point)
{
    std::array<double, 4> clip{};
    for (unsigned row = 0; row < 4; ++row)
    {
        clip[row] = view.world_to_clip[12 + row];
        for (unsigned column = 0; column < 3; ++column)
        {
            clip[row] += view.world_to_clip[column * 4 + row] * point[column];
        }
    }
    CHECK(std::abs(clip[3]) > 1e-8);
    return {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3u;
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.shadow", true};
    config.frames_in_flight = fif;
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
    bool clustered = false;
    DeviceOptions device;
    device.multiple_queues = true;
    device.texture_descriptors = true;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view option = argv[i];
        if (option == "--clustered")
        {
            clustered = true;
        }
        else if (option == "--paged")
        {
            device.buffer_device_address = true;
        }
        else if (option == "--single-queue")
        {
            device.multiple_queues = false;
        }
        else if (option == "--same-family")
        {
            device.prefer_dedicated_queues = false;
        }
        else if (option == "--no-bindless")
        {
            device.texture_descriptors = false;
        }
        else
        {
            CHECK(false);
        }
    }
    auto server = take(createRenderServer(config, *runtime, device));
    if (!device.texture_descriptors)
    {
        verifySamplers(*server, config);
        server.reset();
        runtime->requestStop();
        CHECK(runtime->join());
        return 0;
    }
    const auto gbuffer = take(server->registerFeature<GBufferFeatureDescriptor>());
    const auto lighting = take(server->registerFeature<DeferredLightingFeatureDescriptor>());
    const auto tonemap = take(server->registerFeature<TonemapFeatureDescriptor>());
    const auto projection = take(server->registerFeature<ShadowMapFeatureDescriptor>());
    const auto visibility = take(server->registerFeature<MeshVisibilityFeatureDescriptor>());
    const auto shadow = take(server->registerFeature<MeshShadowFeatureDescriptor>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto cluster_feature = take(server->registerFeature<ClusteredLightingFeatureDescriptor>());
    const auto comparison = take(server->registerFeature<SceneComparison>());
    CommandList bootstrap;
    bootstrap.commands.push_back({std::string(ListCommands::name), take(server->commandId<ListCommands>())});
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = call<ListCommands>(*client, bootstrap, {});
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto meshes = prepare(*client, commands, kMeshStorageType, "meshes");
    const auto materials = prepare(*client, commands, kMaterialStorageType, "materials");
    const auto instances = prepare(*client, commands, kMeshInstanceStorageType, "instances", scene);
    const auto light_storage = prepare(*client, commands, kDirectLightingStorageType, "direct_lights", scene);

    struct Vertex
    {
        std::array<float, 3> position, normal;
        std::array<float, 2> uv;
    };

    const std::array<Vertex, 4> vertices{
        {{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
         {{1, -1, 0}, {0, 0, 1}, {1, 0}},
         {{1, 1, 0}, {0, 0, 1}, {1, 1}},
         {{-1, 1, 0}, {0, 0, 1}, {0, 1}}}
    };
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    std::vector<std::byte> source(sizeof(vertices) + sizeof(indices));
    std::memcpy(source.data(), vertices.data(), sizeof(vertices));
    std::memcpy(source.data() + sizeof(vertices), indices.data(), sizeof(indices));
    MeshDescription geometry{
        sizeof(Vertex),
        4,
        sizeof(vertices),
        4,
        6,
        {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT},
         {2, 12, rdesc::ETextureFormat::RGB32_SFLOAT},
         {1, 24, rdesc::ETextureFormat::RG32_SFLOAT}},
        {{0, 6, 0}}
    };
    geometry.bounds = {{0, 0, 0, 1}, {1, 1, 0, 0}};
    auto mesh = call<CreateMesh>(*client, commands, {meshes, geometry, source}).mesh;
    auto caster_mesh = call<CreateMesh>(*client, commands, {meshes, geometry, source}).mesh;
    auto program = material();
    MaterialParameters material_values{{.7F, .7F, .7F, 1}};
    auto material_id = call<CreateMaterial>(*client, commands, {materials, program, material_values}).material;
    auto unlit_program = material(false, true);
    auto unlit_material = call<CreateMaterial>(
                              *client,
                              commands,
                              {materials, unlit_program, MaterialParameters{{.2F, .1F, .05F, 1}}}
    ).material;
    std::array<MeshInstanceDescription, 4> descriptions{
        {{mesh, material_id}, {caster_mesh, material_id}, {caster_mesh, material_id}, {mesh, unlit_material}}
    };
    descriptions[1].transform[0] = -.25F; // Mirrored caster with nonuniform scale.
    descriptions[1].transform[5] = .2F;
    descriptions[1].transform[14] = .5F;
    descriptions[2].transform[12] = 100;
    descriptions[3].transform[0] = descriptions[3].transform[5] = .08F;
    descriptions[3].transform[12] = descriptions[3].transform[13] = .8F;
    descriptions[3].transform[14] = .2F;
    auto instance_ids = call<CreateMeshInstances>(*client, commands, {instances, descriptions}).instances;
    RasterMeshGroup group{geometry, program, {caster_mesh, material_id}};
    RasterMeshGroup receiver_group{geometry, program, {mesh, material_id}};
    RasterMeshGroup unlit_group{geometry, unlit_program, {mesh, unlit_material}};
    unlit_group.front_counter_clockwise = false;
    // Positive-height viewport: this unmirrored quad is clockwise in framebuffer coordinates.
    receiver_group.front_counter_clockwise = false;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    Parameters parameters;
    parameters.mesh.view[10] = -1;
    parameters.mesh.view[14] = 2;
    parameters.mesh.projection[10] = 1 / 2.9F;
    parameters.mesh.projection[14] = -.1F / 2.9F;
    parameters.lighting.inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2.9F, 0, 0, 0, 1.9F, 1};
    parameters.lighting.camera_position = {0, 0, 2, 0};
    parameters.shadow.inverse_view_projection = parameters.lighting.inverse_view_projection;
    parameters.shadow.position_near = {0, 0, 2, .1F};
    parameters.shadow.direction_far = {0, 0, -1, 3};
    parameters.cluster =
        {parameters.shadow.inverse_view_projection, parameters.shadow.position_near, parameters.shadow.direction_far};
    auto layout = parameterLayout();
    std::array<unsigned, 4> reference_lit{}, reference_occluded{};
    bool masked = false;

    const auto run = [&](EDirectLightingBuffer kind,
                         bool cull,
                         std::uint32_t radius,
                         bool cast = true,
                         bool mixed = false,
                         bool seam = false)
    {
        VDirectLight light;
        if (kind == EDirectLightingBuffer::DIRECTIONAL)
        {
            light = DirectionalLightGpu{{1, 1, 1, 0}, {.6F, 0, -.8F, 0}};
        }
        else if (kind == EDirectLightingBuffer::POINT)
        {
            light = PointLightGpu{{seam ? -1.5F : -1.F, 0, 2, 6}, {1, 1, 1, 0}, {1, 0, 0, 0}};
        }
        else
        {
            const float inverse = 1 / std::sqrt(5.F);
            light = SpotLightGpu{{-1, 0, 2, 6}, {1, 1, 1, 0}, {inverse, 0, -2 * inverse, .7F}, {1, 0, 0, .95F}};
        }
        std::vector<VDirectLight> values{light, light};
        if (mixed)
        {
            const float inverse = 1 / std::sqrt(5.F);
            values = {
                PointLightGpu{},
                DirectionalLightGpu{{1, 1, 1, 0}, {.6F, 0, -.8F, 0}},
                PointLightGpu{{-1, 0, 2, 6}, {1, 1, 1, 0}, {1, 0, 0, 0}},
                SpotLightGpu{{-1, 0, 2, 6}, {1, 1, 1, 0}, {inverse, 0, -2 * inverse, .7F}, {1, 0, 0, .95F}}
            };
        }
        if (clustered && mixed)
        {
            for (unsigned i = 0; i < 64; ++i)
            {
                values.push_back(PointLightGpu{
                    {(int(i % 9) - 4) * .37F, (int(i % 7) - 3) * .31F, 1.5F, 3},
                    {.003F, .004F, .005F, 0},
                    {1, 0, 0, 0}
                });
            }
        }
        auto lights = call<CreateDirectLights>(*client, commands, {light_storage, 1, values}).lights;
        const ShadowMapShape shape{
            64,
            mixed                                        ? 9u
            : kind == EDirectLightingBuffer::DIRECTIONAL ? 2u
            : kind == EDirectLightingBuffer::POINT       ? 6u
                                                         : 1u
        };
        ShadowMapFeatureConfig projections;
        projections.shape = shape;
        projections.camera_offset = offsetof(Parameters, shadow);
        projections.pcf_radius = radius;
        ShadowLightSelection selected;
        selected.light = lights[1];
        selected.kind = kind;
        selected.cascade_far_distances = {1.75F, 3};
        selected.comparison_offset = 0;
        selected.caster_distance = 2;
        selected.depth_bias_constant = radius ? 1.F : 0.F;
        selected.depth_bias_slope = radius ? 1.F : 0.F;
        projections.casters = {selected};
        if (mixed)
        {
            auto point = selected;
            point.light = lights[2];
            point.kind = EDirectLightingBuffer::POINT;
            auto spot = selected;
            spot.light = lights[3];
            spot.kind = EDirectLightingBuffer::SPOT;
            projections.casters.push_back(std::move(point));
            projections.casters.push_back(std::move(spot));
        }
        if (!cast)
        {
            projections.casters.clear();
        }
        MeshVisibilityFeatureConfig visibility_config;
        visibility_config.groups = {group.group};
        visibility_config.view_count = shape.view_count;
        visibility_config.candidate_capacity = 8;
        visibility_config.frustum_culling = cull;
        MeshShadowFeatureConfig shadow_config{shape, {group}, 8};
        GBufferFeatureConfig surface{target};
        surface.groups = {receiver_group, group, unlit_group};
        surface.input_layout = layout;
        surface.visible_capacity = 8;
        DeferredLightingFeatureConfig light_config;
        light_config.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        light_config.extent.extent.target_relative = true;
        light_config.input_layout = layout;
        light_config.shadows = shape;
        CapturedPixels pixels, view_bytes, counts, surface_pixels, depth_pixels, indirect, differences, cluster_status;
        CreateEffectRequest request{target};
        request.definition.features.push_back({gbuffer, "gbuffer", take(ConfigValue::make(surface))});
        request.definition.features.push_back({projection, "projection", take(ConfigValue::make(projections))});
        request.definition.features.push_back({visibility, "visibility", take(ConfigValue::make(visibility_config))});
        request.definition.features.push_back({shadow, "shadow", take(ConfigValue::make(shadow_config))});
        request.definition.features.push_back({lighting, "lighting", take(ConfigValue::make(light_config))});
        request.definition.features.push_back(
            {tonemap, "tonemap", take(ConfigValue::make(TonemapFeatureConfig{target, EColorEncoding::LINEAR, layout}))}
        );
        request.definition.features.push_back({capture, "pixels", take(ConfigValue::make(CaptureConfig{&pixels}))});
        CaptureConfig view_capture{&view_bytes};
        view_capture.bytes = shape.view_count * sizeof(MeshVisibilityView);
        request.definition.features.push_back({capture, "views", take(ConfigValue::make(view_capture))});
        CaptureConfig count_capture{&counts};
        count_capture.bytes = shape.view_count * kMeshWindingCount * 4;
        request.definition.features.push_back({capture, "counts", take(ConfigValue::make(count_capture))});
        request.definition.features.push_back(
            {capture, "surface", take(ConfigValue::make(CaptureConfig{&surface_pixels}))}
        );
        request.definition.features.push_back(
            {capture, "depth", take(ConfigValue::make(CaptureConfig{&depth_pixels, rdesc::ETextureFormat::D32_SFLOAT}))}
        );
        CaptureConfig indirect_capture{&indirect};
        indirect_capture.bytes = shape.view_count * kMeshWindingCount * 20;
        request.definition.features.push_back({capture, "indirect", take(ConfigValue::make(indirect_capture))});
        auto& connections = request.definition.connections;
        for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
        {
            connections.push_back({{0, name}, {4, name}});
        }
        for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
        {
            connections.push_back({{1, name}, {4, name}});
        }
        connections.push_back({{1, "views"}, {2, "views"}});
        connections.push_back({{1, "views"}, {3, "views"}});
        connections.push_back({{1, "raster_states"}, {3, "raster_states"}});
        connections.push_back({{1, "views"}, {4, "shadow_views"}});
        connections.push_back({{1, "slices"}, {4, "shadow_slices"}});
        connections.push_back({{1, "light_ranges"}, {4, "shadow_ranges"}});
        connections.push_back({{2, "g0.visible"}, {3, "g0.members"}});
        connections.push_back({{2, "g0.commands"}, {3, "g0.commands"}});
        connections.push_back({{2, "g0.counts"}, {3, "g0.counts"}});
        connections.push_back({{3, "depth"}, {4, "shadow_depth"}});
        connections.push_back({{4, "hdr_color"}, {5, "hdr_color"}});
        connections.push_back({{5, "color"}, {6, "color"}});
        connections.push_back({{1, "views"}, {7, "color"}});
        connections.push_back({{2, "g0.counts"}, {8, "color"}});
        connections.push_back({{0, "albedo_metallic"}, {9, "color"}});
        connections.push_back({{0, "depth"}, {10, "color"}});
        connections.push_back({{2, "g0.commands"}, {11, "color"}});
        request.definition.outputs.push_back({{5, "color"}, GraphOutputRequirements{target.format, 1}});
        for (unsigned capture_index : {6u, 7u, 8u, 9u, 10u, 11u})
        {
            request.definition.outputs.push_back({{capture_index, "pixels"}, {}});
        }
        if (clustered)
        {
            ClusteredLightingFeatureConfig clusters;
            clusters.grid = {{7, 5, 9}, radius ? 1u : 65536u, EClusterDepth::LOGARITHMIC};
            clusters.external_lights = true;
            clusters.input_layout = layout;
            auto optimized = light_config;
            optimized.clusters = clusters.grid;
            request.definition.features.push_back({cluster_feature, "clusters", take(ConfigValue::make(clusters))}
            ); // 12
            request.definition.features.push_back({lighting, "clustered_lighting", take(ConfigValue::make(optimized))}
            );                                                                  // 13
            request.definition.features.push_back({comparison, "compare", {}}); // 14
            CaptureConfig compare_config{&differences};
            compare_config.bytes = 64 * 64 * 4;
            request.definition.features.push_back(
                {capture, "difference_pixels", take(ConfigValue::make(compare_config))}
            ); // 15
            CaptureConfig status_config{&cluster_status};
            status_config.bytes = sizeof(ClusterStatusGpu);
            request.definition.features.push_back({capture, "cluster_status", take(ConfigValue::make(status_config))}
            ); // 16
            for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
            {
                connections.push_back({{1, name}, {12, name}});
                connections.push_back({{1, name}, {13, name}});
            }
            for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
            {
                connections.push_back({{0, name}, {13, name}});
            }
            for (auto name : {"grid", "offsets", "indices", "status"})
            {
                connections.push_back({{12, name}, {13, "cluster_" + std::string(name)}});
            }
            connections.push_back({{1, "views"}, {13, "shadow_views"}});
            connections.push_back({{1, "slices"}, {13, "shadow_slices"}});
            connections.push_back({{1, "light_ranges"}, {13, "shadow_ranges"}});
            connections.push_back({{3, "depth"}, {13, "shadow_depth"}});
            connections.push_back({{4, "hdr_color"}, {14, "reference"}});
            connections.push_back({{13, "hdr_color"}, {14, "candidate"}});
            connections.push_back({{14, "differences"}, {15, "color"}});
            connections.push_back({{12, "status"}, {16, "color"}});
            request.definition.outputs.push_back({{15, "pixels"}, {}});
            request.definition.outputs.push_back({{16, "pixels"}, {}});
        }
        MeshVisibilityFeatureConfig main_visibility;
        for (const auto& group : surface.groups)
        {
            main_visibility.groups.push_back(group.group);
        }
        main_visibility.candidate_capacity = surface.visible_capacity;
        main_visibility.view_parameters = layout;
        const auto visibility_index = static_cast<std::uint32_t>(request.definition.features.size());
        request.definition.features.push_back({visibility, "main_visibility", take(ConfigValue::make(main_visibility))}
        );
        for (unsigned g = 0; g < surface.groups.size(); ++g)
        {
            const auto source = "g" + std::to_string(g) + ".";
            const auto destination = "group" + std::to_string(g) + ".";
            for (auto [from, to] : {std::pair{"visible", "members"}, {"commands", "commands"}, {"counts", "counts"}})
            {
                connections.push_back({{visibility_index, source + from}, {0, destination + to}});
            }
        }
        auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {target}).target;
        auto attachment =
            call<Attach>(*client, commands, {scene, view, output, effect, parameterBytes(parameters)}).attachment;
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_target = call<CreateTarget>(*client, commands, {target}).target;
        auto second_parameters = parameters;
        second_parameters.shadow.position_near[0] = .2F;
        second_parameters.shadow.inverse_view_projection[12] = .2F;
        auto second = call<Attach>(
                          *client,
                          commands,
                          {scene, second_view, second_target, effect, parameterBytes(second_parameters)}
        )
                          .attachment;
        const auto compile_count = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        auto moving = parameters;
        moving.shadow.position_near[0] = -.2F;
        moving.shadow.inverse_view_projection[12] = -.2F;
        call<UpdateAttachmentInputs>(*client, commands, {attachment, parameterBytes(moving)});
        call<UpdateAttachmentInputs>(*client, commands, {attachment, parameterBytes(parameters)});
        CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compile_count);
        // A live attachment observes dense reordering; an old cached index must not shadow another light.
        call<RemoveDirectLight>(*client, commands, {lights[0], 2});
        if (mixed)
        {
            auto moved = std::get<PointLightGpu>(values[2]);
            moved.position_range[1] = .25F;
            const std::array update{DirectLightUpdate{lights[2], moved}};
            call<UpdateDirectLights>(*client, commands, {light_storage, 3, update});
            const auto moved_before =
                call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED);
            const auto moved_deadline = Clock::now() + 10s;
            while (call<QueryExecutionProgress>(*client, commands, attachment)
                       .progress.count(EExecutionOutcome::EXECUTED) < moved_before + 2)
            {
                CHECK(Clock::now() < moved_deadline);
                std::this_thread::yield();
            }
            const std::array restore{DirectLightUpdate{lights[2], values[2]}};
            call<UpdateDirectLights>(*client, commands, {light_storage, 4, restore});
        }
        const auto before =
            call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED);
        const auto second_before =
            call<QueryExecutionProgress>(*client, commands, second).progress.count(EExecutionOutcome::EXECUTED);
        const auto deadline = Clock::now() + 20s;
        while (call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) <
                   before + fif + 2 ||
               call<QueryExecutionProgress>(*client, commands, second).progress.count(EExecutionOutcome::EXECUTED) <
                   second_before + fif + 2)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        detach(*client, commands, attachment);
        detach(*client, commands, second);
        while (pixels.ready.load(std::memory_order_acquire) != 2 ||
               view_bytes.ready.load(std::memory_order_acquire) != 2 ||
               counts.ready.load(std::memory_order_acquire) != 2 ||
               surface_pixels.ready.load(std::memory_order_acquire) != 2 ||
               depth_pixels.ready.load(std::memory_order_acquire) != 2 ||
               indirect.ready.load(std::memory_order_acquire) != 2 ||
               (clustered && (differences.ready.load(std::memory_order_acquire) != 2 ||
                              cluster_status.ready.load(std::memory_order_acquire) != 2)))
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        if (clustered)
        {
            verifyEqualPixels(differences);
            bool overflow = false;
            for (const auto& frame : cluster_status.frames)
            {
                ClusterStatusGpu state;
                std::memcpy(&state, frame.data(), sizeof(state));
                CHECK((state.flags & ~1u) == 0);
                overflow |= (state.flags & 1u) != 0;
            }
            CHECK(overflow == (radius != 0 && (mixed || kind != EDirectLightingBuffer::DIRECTIONAL)));
        }
        unsigned rendered = 0;
        for (const auto& frame : surface_pixels.frames)
        {
            if (frame[32 * surface_pixels.pitch + 12 * 4 + 3] != std::byte{})
            {
                CHECK(std::to_integer<unsigned>(frame[32 * surface_pixels.pitch + 12 * 4]) == 178);
            }
        }
        for (const auto& frame : depth_pixels.frames)
        {
            float depth = 0;
            std::memcpy(&depth, frame.data() + 32 * depth_pixels.pitch + 12 * 4, 4);
            if (depth != 0)
            {
                CHECK(std::abs(depth - 1.9F / 2.9F) < 1e-5F);
            }
            if (masked && depth != 0)
            {
                float hole = 0, covered = 0;
                std::memcpy(&hole, frame.data() + 32 * depth_pixels.pitch + 36 * 4, 4);
                std::memcpy(&covered, frame.data() + 32 * depth_pixels.pitch + 28 * 4, 4);
                CHECK(std::abs(hole - 1.9F / 2.9F) < 1e-5F && std::abs(covered - 1.4F / 2.9F) < 1e-5F);
            }
        }
        for (const auto& frame : pixels.frames)
        {
            if (frame[32 * pixels.pitch + 48 * 4 + 3] == std::byte{})
            {
                continue;
            }
            ++rendered;
            const auto shadow_value = std::to_integer<unsigned>(frame[32 * pixels.pitch + 48 * 4]);
            const auto lit_value = std::to_integer<unsigned>(frame[32 * pixels.pitch + 12 * 4]);
            std::printf(
                "shadow kind=%u cull=%u radius=%u lit=%u occluded=%u\n",
                unsigned(kind),
                cull,
                radius,
                lit_value,
                shadow_value
            );
            if (cast)
            {
                CHECK(std::abs(int(lit_value) - int(reference_lit[mixed ? 3 : static_cast<unsigned>(kind)])) <= 2);
                if (clustered && mixed)
                {
                    CHECK(shadow_value > 0 && shadow_value + 10 < reference_occluded[3]);
                }
                else
                {
                    CHECK(masked ? shadow_value > 0 : shadow_value <= 3);
                }
                if (seam)
                {
                    CHECK(std::to_integer<unsigned>(frame[32 * pixels.pitch + 47 * 4]) <= 3);
                }
                if (masked)
                {
                    const auto covered = std::to_integer<unsigned>(frame[32 * pixels.pitch + 40 * 4]);
                    CHECK(covered <= 3);
                }
            }
            else
            {
                CHECK(lit_value > 0 && shadow_value > 0);
                reference_lit[mixed ? 3 : static_cast<unsigned>(kind)] = lit_value;
                reference_occluded[mixed ? 3 : static_cast<unsigned>(kind)] = shadow_value;
            }
        }
        CHECK(rendered);
        for (const auto& frame : view_bytes.frames)
        {
            MeshVisibilityView first;
            std::memcpy(&first, frame.data(), sizeof(first));
            if (!first.flags[0])
            {
                continue;
            }
            for (unsigned v = 0; v < shape.view_count; ++v)
            {
                MeshVisibilityView matrix;
                std::memcpy(&matrix, frame.data() + v * sizeof(matrix), sizeof(matrix));
                CHECK(matrix.flags[0] == 1);
                for (const auto& plane : matrix.planes)
                {
                    CHECK(
                        std::abs(
                            std::sqrt(
                                double(plane[0]) * plane[0] + double(plane[1]) * plane[1] + double(plane[2]) * plane[2]
                            ) -
                            1
                        ) < 1e-5
                    );
                }
                if (kind == EDirectLightingBuffer::POINT || (mixed && v >= 2 && v < 8))
                {
                    const auto face = mixed ? v - 2 : v;
                    const auto origin_x = std::get<PointLightGpu>(values[mixed ? 2 : 1]).position_range[0];
                    std::array<double, 3> point{origin_x, 0, 2};
                    point[face / 2] += face % 2 ? -selected.near_distance : selected.near_distance;
                    auto near_clip = project(matrix, point);
                    CHECK(
                        std::abs(near_clip[0]) < 1e-5 && std::abs(near_clip[1]) < 1e-5 && std::abs(near_clip[2]) < 1e-5
                    );
                    point = {origin_x, 0, 2};
                    point[face / 2] += face % 2 ? -6 : 6;
                    CHECK(std::abs(project(matrix, point)[2] - 1) < 1e-5);
                }
                else if (kind == EDirectLightingBuffer::DIRECTIONAL && (!mixed || v < 2))
                {
                    const double begin = v ? 1.75 : .1, end = v ? 3 : 1.75;
                    bool covered = false;
                    for (double origin : {0., .2})
                    {
                        bool contains = true;
                        for (double distance : {begin, end})
                        {
                            for (double x : {-1., 1.})
                            {
                                for (double y : {-1., 1.})
                                {
                                    const auto p = project(matrix, {x + origin, y, 2 - distance});
                                    contains &= std::abs(p[0]) <= 1.00001 && std::abs(p[1]) <= 1.00001 &&
                                                p[2] >= -1e-5 && p[2] <= 1.00001;
                                }
                            }
                        }
                        covered |= contains;
                    }
                    CHECK(covered);
                }
            }
        }
        bool have_count = false;
        for (const auto& frame : counts.frames)
        {
            for (unsigned v = 0; v < shape.view_count * kMeshWindingCount; ++v)
            {
                std::uint32_t count = 0;
                std::memcpy(&count, frame.data() + v * 4, 4);
                CHECK(count <= 1);
                have_count |= count != 0;
            }
        }
        CHECK(have_count == cast);
        bool observed_draw = false, observed_empty = false;
        for (const auto& frame : indirect.frames)
        {
            for (unsigned v = 0; v < shape.view_count; ++v)
            {
                unsigned instances = 0;
                bool initialized = false;
                for (unsigned winding = 0; winding < kMeshWindingCount; ++winding)
                {
                    std::array<std::uint32_t, 5> draw{};
                    std::memcpy(draw.data(), frame.data() + (v * kMeshWindingCount + winding) * 20, 20);
                    if (!draw[0])
                    {
                        continue;
                    }
                    initialized = true;
                    CHECK(draw[0] == 6 && draw[2] == 0 && draw[3] == 0 && draw[4] == 0);
                    instances += draw[1];
                }
                if (initialized)
                {
                    CHECK(instances <= (cast ? cull ? 1u : 2u : 0u));
                    observed_draw |= instances != 0;
                    observed_empty |= instances == 0;
                }
            }
        }
        CHECK(observed_draw == cast);
        if (!cast || (cull && (mixed || kind == EDirectLightingBuffer::POINT)))
        {
            CHECK(observed_empty);
        }
        call<RemoveEffect>(*client, commands, {effect});
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
        for (std::size_t i = 1; i < lights.size(); ++i)
        {
            call<RemoveDirectLight>(*client, commands, {lights[i], 5});
        }
    };
    for (auto kind : {EDirectLightingBuffer::DIRECTIONAL, EDirectLightingBuffer::POINT, EDirectLightingBuffer::SPOT})
    {
        run(kind, true, 0, false);
        run(kind, true, 0);
        run(kind, false, 1);
    }
    run(EDirectLightingBuffer::DIRECTIONAL, true, 0, false, true);
    if (clustered)
    {
        run(EDirectLightingBuffer::DIRECTIONAL, true, 0, true, true);
    }
    run(EDirectLightingBuffer::DIRECTIONAL, true, 1, true, true);
    run(EDirectLightingBuffer::POINT, true, 1, false, false, true);
    run(EDirectLightingBuffer::POINT, true, 1, true, false, true);
    masked = true;
    group.program = material(true);
    const auto masked_material =
        call<CreateMaterial>(*client, commands, {materials, group.program, material_values}).material;
    group.group.material_table_source = masked_material;
    descriptions[1].material = descriptions[2].material = masked_material;
    const std::array replacements{
        MeshInstanceUpdate{instance_ids[1], descriptions[1]},
        MeshInstanceUpdate{instance_ids[2], descriptions[2]}
    };
    call<UpdateMeshInstances>(*client, commands, {instances, 4, replacements});
    run(EDirectLightingBuffer::DIRECTIONAL, true, 0);
    call<RemoveScene>(*client, commands, {scene});
    call<ReleaseMaterial>(*client, commands, material_id);
    call<ReleaseMaterial>(*client, commands, masked_material);
    call<ReleaseMaterial>(*client, commands, unlit_material);
    call<ReleaseMesh>(*client, commands, mesh);
    call<ReleaseMesh>(*client, commands, caster_mesh);
    server->stop();
    thread.join();
    auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server.reset();
    runtime->requestStop();
    CHECK(runtime->join());
    std::printf(
        "shadow scene FIF=%u validation=%llu warnings=%llu\n",
        fif,
        stats.validation_errors,
        stats.validation_warnings
    );
}
