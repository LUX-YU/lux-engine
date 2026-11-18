#include "MaterialFixture.hpp"
#include "FullscreenVertex.hpp"
#include "GraphShadowSampler.hpp"
#include "SceneCapture.hpp"
#include "SceneComparison.hpp"
#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/resources/lighting/Commands.hpp>
#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/lighting/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/shadow/MeshShadowFeature.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/features/postprocess/TonemapFeature.hpp>

struct SamplerProbe
{
    using Config = void;
    static constexpr FeatureName name{"test.SamplerProbe"};
    using Storages = std::tuple<>;
    using Commands = std::tuple<>;

    using Ports = SamplerProbe;

    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        GraphImageDesc color{64, 64};
        color.extent.source = EImageExtentSource::TARGET;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        ports.resources.push_back(
            {"depths", EFeaturePort::OUTPUT, GraphImageDesc{64, 64, 1, 2, rdesc::ETextureFormat::D32_SFLOAT}}
        );
        return {};
    }

    static RenderResult<void>
    build(
        const void*,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        auto depth = context.resource("depths");
        // A culled sampler input leaves a hole before live inputs. Attach must not instantiate it.
        (void)context.graph.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS});
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
        GraphProgram shaders{kGraphShadowSampler};
        for (auto& shader : shaders.targets)
        {
            auto vertex = kFullscreenVertex.load(shader.target);
            shader.stages.insert(shader.stages.begin(), std::move(vertex.stages.front()));
            shader.interface.entry_points.push_back(std::move(vertex.interface.entry_points.front()));
        }
        RasterWork work;
        auto& fixed_work = work.fixed();
        fixed_work.interface.program = take(context.graph.addProgram(std::move(shaders)));
        fixed_work.interface.resources = {{"depths", {depth}}};
        auto ordinary = context.graph.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE});
        auto comparison =
            context.graph.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS_EQUAL});
        fixed_work.interface.samplers = {
            {"ordinary", {ordinary}},
            {"comparison", {comparison}, kGraphInvalidIndex, true}
        };
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


static void verifySamplers(RenderServer& server, const RenderServerConfig& config, const DeviceOptions& device)
{
    const auto probe = take(server.registerFeature<SamplerProbe>());
    take(server.registerResource<CaptureBuffer>());
    const auto capture = take(server.registerFeature<CaptureFeature>());

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server.run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, device).device;
    const auto catalog = call<ListCapabilities>(*client, commands, {session, std::string(kRasterSurfaceCapability)});
    CHECK(catalog.implementations.size() == 1);
    CHECK(catalog.implementations.front().unavailable.type == kStorageCapability);
    CapturedPixels pixels;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    CreateEffectRequest request{{target.format}};
    auto& recipe = request.definition;
    recipe.features.push_back({probe, "samplers", {}});
    recipe.features.push_back({capture, "pixels", take(ConfigValue::make(CaptureConfig{&pixels}))});
    recipe.connections.push_back({{0, "color"}, {1, "color"}});
    recipe.outputs = {{{0, "color"}, GraphOutputRequirements{target.format, 1}}, {{1, "pixels"}, {}}};
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto output = call<CreateTarget>(*client, commands, {session, target}).target;
    const auto attachment = startRenderJob(*client, commands, session, scene, view, output, effect);
    const auto deadline = Clock::now() + 10s;
    while (call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) < 4)
    {
        renderFrame(*client, commands, attachment);
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    stopAndRemoveJob(*client, commands, attachment);
    call<RemoveView>(*client, commands, {scene, view});
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

static rdesc::MaterialProgramSet material(rdesc::EShaderTarget target, bool masked = false, bool unlit = false)
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
    auto result = deriveFixtureMaterial(graph, purposes, target);
    CHECK(result);
    return std::move(result->material.programs);
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

using ShadowProbeConfiguration = std::tuple<
    GBufferFeatureConfig, ShadowMapFeatureConfig, MeshVisibilityFeatureConfig, MeshShadowFeatureConfig,
    DeferredLightingFeatureConfig, std::array<CaptureConfig, 8>, bool, bool
>;

static RenderResult<EffectDefinition> lowerShadowProbe(
    const EffectOutputDesc& output, const ShadowProbeConfiguration& configuration, std::span<const FeatureId> features
) noexcept
{
    const auto& [surface, projections, visibility_config, shadow_config, light_config, captures, clustered, radius] =
        configuration;
    const auto& target = output;
    const auto& shape = shadow_config.shape;
    const auto gbuffer = features[0], projection = features[1], visibility = features[2];
    const auto shadow = features[3], lighting = features[4], tonemap = features[5];
    const auto capture = features[6], cluster_feature = features[7], comparison = features[8];
    EffectDefinition request;
    request.features.push_back({gbuffer, "gbuffer", take(ConfigValue::make(surface))});
    request.features.push_back({projection, "projection", take(ConfigValue::make(projections))});
    request.features.push_back({visibility, "visibility", take(ConfigValue::make(visibility_config))});
    request.features.push_back({shadow, "shadow", take(ConfigValue::make(shadow_config))});
    request.features.push_back({lighting, "lighting", take(ConfigValue::make(light_config))});
    request.features.push_back(
        {tonemap, "tonemap", take(ConfigValue::make(TonemapFeatureConfig{target, EColorEncoding::LINEAR}))}
    );
    request.features.push_back({capture, "pixels", take(ConfigValue::make(captures[0]))});
    auto view_capture = captures[1];
    view_capture.bytes = shape.view_count * sizeof(MeshVisibilityView);
    request.features.push_back({capture, "views", take(ConfigValue::make(view_capture))});
    auto activity_capture = captures[2];
    activity_capture.bytes = shape.view_count * kMeshWindingCount * sizeof(nri::DrawIndexedDesc);
    request.features.push_back({capture, "activity_commands", take(ConfigValue::make(activity_capture))});
    request.features.push_back(
        {capture, "surface", take(ConfigValue::make(captures[3]))}
    );
    request.features.push_back(
        {capture, "depth", take(ConfigValue::make(captures[4]))}
    );
    auto indirect_capture = captures[5];
    indirect_capture.bytes = shape.view_count * kMeshWindingCount * 20;
    request.features.push_back({capture, "indirect", take(ConfigValue::make(indirect_capture))});
    auto& connections = request.connections;
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
    for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
    {
        connections.push_back({{2, port}, {3, port}});
    }
    connections.push_back({{3, "depth"}, {4, "shadow_depth"}});
    connections.push_back({{4, "hdr_color"}, {5, "hdr_color"}});
    connections.push_back({{5, "color"}, {6, "color"}});
    connections.push_back({{1, "views"}, {7, "color"}});
    connections.push_back({{2, "commands"}, {8, "color"}});
    connections.push_back({{0, "albedo_metallic"}, {9, "color"}});
    connections.push_back({{0, "depth"}, {10, "color"}});
    connections.push_back({{2, "commands"}, {11, "color"}});
    request.outputs.push_back({{5, "color"}, GraphOutputRequirements{target.format, 1}});
    for (unsigned capture_index : {6u, 7u, 8u, 9u, 10u, 11u})
    {
        request.outputs.push_back({{capture_index, "pixels"}, {}});
    }
    if (clustered)
    {
        ClusteredLightingFeatureConfig clusters;
        clusters.grid = {{7, 5, 9}, radius ? 1u : 65536u, EClusterDepth::LOGARITHMIC};
        clusters.external_lights = true;
        auto optimized = light_config;
        optimized.clusters =
            ClusterLightListShape{static_cast<std::uint32_t>(clusters.grid.count()), clusters.grid.index_capacity};
        optimized.external_lights = true;
        request.features.push_back({cluster_feature, "clusters", take(ConfigValue::make(clusters))}
        ); // 12
        request.features.push_back({lighting, "clustered_lighting", take(ConfigValue::make(optimized))}
        );                                                                  // 13
        request.features.push_back({comparison, "compare", {}}); // 14
        auto compare_config = captures[6];
        compare_config.bytes = 64 * 64 * 4;
        request.features.push_back(
            {capture, "difference_pixels", take(ConfigValue::make(compare_config))}
        ); // 15
        auto status_config = captures[7];
        status_config.bytes = sizeof(ClusterStatusGpu);
        request.features.push_back({capture, "cluster_status", take(ConfigValue::make(status_config))}
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
        request.outputs.push_back({{15, "pixels"}, {}});
        request.outputs.push_back({{16, "pixels"}, {}});
    }
    MeshVisibilityFeatureConfig main_visibility;
    main_visibility.camera_view = true;
    const auto visibility_index = static_cast<std::uint32_t>(request.features.size());
    request.features.push_back({visibility, "main_visibility", take(ConfigValue::make(main_visibility))}
    );
    for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
    {
        connections.push_back({{visibility_index, port}, {0, port}});
    }
    return request;
}

static void runPresets(
    ipc::IpcRequester& client,
    const CommandList& commands,
    DeviceSessionId session,
    SceneId scene,
    ResourceInstanceId lights,
    FeatureId capture,
    CapabilityImplementationId capture_probe,
    const TargetDesc& target,
    CameraInput camera,
    unsigned fif,
    std::uint32_t profile_frames
)
{
    const EffectOutputDesc output{target.format};
    const auto catalog = call<ListCapabilities>(client, commands, {session, {}, {}, output});
    const auto select = [&](std::string_view name) -> const CapabilityImplementationInfo&
    {
        const auto found = std::ranges::find(catalog.implementations, name,
            [](const auto& entry) { return entry.capability.type.name(); });
        CHECK(found != catalog.implementations.end() && !found->unavailable.type);
        return *found;
    };
    const auto& surface = select(kRasterSurfaceCapability);
    const auto& lighting = select(kDeferredLightingCapability);
    const auto unsupported = call<ListCapabilities>(client, commands, {
        session, std::string(kDeferredLightingCapability), {}, EffectOutputDesc{rdesc::ETextureFormat::RGBA16_SFLOAT}
    });
    CHECK(unsupported.implementations.size() == 1);
    CHECK(unsupported.implementations.front().unavailable.type == kRuntimeUnsupported);

    const std::array<VDirectLight, 2> sources{
        DirectionalLightGpu{{1, 1, 1, 0}, {.6F, 0, -.8F, 0}},
        PointLightGpu{{-1, 0, 2, 6}, {.2F, .2F, .2F, 0}, {1, 0, 0, 0}}
    };
    const std::array shadow_settings{LightShadowSettings{true}, LightShadowSettings{}};
    const auto ids = call<LightingCommands::CreateLights>(client, commands, {lights, {1, sources, shadow_settings}}).lights;
    // The preset uses CCW geometry. Flip this fixture's projection and its inverse together.
    camera.projection[0] *= -1;
    camera.inverse_view_projection[0] *= -1;
    std::array<std::vector<std::byte>, 4> results;
    for (std::uint32_t variant = profile_frames ? 3u : 0u; variant < results.size(); ++variant)
    {
        DeferredLightingConfiguration options;
        options.shadows = (variant & 1) != 0;
        options.clustered = (variant & 2) != 0;
        options.encoding = EColorEncoding::LINEAR;
        options.shadow = {64, 0, 3};
        options.clusters = {{7, 5, 9}, 4096, EClusterDepth::LINEAR};
        auto request = take(makeDeferredRenderEffect(output, surface, lighting, {}, options));
        CHECK(request.definition.features.size() == 4u + 3u * options.shadows + options.clustered);
        request.parameters.exposure = 0;
        CapturedPixels pixels;
        if (!profile_frames)
        {
            auto& recipe = request.definition;
            const auto copy = static_cast<std::uint32_t>(recipe.features.size());
            request.implementations.push_back({capture_probe, "capture"});
            recipe.features.push_back({capture, "capture/pixels", take(ConfigValue::make(CaptureConfig{&pixels}))});
            recipe.connections.push_back({recipe.outputs.front().source, {copy, "color"}});
            recipe.outputs.push_back({{copy, "pixels"}, {}});
        }

        const auto effect = call<CreateEffect>(client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(client, commands, {scene}).view;
        const auto image = call<CreateTarget>(client, commands, {session, target}).target;
        call<UpdateViewCamera>(client, commands, {{scene, view}, camera});
        const auto attached = startRenderJob(client, commands, session, scene, view, image, effect);
        const auto deadline = Clock::now() + 20s;
        while (call<QueryExecutionProgress>(client, commands, attached).progress.count(EExecutionOutcome::EXECUTED)
               < fif + 2)
        {
            renderFrame(client, commands, attached);
            CHECK(Clock::now() < deadline);
        }
        if (profile_frames)
        {
            constexpr std::uint32_t warmup_frames = 1500;
            for (std::uint32_t frame = 0; frame < warmup_frames; ++frame)
            {
                renderFrame(client, commands, attached);
            }
            const auto submissions = call<QueryRenderStats>(client, commands, {}).submissions;
            std::printf("PRESET_PROFILE_BEGIN frames=%u fif=%u\n", profile_frames, fif);
            const auto start = Clock::now();
            for (std::uint32_t frame = 0; frame < profile_frames; ++frame)
            {
                renderFrame(client, commands, attached);
            }
            const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
            const auto submitted = call<QueryRenderStats>(client, commands, {}).submissions - submissions;
            std::printf("PRESET_PROFILE_END seconds=%.6f submissions=%llu\n",
                seconds, static_cast<unsigned long long>(submitted));
        }
        stopAndRemoveJob(client, commands, attached);
        call<RemoveView>(client, commands, {scene, view});
        call<RemoveTarget>(client, commands, image);
        call<RemoveEffect>(client, commands, {effect});
        if (profile_frames) break;
        while (!pixels.ready.load(std::memory_order_acquire))
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(pixels.frames.size() == fif);
        CHECK(std::ranges::count(pixels.frames, pixels.frames.front()) == fif);
        results[variant] = std::move(pixels.frames.front());
    }
    for (auto id : ids) call<LightingCommands::ReleaseLight>(client, commands, {id, 2});
    if (profile_frames) return;
    CHECK(results[0] == results[2] && results[1] == results[3]);
    std::size_t shadowed = 0;
    for (std::size_t pixel = 0; pixel < results[0].size(); pixel += 4)
    {
        if (results[1][pixel] < results[0][pixel]) ++shadowed;
    }
    CHECK(shadowed > 0);
    std::printf("RENDER PRESETS PASS variants=4 FIF=%u shadowed_pixels=%zu\n", fif, shadowed);
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3u;
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.shadow", nativeValidationEnabled()};
    config.diagnostics = {nullptr, [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "%.*s:%u %.*s\n", int(diagnostic.file.size()), diagnostic.file.data(),
                diagnostic.line, int(diagnostic.message.size()), diagnostic.message.data());
    }};
    config.frames_in_flight = fif;
    // Two active views prepare shadow and lighting data in the same frame.
    config.uploads = TransferOptions{1024 * 1024, 4, 16 * 1024 * 1024};
    bool clustered = false;
    bool projection_revisions = false;
    bool presets = false;
    std::uint32_t profile_frames = 0;
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
        else if (option == "--projection-revisions")
        {
            projection_revisions = true;
        }
        else if (option == "--paged")
        {
            device.buffer_device_address = true;
        }
        else if (option == "--d3d12")
        {
            device.backend = EGraphicsBackend::D3D12;
        }
        else if (option == "--presets")
        {
            presets = true;
        }
        else if (option == "--profile-frames" && i + 1 < argc)
        {
            profile_frames = static_cast<std::uint32_t>(std::atoi(argv[++i]));
            CHECK(profile_frames > 0);
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
    auto server = take(RenderServer::create(config, **runtime));
    if (!device.texture_descriptors)
    {
        verifySamplers(*server, config, device);
        server.reset();
        runtime->reset(); // The Runtime owns its stop/join boundary.
        return 0;
    }
    const auto gbuffer = take(server->featureId("lux.render.feature.GBufferFeature"));
    const auto lighting = take(server->featureId("lux.render.feature.DeferredLightingFeature"));
    const auto tonemap = take(server->featureId("lux.render.feature.TonemapFeature"));
    const auto projection = take(server->featureId("lux.render.feature.ShadowMapFeature"));
    const auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    const auto shadow = take(server->featureId("lux.render.feature.MeshShadowFeature"));
    take(server->registerResource<CaptureBuffer>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto capture_probe = presets ? registerCaptureProbe(*server, capture) : CapabilityImplementationId{};
    const auto cluster_feature = take(server->featureId("lux.render.feature.ClusteredLightingFeature"));
    const auto comparison = take(server->registerFeature<SceneComparison>());
    const auto shadow_probe = registerRenderProbe<ShadowProbeConfiguration>(
        *server, "test.shadow", {gbuffer, projection, visibility, shadow, lighting, tonemap, capture,
            cluster_feature, comparison},
        probePorts({"color"}, {"pixels", "views", "activity_commands", "surface", "depth", "indirect"}), lowerShadowProbe
    );
    const auto clustered_probe = registerRenderProbe<ShadowProbeConfiguration>(
        *server, "test.clustered_shadow", {gbuffer, projection, visibility, shadow, lighting, tonemap, capture,
            cluster_feature, comparison},
        probePorts(
            {"color"},
            {"pixels", "views", "activity_commands", "surface", "depth", "indirect", "differences", "cluster_status"}
        ), lowerShadowProbe
    );

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, device).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto meshes = call<GetOrCreateResource>(
        *client, commands, {session, DeviceScope{}, kMeshResourceName}
    ).resource_id;
    const auto materials = call<GetOrCreateResource>(
        *client, commands, {session, DeviceScope{}, kMaterialResourceName}
    ).resource_id;
    const auto instances = call<GetOrCreateResource>(*client, commands, {session, scene, kInstanceResourceName}).resource_id;
    const auto light_storage = call<GetOrCreateResource>(*client, commands, {session, scene, kLightResourceName}).resource_id;
    const auto scene_inputs = installInputs(*client, commands, session, scene);
    const auto instance_updates = scene_inputs.instances;
    const auto light_updates = scene_inputs.lights;

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
    rdesc::Mesh mesh_data;
    mesh_data.indices.assign(indices.begin(), indices.end());
    mesh_data.vertices.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i)
    {
        const auto& source_vertex = vertices[i];
        auto& vertex = mesh_data.vertices[i];
        vertex.position = Eigen::Map<const Eigen::Vector3f>(source_vertex.position.data());
        vertex.normal = Eigen::Map<const Eigen::Vector3f>(source_vertex.normal.data());
        vertex.uv = Eigen::Map<const Eigen::Vector2f>(source_vertex.uv.data());
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
    }
    mesh_data.bounds = lux::math::AABB{{-1, -1, 0}, {1, 1, 0}};
    auto geometry = take(describeIndexedMesh(mesh_data));
    auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
    auto caster_mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto program = material(shader_target);
    MaterialParameters material_values{{.7F, .7F, .7F, 1}};
    const auto registered = registerMaterialProgram(*client, commands, materials, std::move(program));
    auto material_id = createMaterialInstance(*client, commands, registered, material_values);
    auto unlit_program = material(shader_target, false, true);
    const auto registered_unlit = registerMaterialProgram(*client, commands, materials, std::move(unlit_program));
    auto unlit_material = createMaterialInstance(*client, commands, registered_unlit, {{.2F, .1F, .05F, 1}});
    std::array<MeshInstanceData, 4> descriptions{
        {{mesh, material_id}, {caster_mesh, material_id}, {caster_mesh, material_id}, {mesh, unlit_material}}
    };
    for (auto& value : descriptions) value.render_enabled = true;
    descriptions[1].transform[0] = -.25F; // Mirrored caster with nonuniform scale.
    descriptions[1].transform[5] = .2F;
    descriptions[1].transform[14] = .5F;
    descriptions[2].transform[12] = 100;
    descriptions[3].transform[0] = descriptions[3].transform[5] = .08F;
    descriptions[3].transform[12] = descriptions[3].transform[13] = .8F;
    descriptions[3].transform[14] = .2F;
    // The readback probes the caster group's streams. Dynamic grouping follows first membership,
    // so create that group first; the receiver and marker remain ordinary color-only instances.
    const std::array ordered_descriptions{descriptions[1], descriptions[2], descriptions[0], descriptions[3]};
    auto instance_ids = call<InstanceCommands::CreateInstances>(
                            *client,
                            commands,
                            {instances, ordered_descriptions}
    )
                            .instances;
    std::rotate(instance_ids.begin(), instance_ids.begin() + 2, instance_ids.begin() + 3);
    // Positive-height viewport: this unmirrored quad is clockwise in framebuffer coordinates.
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    CameraInput camera;
    camera.view[10] = -1;
    camera.view[14] = 2;
    camera.projection[10] = 1 / 2.9F;
    camera.projection[14] = -.1F / 2.9F;
    camera.inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2.9F, 0, 0, 0, 1.9F, 1};
    camera.position = {0, 0, 2};
    camera.far_distance = 3;
    camera.background = {0, 0, 0, 1};
    if (presets || profile_frames)
    {
        runPresets(*client, commands, session, scene, light_storage, capture, capture_probe, target, camera, fif,
                   profile_frames);
    }
    std::array<unsigned, 4> reference_lit{}, reference_occluded{};
    bool masked = false;

    const auto run = [&](EDirectLightingBuffer kind,
                         bool cull,
                         std::uint32_t radius,
                         bool cast = true,
                         bool mixed = false,
                         bool seam = false,
                         bool instance_shadow = true)
    {
        std::array<MeshInstanceUpdate, 4> permissions;
        for (std::size_t i = 0; i < descriptions.size(); ++i)
        {
            descriptions[i].cast_shadow = instance_shadow && (i == 1 || i == 2);
            permissions[i] = {instance_ids[i], descriptions[i]};
        }
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{permissions});
        const bool expected_shadow = cast && instance_shadow;
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
        auto lights = call<LightingCommands::CreateLights>(
            *client, commands, {light_storage, {1, values}}
        ).lights;
        const ShadowMapShape shape{
            64,
            mixed                                        ? 9u
            : kind == EDirectLightingBuffer::DIRECTIONAL ? 2u
            : kind == EDirectLightingBuffer::POINT       ? 6u
                                                         : 1u
        };
        ShadowMapFeatureConfig projections;
        projections.shape = shape;
        projections.pcf_radius = radius;
        std::vector<DirectLightUpdate> shadow_updates;
        const auto add_shadow = [&](std::size_t index, bool directional)
        {
            LightShadowSettings settings;
            settings.enabled = cast;
            settings.depth_bias_constant = radius ? 1.F : 0.F;
            settings.depth_bias_slope = radius ? 1.F : 0.F;
            settings.cascade_count = directional ? 2u : 1u;
            settings.cascade_splits = {(1.75F - .1F) / (3.F - .1F), 1.F};
            shadow_updates.push_back({lights[index], values[index], settings});
        };
        add_shadow(1, kind == EDirectLightingBuffer::DIRECTIONAL);
        if (mixed)
        {
            add_shadow(2, false);
            add_shadow(3, false);
        }
        projections.distance = 3;
        updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{shadow_updates});
        MeshVisibilityFeatureConfig visibility_config;
        visibility_config.view_count = shape.view_count;
        visibility_config.frustum_culling = cull;
        visibility_config.shadow_pass = true;
        MeshShadowFeatureConfig shadow_config{shape};
        GBufferFeatureConfig surface{EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}};
        surface.front_counter_clockwise = false;
        DeferredLightingFeatureConfig light_config;
        light_config.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        light_config.extent.extent.source = EImageExtentSource::TARGET;
        light_config.shadows = shape;
        light_config.external_lights = true;
        CapturedPixels pixels, view_bytes, activity_commands, surface_pixels, depth_pixels;
        CapturedPixels indirect, differences, cluster_status;
        auto request = probeRequest(
            target, clustered ? clustered_probe : shadow_probe,
            ShadowProbeConfiguration{surface, projections, visibility_config, shadow_config, light_config,
                {CaptureConfig{&pixels}, CaptureConfig{&view_bytes}, CaptureConfig{&activity_commands},
                 CaptureConfig{&surface_pixels}, CaptureConfig{&depth_pixels, rdesc::ETextureFormat::D32_SFLOAT},
                 CaptureConfig{&indirect}, CaptureConfig{&differences}, CaptureConfig{&cluster_status}},
                clustered, radius != 0},
            clustered ? std::initializer_list<std::string_view>{
                "color", "pixels", "views", "activity_commands", "surface", "depth", "indirect",
                "differences", "cluster_status"
            } : std::initializer_list<std::string_view>{
                "color", "pixels", "views", "activity_commands", "surface", "depth", "indirect"
            }
        );
        request.parameters.exposure = 0;
        auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {session, target}).target;
        call<UpdateViewCamera>(*client, commands, {{scene, view}, camera});
        const auto attachment = startRenderJob(*client, commands, session, scene, view, output, effect);
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_target = call<CreateTarget>(*client, commands, {session, target}).target;
        auto second_camera = camera;
        second_camera.projection[0] = 1.02F;
        second_camera.inverse_view_projection[0] = 1.F / second_camera.projection[0];
        call<UpdateViewCamera>(*client, commands, {{scene, second_view}, second_camera});
        const auto second = startRenderJob(*client, commands, session, scene, second_view, second_target, effect);
        const auto compile_count = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        auto moving = camera;
        moving.position[0] = -.2F;
        moving.view[12] = .2F;
        moving.inverse_view_projection[12] = -.2F;
        call<UpdateViewCamera>(*client, commands, {{scene, view}, moving});
        call<UpdateViewCamera>(*client, commands, {{scene, view}, camera});
        CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compile_count);
        // A live attachment observes dense reordering; an old cached index must not shadow another light.
        if (!projection_revisions || !cast)
        {
            call<LightingCommands::ReleaseLight>(*client, commands, {lights[0], 2});
        }
        if (mixed)
        {
            auto moved = std::get<PointLightGpu>(values[2]);
            moved.position_range[1] = .25F;
            const auto point_shadows = shadow_updates[1].shadows;
            const std::array update{DirectLightUpdate{lights[2], moved, point_shadows}};
            updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{update});
            const auto moved_before =
                call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED);
            const auto moved_deadline = Clock::now() + 10s;
            while (call<QueryExecutionProgress>(*client, commands, attachment)
                       .progress.count(EExecutionOutcome::EXECUTED) < moved_before + 2)
            {
                renderFrame(*client, commands, attachment);
                CHECK(Clock::now() < moved_deadline);
                std::this_thread::yield();
            }
            const std::array restore{DirectLightUpdate{lights[2], values[2], point_shadows}};
            updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{restore});
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
            renderFrame(*client, commands, attachment);
            renderFrame(*client, commands, second);
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        if (projection_revisions && cast)
        {
            CHECK(call_capture_core); // Use the existing native observation fixture, never production counters.
            auto current = shadow_updates.front();
            const auto original = current;
            const auto copy_bytes = []() { return call_capture_counts[44].load(std::memory_order_relaxed); };
            std::puts("PROJECTION BEGIN dense-removal expected=2");
            std::fflush(stdout);
            call<LightingCommands::ReleaseLight>(*client, commands, {lights[0], 2});
            for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, attachment);
            std::puts("PROJECTION END dense-removal");
            std::fflush(stdout);
            const auto before_stable = copy_bytes();
            for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, attachment);
            const auto stable_bytes = copy_bytes() - before_stable;
            const auto check_update = [&](const char* name, DirectLightUpdate next, bool projection_changes)
            {
                const auto light_bytes = std::visit([&](const auto& value) -> std::uint64_t
                {
                    const auto& previous = std::get<std::decay_t<decltype(value)>>(current.value);
                    return std::memcmp(&previous, &value, sizeof(value)) == 0 ? 0 : sizeof(value) * fif;
                }, next.value);
                std::printf("PROJECTION BEGIN %s expected=%u\n", name, projection_changes ? 2u : 0u);
                std::fflush(stdout); // Correlate existing projectShadows debugger hits with this completed input.
                const auto before = copy_bytes();
                updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{&next, 1});
                for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, attachment);
                const auto copied = copy_bytes() - before;
                std::printf(
                    "PROJECTION END %s copy_bytes=%llu stable=%llu light=%llu\n",
                    name,
                    static_cast<unsigned long long>(copied),
                    static_cast<unsigned long long>(stable_bytes),
                    static_cast<unsigned long long>(light_bytes)
                );
                std::fflush(stdout);
                if (projection_changes) CHECK(copied > stable_bytes + light_bytes);
                else CHECK(copied == stable_bytes + light_bytes);
                current = next;
            };
            check_update("unchanged", current, false);
            auto changed = current;
            std::visit([](auto& value) { value.radiance[0] *= .5F; }, changed.value);
            check_update("radiance", changed, false);
            check_update("restore-radiance", original, false);
            if (kind != EDirectLightingBuffer::POINT)
            {
                changed = current;
                std::visit([](auto& value)
                {
                    if constexpr (std::same_as<std::decay_t<decltype(value)>, DirectionalLightGpu>)
                        value.direction = {0, 0, -1, 0};
                    else if constexpr (std::same_as<std::decay_t<decltype(value)>, SpotLightGpu>)
                        value.direction_outer = {0, 0, -1, .7F};
                }, changed.value);
                check_update("direction", changed, true);
                check_update("restore-direction", original, true);
            }
            if (kind != EDirectLightingBuffer::DIRECTIONAL)
            {
                for (auto field : {0u, 3u})
                {
                    changed = current;
                    std::visit([field](auto& value)
                    {
                        if constexpr (!std::same_as<std::decay_t<decltype(value)>, DirectionalLightGpu>)
                            value.position_range[field] += .25F;
                    }, changed.value);
                    check_update(field == 0 ? "position" : "range", changed, true);
                    check_update("restore-position-range", original, true);
                }
            }
            if (kind == EDirectLightingBuffer::SPOT)
            {
                changed = current;
                std::get<SpotLightGpu>(changed.value).direction_outer[3] = .65F;
                check_update("outer-angle", changed, true);
                check_update("restore-angle", original, true);
            }
            for (auto member : {&LightShadowSettings::bias, &LightShadowSettings::normal_bias,
                                &LightShadowSettings::depth_bias_constant, &LightShadowSettings::depth_bias_slope})
            {
                changed = current;
                changed.shadows.*member += .01F;
                check_update("bias", changed, true);
                check_update("restore-bias", original, true);
            }
            changed = current;
            changed.shadows.enabled = false;
            check_update("disabled", changed, true);
            check_update("enabled", original, true);
            if (kind == EDirectLightingBuffer::DIRECTIONAL)
            {
                changed = current;
                changed.shadows.cascade_count = 1;
                changed.shadows.cascade_splits[0] = 1;
                check_update("cascade-count", changed, true);
                check_update("restore-cascade-count", original, true);
                changed = current;
                changed.shadows.cascade_splits[0] = .4F;
                check_update("cascade-split", changed, true);
                check_update("restore-cascade-split", original, true);
            }
            for (bool moved_camera : {true, false})
            {
                const auto* name = moved_camera ? "camera" : "restore-camera";
                std::printf("PROJECTION BEGIN %s expected=1\n", name);
                std::fflush(stdout);
                const auto before = copy_bytes();
                call<UpdateViewCamera>(
                    *client, commands, {{scene, view}, moved_camera ? moving : camera}
                );
                for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, attachment);
                CHECK(copy_bytes() > before + stable_bytes);
                std::printf("PROJECTION END %s\n", name);
                std::fflush(stdout);
            }
        }
        stopAndRemoveJob(*client, commands, attachment);
        stopAndRemoveJob(*client, commands, second);
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
        while (pixels.ready.load(std::memory_order_acquire) != 2 ||
               view_bytes.ready.load(std::memory_order_acquire) != 2 ||
               activity_commands.ready.load(std::memory_order_acquire) != 2 ||
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
            if (expected_shadow)
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
                    point[face / 2] += face % 2 ? -0.05 : 0.05;
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
                    for (const auto& source_camera : {camera, second_camera})
                    {
                        bool contains = true;
                        for (double distance : {begin, end})
                        {
                            for (double x : {-1., 1.})
                            {
                                for (double y : {-1., 1.})
                                {
                                    const auto world_x = x * source_camera.inverse_view_projection[0] +
                                        source_camera.inverse_view_projection[12];
                                    const auto p = project(matrix, {world_x, y, 2 - distance});
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
        for (const auto& frame : activity_commands.frames)
        {
            for (unsigned v = 0; v < shape.view_count * kMeshWindingCount; ++v)
            {
                std::uint32_t count = 0;
                std::memcpy(&count, frame.data() + v * sizeof(nri::DrawIndexedDesc) + 4, 4);
                CHECK(count <= 2);
                have_count |= count != 0;
            }
        }
        CHECK(have_count == expected_shadow);
        bool observed_draw = false, observed_empty = false;
        std::optional<std::array<std::uint32_t, 2>> caster_address;
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
                    CHECK(draw[0] == 6);
                    const std::array address{draw[2], draw[3]};
                    if (caster_address) CHECK(address == *caster_address);
                    else caster_address = address;
                    const auto stream = v * kMeshWindingCount + winding;
                    const auto base_instance = call_capture_first_instance ? stream * meshVisibleStride(2) / 4 : 0;
                    CHECK(draw[4] == base_instance);
                    instances += draw[1];
                }
                if (initialized)
                {
                    CHECK(instances <= (expected_shadow ? cull ? 1u : 2u : 0u));
                    observed_draw |= instances != 0;
                    observed_empty |= instances == 0;
                }
            }
        }
        CHECK(observed_draw == expected_shadow);
        if (!expected_shadow || (cull && (mixed || kind == EDirectLightingBuffer::POINT)))
        {
            CHECK(observed_empty);
        }
        call<RemoveEffect>(*client, commands, {effect});
        for (std::size_t i = 1; i < lights.size(); ++i)
        {
            call<LightingCommands::ReleaseLight>(
                *client,
                commands,
                {lights[i], 5}
            );
        }
    };
    if (!profile_frames)
    {
        for (auto kind : {EDirectLightingBuffer::DIRECTIONAL, EDirectLightingBuffer::POINT, EDirectLightingBuffer::SPOT})
        {
            run(kind, true, 0, false);
            run(kind, true, 0);
            if (!projection_revisions) run(kind, false, 1);
        }
    }
    MaterialId masked_material;
    MaterialProgramId registered_masked;
    if (!projection_revisions && !profile_frames)
    {
        // Keep the light and color rendering enabled; only per-instance shadow permission is disabled.
        run(EDirectLightingBuffer::DIRECTIONAL, true, 0, true, false, false, false);
        run(EDirectLightingBuffer::DIRECTIONAL, false, 0, true, false, false, false);
        run(EDirectLightingBuffer::DIRECTIONAL, true, 0, false, true);
        if (clustered)
        {
            run(EDirectLightingBuffer::DIRECTIONAL, true, 0, true, true);
        }
        run(EDirectLightingBuffer::DIRECTIONAL, true, 1, true, true);
        run(EDirectLightingBuffer::POINT, true, 1, false, false, true);
        run(EDirectLightingBuffer::POINT, true, 1, true, false, true);
        masked = true;
        registered_masked = registerMaterialProgram(*client, commands, materials, material(shader_target, true));
        masked_material = createMaterialInstance(*client, commands, registered_masked, material_values);
        descriptions[1].material = descriptions[2].material = masked_material;
        const std::array replacements{
            MeshInstanceUpdate{instance_ids[1], descriptions[1]},
            MeshInstanceUpdate{instance_ids[2], descriptions[2]}
        };
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{replacements});
        run(EDirectLightingBuffer::DIRECTIONAL, true, 0);
    }
    call<RemoveScene>(*client, commands, {scene});
    call<MaterialCommands::ReleaseInstance>(
        *client,
        commands,
        {material_id}
    );
    if (!projection_revisions && !profile_frames)
    {
        call<MaterialCommands::ReleaseInstance>(*client, commands, {masked_material});
        call<MaterialCommands::UnregisterProgram>(*client, commands, {registered_masked});
    }
    call<MaterialCommands::ReleaseInstance>(
        *client,
        commands,
        {unlit_material}
    );
    call<MaterialCommands::UnregisterProgram>(*client, commands, {registered});
    call<MaterialCommands::UnregisterProgram>(*client, commands, {registered_unlit});
    call<MeshCommands::ReleaseMesh>(
        *client,
        commands,
        {mesh}
    );
    call<MeshCommands::ReleaseMesh>(
        *client,
        commands,
        {caster_mesh}
    );
    server->stop();
    thread.join();
    endCallCapture();
    auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server.reset();
    runtime->reset(); // Runtime RAII settles execution after the RenderServer has been destroyed.
    std::printf(
        "shadow scene FIF=%u validation=%llu warnings=%llu\n",
        fif,
        stats.validation_errors,
        stats.validation_warnings
    );
}
