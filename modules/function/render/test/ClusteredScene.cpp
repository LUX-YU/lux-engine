#include "MaterialFixture.hpp"
#include "ClusterScan.hpp"
#include "SceneComparison.hpp"
#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/resources/lighting/Commands.hpp>
#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/lighting/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>

struct ClusterCaptureConfig
{
    ClusterGrid grid;
    CapturedPixels* pixels = nullptr;
    bool operator==(const ClusterCaptureConfig&) const noexcept = default;
};

static constexpr std::array<std::string_view, 6> outputs{"grid", "bounds", "counts", "offsets", "indices", "status"};

static auto sizes(const ClusterGrid& grid)
{
    const auto n = static_cast<std::uint32_t>(grid.count());
    return std::array<std::uint32_t, 6>{128, n * 32, n * 4, (n + 1) * 4, grid.index_capacity * 4, 16};
}

struct ClusterCapture
{
    using Config = ClusterCaptureConfig;
    static constexpr FeatureName name{"test.ClusterCapture"};
    using Storages = std::tuple<CaptureBuffer>;
    using Commands = std::tuple<>;

    using Ports = ClusterCapture;

    static RenderResult<void> describe(const void* data, std::string_view key, FeaturePorts& ports) noexcept
    {
        const auto& config = *static_cast<const ClusterCaptureConfig*>(data);
        std::uint32_t bytes = 0;
        const auto lengths = sizes(config.grid);
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            ports.resources.push_back({std::string(outputs[i]), EFeaturePort::INPUT, GraphBufferDesc{lengths[i]}});
            bytes += lengths[i];
        }
        CaptureConfig capture{config.pixels};
        capture.bytes = bytes;
        ports.storage.view.push_back(
            {"pixels",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{bytes},
             EGraphOrigin::IMPORTED,
             {},
             ResourceRequirement{ResourceName(CaptureDescriptor::descriptor.type.name())}}
        );
        return {};
    }

    static RenderResult<void>
    build(
        const void* data,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        const auto& config = *static_cast<const ClusterCaptureConfig*>(data);
        const auto lengths = sizes(config.grid);
        CaptureConfig capture{config.pixels};
        for (auto bytes : lengths) capture.bytes += bytes;
        context.require(StorageArgument::make(context.resource("pixels").resource, capture));
        TransferWork work;
        std::uint64_t offset = 0;
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            work.operations.push_back(CopyBuffer{
                {context.resource(outputs[i])},
                {context.resource("pixels"), GraphBufferRange{offset, lengths[i]}, EGraphAccess::WRITE}
            });
            offset += lengths[i];
        }
        auto copied =
            context.graph.addTransferPass(featureKey(FeatureName{"test.ClusterCapture"}, key, "copy"), std::move(work));
        if (!copied)
        {
            return lux::cxx::unexpected(copied.error());
        }
        return context.publish("pixels", copied->written[0]);
    }
};

struct ScanProbe
{
    using Config = void;
    static constexpr FeatureName name{"test.Scan"};
    using Storages = std::tuple<>;
    using Commands = std::tuple<>;

    using Ports = ScanProbe;

    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        for (auto [name, size] : {std::pair{"counts", 129u * 4}, {"offsets", 130u * 4}, {"status", 16u}})
        {
            ports.resources.push_back({name, EFeaturePort::OUTPUT, GraphBufferDesc{size}});
        }
        return {};
    }

    static RenderResult<void>
    build(
        const void*,
        std::string_view key,
        FeatureBuildContext& context
    ) noexcept
    {
        auto initialized = take(context.graph.addTransferPass(
            featureKey(FeatureName{"test.Scan"}, key, "initialize"),
            TransferWork{
                {FillBuffer{{context.resource("counts")}, UINT32_MAX}, FillBuffer{{context.resource("status")}, 0}}
            }
        ));
        ComputeWork work;
        work.interface.program = take(context.graph.addProgram(GraphProgram{kClusterScan}));
        work.interface.resources = {
            {"counts", {initialized.written[0]}},
            {"offsets", {context.resource("offsets"), {}, EGraphAccess::WRITE}},
            {"status", {initialized.written[1], {}, EGraphAccess::READ_WRITE}}
        };
        const std::array<std::uint32_t, 2> constants{129, 32};
        const auto bytes = std::as_bytes(std::span(constants));
        work.interface.parameters.values.assign(bytes.begin(), bytes.end());
        work.dispatch = DispatchArguments{{1, 1, 1}};
        auto scanned =
            take(context.graph.addComputePass(featureKey(FeatureName{"test.Scan"}, key, "scan"), std::move(work)));
        CHECK(context.publish("offsets", scanned.written[0]));
        return context.publish("status", scanned.written[1]);
    }
};



static CameraInput parameters(bool perspective)
{
    CameraInput result;
    result.view[10] = -1;
    result.view[14] = 2;
    const float near_z = .1F, far_z = 8, q = far_z / (far_z - near_z), r = near_z * q;
    if (perspective)
    {
        result.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, q, 1, 0, 0, -r, 0};
        result.inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2 / r, -1 / r, 0, 0, 2 * q / r - 1, q / r};
    }
    else
    {
        result.projection[10] = 1 / (far_z - near_z);
        result.projection[14] = -near_z / (far_z - near_z);
        result.inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, near_z - far_z, 0, 0, 0, 2 - near_z, 1};
    }
    result.position = {0, 0, 2};
    result.near_distance = near_z;
    result.far_distance = far_z;
    result.background = {0, 0, 0, 1};
    return result;
}

template <class T> static T load(const std::vector<std::byte>& data, std::size_t at)
{
    T result;
    std::memcpy(&result, data.data() + at, sizeof(T));
    return result;
}

static void verifyLists(
    const CapturedPixels& pixels,
    const ClusterGrid& config,
    std::span<const VDirectLight> lights,
    bool perspective
)
{
    const auto lengths = sizes(config);
    std::array<std::size_t, 6> at{};
    for (std::size_t i = 1; i < at.size(); ++i)
    {
        at[i] = at[i - 1] + lengths[i - 1];
    }
    unsigned observed = 0;
    for (const auto& data : pixels.frames)
    {
        const auto grid = load<ClusterGridGpu>(data, 0);
        if (!grid.cells[0])
        {
            continue;
        }
        CHECK(grid.limits[0] == config.count() && grid.limits[1] == config.index_capacity);
        const auto status = load<ClusterStatusGpu>(data, at[5]);
        std::uint32_t total = 0;
        for (std::uint32_t cell = 0; cell < config.count(); ++cell)
        {
            const auto bounds = load<ClusterBoundsGpu>(data, at[1] + cell * 32);
            const std::array dimensions{
                cell % config.cells[0],
                (cell / config.cells[0]) % config.cells[1],
                cell / (config.cells[0] * config.cells[1])
            };
            // Independent camera-space corner oracle; no production projection helper is called.
            for (unsigned z = 0; z < 2; ++z)
            {
                const double t = double(dimensions[2] + z) / config.cells[2];
                const double depth = config.depth == EClusterDepth::LINEAR ? .1 + 7.9 * t : .1 * std::pow(80., t);
                for (unsigned y = 0; y < 2; ++y)
                {
                    for (unsigned x = 0; x < 2; ++x)
                    {
                        const double scale = perspective ? depth : 1;
                        const std::array<double, 3> point{
                            (2. * (dimensions[0] + x) / config.cells[0] - 1) * scale + grid.position_near[0],
                            (2. * (dimensions[1] + y) / config.cells[1] - 1) * scale,
                            2 - depth
                        };
                        for (unsigned axis = 0; axis < 3; ++axis)
                        {
                            CHECK(
                                point[axis] >= bounds.minimum[axis] - 1e-5 && point[axis] <= bounds.maximum[axis] + 1e-5
                            );
                        }
                    }
                }
            }
            std::vector<std::uint32_t> expected;
            for (unsigned kind = 1; kind <= 2; ++kind)
            {
                std::uint32_t dense = 0;
                for (const auto& value : lights)
                {
                    if (value.index() != kind)
                    {
                        continue;
                    }
                    const auto sphere = kind == 1 ? std::get<PointLightGpu>(value).position_range
                                                  : std::get<SpotLightGpu>(value).position_range;
                    double distance = 0;
                    for (unsigned axis = 0; axis < 3; ++axis)
                    {
                        const double delta = std::max(
                            {0.,
                             double(bounds.minimum[axis]) - sphere[axis],
                             double(sphere[axis]) - bounds.maximum[axis]}
                        );
                        distance += delta * delta;
                    }
                    if (distance <= double(sphere[3]) * sphere[3])
                    {
                        expected.push_back(dense | (kind == 2 ? 0x80000000u : 0));
                    }
                    ++dense;
                }
            }
            CHECK(load<std::uint32_t>(data, at[2] + cell * 4) == expected.size());
            CHECK(load<std::uint32_t>(data, at[3] + cell * 4) == total);
            if (!status.flags)
            {
                for (std::size_t i = 0; i < expected.size(); ++i)
                {
                    CHECK(load<std::uint32_t>(data, at[4] + (total + i) * 4) == expected[i]);
                }
            }
            total += static_cast<std::uint32_t>(expected.size());
        }
        CHECK(load<std::uint32_t>(data, at[3] + config.count() * 4) == total);
        CHECK(status.required_indices == total && status.flags == (total > config.index_capacity ? 1u : 0u));
        std::printf(
            "cluster cells=%llu indices=%u capacity=%u fallback=%u\n",
            config.count(),
            total,
            config.index_capacity,
            status.flags
        );
        ++observed;
    }
    CHECK(observed);
}

using ClusterProbeConfiguration = std::tuple<
    GBufferFeatureConfig, DeferredLightingFeatureConfig, ClusteredLightingFeatureConfig,
    ClusterCaptureConfig, CaptureConfig
>;

static RenderResult<EffectDefinition> lowerClusterProbe(
    const EffectOutputDesc& output, const ClusterProbeConfiguration& configuration, std::span<const FeatureId> features
) noexcept
{
    const auto& [surface, reference, clustered, list_capture, capture_diff] =
        configuration;
    const auto& grid = clustered.grid;
    auto optimized = reference;
    optimized.clusters = ClusterLightListShape{static_cast<std::uint32_t>(grid.count()), grid.index_capacity};
    optimized.external_lights = true;
    const auto gbuffer = features[0], lighting = features[1], cluster = features[2];
    const auto compare = features[3], capture = features[4], cluster_capture = features[5];
    const auto visibility = features[6];
    EffectDefinition request;
    request.features.push_back({gbuffer, "surface", take(ConfigValue::make(surface))});
    request.features.push_back({lighting, "reference", take(ConfigValue::make(reference))});
    request.features.push_back({cluster, "cluster", take(ConfigValue::make(clustered))});
    request.features.push_back({lighting, "optimized", take(ConfigValue::make(optimized))});
    request.features.push_back({compare, "compare", {}});
    request.features.push_back({capture, "differences", take(ConfigValue::make(capture_diff))});
    request.features.push_back(
        {cluster_capture, "lists", take(ConfigValue::make(list_capture))}
    );
    auto& connections = request.connections;
    for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
    {
        connections.push_back({{0, name}, {1, name}});
        connections.push_back({{0, name}, {3, name}});
    }
    for (auto name : {"directional_lights", "point_lights", "spot_lights", "light_counts"})
    {
        connections.push_back({{2, name}, {3, name}});
    }
    for (auto name : {"grid", "offsets", "indices", "status"})
    {
        connections.push_back({{2, name}, {3, "cluster_" + std::string(name)}});
    }
    for (auto name : outputs)
    {
        connections.push_back({{2, std::string(name)}, {6, std::string(name)}});
    }
    connections.push_back({{1, "hdr_color"}, {4, "reference"}});
    connections.push_back({{3, "hdr_color"}, {4, "candidate"}});
    connections.push_back({{4, "differences"}, {5, "color"}});
    request.outputs = {{{5, "pixels"}, {}}, {{6, "pixels"}, {}}};
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

using ScanProbeConfiguration = std::array<CaptureConfig, 2>;

static RenderResult<EffectDefinition> lowerScanProbe(
    const EffectOutputDesc& output, const ScanProbeConfiguration& configuration, std::span<const FeatureId> features
) noexcept
{
    const auto& [offset_config, status_config] =
        configuration;
    const auto scan_probe = features[0], capture = features[1];
    EffectDefinition scan_request;
    scan_request.features.push_back({scan_probe, "scan", {}});
    scan_request.features.push_back({capture, "offsets", take(ConfigValue::make(offset_config))});
    scan_request.features.push_back({capture, "status", take(ConfigValue::make(status_config))});
    scan_request.connections = {{{0, "offsets"}, {1, "color"}}, {{0, "status"}, {2, "color"}}};
    scan_request.outputs = {{{1, "pixels"}, {}}, {{2, "pixels"}, {}}};
    return scan_request;
}

int main(int argc, char** argv)
{
    const unsigned fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3;
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"test.clustered", nativeValidationEnabled()};
    config.frames_in_flight = fif;
    config.uploads = TransferOptions{1024 * 1024, 4, 16 * 1024 * 1024};
    DeviceOptions device;
    device.texture_descriptors = true;
    device.multiple_queues = true;
    if (argc > 2 && std::string_view(argv[2]) == "--single-queue")
    {
        device.multiple_queues = false;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--same-family")
    {
        device.prefer_dedicated_queues = false;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--paged")
    {
        device.buffer_device_address = true;
    }
    if (argc > 2 && std::string_view(argv[2]) == "--d3d12")
    {
        device.backend = EGraphicsBackend::D3D12;
    }
    auto server = take(RenderServer::create(config, **execution, device));
    auto gbuffer = take(server->featureId("lux.render.feature.GBufferFeature"));
    auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    auto lighting = take(server->featureId("lux.render.feature.DeferredLightingFeature"));
    auto cluster = take(server->featureId("lux.render.feature.ClusteredLightingFeature"));
    auto capture = take(server->registerFeature<CaptureFeature>());
    auto cluster_capture = take(server->registerFeature<ClusterCapture>());
    auto compare = take(server->registerFeature<SceneComparison>());
    auto scan_probe = take(server->registerFeature<ScanProbe>());
    take(server->registerResource<CaptureBuffer>());
    const auto cluster_probe = registerRenderProbe<ClusterProbeConfiguration>(
        *server, "test.clustered", {gbuffer, lighting, cluster, compare, capture, cluster_capture, visibility},
        probePorts({}, {"differences", "lists"}), lowerClusterProbe
    );
    const auto scan_implementation = registerRenderProbe<ScanProbeConfiguration>(
        *server, "test.scan", {scan_probe, capture}, probePorts({}, {"offsets", "status"}), lowerScanProbe
    );

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto meshes = call<GetOrCreateResource>(
        *client, commands, {GlobalRenderScope{}, kMeshResourceName}
    ).resource_id;
    const auto materials = call<GetOrCreateResource>(
        *client, commands, {GlobalRenderScope{}, kMaterialResourceName}
    ).resource_id;

    struct Vertex
    {
        std::array<float, 3> position, normal;
    };

    const std::array<Vertex, 4> vertices{
        {{{-1, -1, 0}, {0, 0, 1}}, {{1, -1, 0}, {0, 0, 1}}, {{1, 1, 0}, {0, 0, 1}}, {{-1, 1, 0}, {0, 0, 1}}}
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
        vertex.uv.setZero();
        vertex.tangent = {1, 0, 0};
        vertex.bitangent = {0, 1, 0};
    }
    auto geometry = take(describeIndexedMesh(mesh_data));
    const auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
    using namespace lux::material;
    MaterialGraph author;
    author.shading_model = rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
    author.render_state.double_sided = true;
    const auto output = material_test::add(author, MaterialOutputSurface{});
    author.param_slots = {{"albedo", EValueType::VEC4, {.5F, .6F, .7F, 1}}};
    const auto albedo = material_test::add(author, MaterialParameter{0, EValueType::VEC4});
    CHECK(material_test::connect(author, albedo, 0, output, 0));
    constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER};
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto compiled = deriveFixtureMaterial(author, purposes, shader_target);
    CHECK(compiled);
    const auto program = registerMaterialProgram(*client, commands, materials, std::move(compiled->material.programs));
    const auto material = createMaterialInstance(*client, commands, program, {{.5F, .6F, .7F, 1}});
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN};
    for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
        const bool perspective = scenario % 2 != 0;
        const auto scene = call<CreateScene>(*client, commands, {}).scene;
        auto instances = call<GetOrCreateResource>(*client, commands, {scene, kInstanceResourceName}).resource_id;
        const std::array descriptions{MeshInstanceData{mesh, material, 0, kIdentityTransform, true}};
        call<InstanceCommands::CreateInstances>(
            *client,
            commands,
            {instances, descriptions}
        );
        auto light_storage = call<GetOrCreateResource>(*client, commands, {scene, kLightResourceName}).resource_id;
        std::vector<VDirectLight> lights;
        if (scenario)
        {
            lights.push_back(DirectionalLightGpu{{.1F, .1F, .1F, 0}, {0, 0, -1, 0}});
        }
        const unsigned local_count = scenario < 2 ? scenario : 128;
        for (unsigned i = 0; i < local_count; ++i)
        {
            const float x = (int(i % 9) - 4) * .43F, y = (int(i % 7) - 3) * .37F;
            const float z = i % 5 == 0 ? 2.03F : .4F + (i % 11) * .31F;
            const float range = i % 17 == 0 ? 100.F : i % 4 == 0 ? .07F : 1.43F;
            if (i % 3)
            {
                lights.push_back(PointLightGpu{{x, y, z, range}, {.01F, .02F, .03F, 0}, {1, 0, 0, 0}});
            }
            else
            {
                lights.push_back(SpotLightGpu{{x, y, z, range}, {.03F, .02F, .01F, 0}, {0, 0, -1, .65F}, {1, 0, 0, .9F}}
                );
            }
        }
        if (!lights.empty())
        {
            call<LightingCommands::CreateLights>(*client, commands, {light_storage, {1, lights}});
        }
        ClusterGrid grid{
            scenario < 2 ? std::array<std::uint32_t, 3>{1, 1, 1} : std::array<std::uint32_t, 3>{7, 5, 9},
            scenario >= 4 ? 1u : 65536u,
            perspective ? EClusterDepth::LOGARITHMIC : EClusterDepth::LINEAR
        };
        CapturedPixels differences, lists;
        GBufferFeatureConfig surface{EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}};
        surface.front_counter_clockwise = false;
        DeferredLightingFeatureConfig reference;
        reference.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        reference.extent.extent.source = EImageExtentSource::TARGET;
        ClusteredLightingFeatureConfig clustered;
        clustered.grid = grid;
        CaptureConfig capture_diff{&differences};
        capture_diff.bytes = 64 * 64 * 4;
        auto request = probeRequest(
            target, cluster_probe,
            ClusterProbeConfiguration{surface, reference, clustered, ClusterCaptureConfig{grid, &lists}, capture_diff},
            {"differences", "lists"}
        );
        auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        std::array<RenderJobId, 2> attachments;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto input = parameters(perspective);
            if (i)
            {
                input.view[12] = -.3F;
                input.position[0] = .3F;
                for (unsigned c = 0; c < 4; ++c)
                {
                    input.inverse_view_projection[c * 4] +=
                        .3F * input.inverse_view_projection[c * 4 + 3];
                }
            }
            auto view = call<CreateView>(*client, commands, {scene}).view;
            auto output = call<CreateTarget>(*client, commands, {target}).target;
            call<UpdateViewCamera>(*client, commands, {{scene, view}, input});
            attachments[i] = startRenderJob(*client, commands, scene, view, output, effect);
        }
        const auto deadline = Clock::now() + 30s;
        for (auto attachment : attachments)
        {
            while (call<QueryExecutionProgress>(*client, commands, attachment)
                       .progress.count(EExecutionOutcome::EXECUTED) < fif + 2)
            {
                renderFrame(*client, commands, attachment);
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            stopAndRemoveJob(*client, commands, attachment);
        }
        call<RemoveScene>(*client, commands, {scene});
        while (differences.ready.load() != 2 || lists.ready.load() != 2)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        verifyEqualPixels(differences);
        verifyLists(lists, grid, lights, perspective);
        call<RemoveEffect>(*client, commands, {effect});
    }
    // Inject unrepresentable count sums into the actual production Scan shader, not a CPU imitation.
    CapturedPixels offsets, status;
    CaptureConfig offset_config{&offsets};
    offset_config.bytes = 130 * 4;
    CaptureConfig status_config{&status};
    status_config.bytes = 16;
    auto scan_request = probeRequest(
        target, scan_implementation, ScanProbeConfiguration{offset_config, status_config}, {"offsets", "status"}
    );
    auto scan_effect = call<CreateEffect>(*client, commands, std::move(scan_request)).effect;
    auto scan_scene = call<CreateScene>(*client, commands, {}).scene;
    auto scan_view = call<CreateView>(*client, commands, {scan_scene}).view;
    auto scan_target = call<CreateTarget>(*client, commands, {target}).target;
    auto attachment = startRenderJob(*client, commands, scan_scene, scan_view, scan_target, scan_effect);
    const auto deadline = Clock::now() + 20s;
    while (call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) <
           fif + 2)
    {
        renderFrame(*client, commands, attachment);
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    stopAndRemoveJob(*client, commands, attachment);
    call<RemoveScene>(*client, commands, {scan_scene});
    while (!offsets.ready.load() || !status.ready.load())
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    unsigned observed = 0;
    for (const auto& frame : offsets.frames)
    {
        if (!load<std::uint32_t>(frame, 4))
        {
            continue;
        }
        CHECK(load<std::uint32_t>(frame, 0) == 0);
        for (unsigned i = 1; i <= 129; ++i)
        {
            CHECK(load<std::uint32_t>(frame, i * 4) == UINT32_MAX);
        }
        ++observed;
    }
    CHECK(observed);
    for (const auto& frame : status.frames)
    {
        auto value = load<ClusterStatusGpu>(frame, 0);
        if (!value.required_indices)
        {
            continue;
        }
        CHECK(value.required_indices == UINT32_MAX && value.flags == 5);
    }
    call<RemoveEffect>(*client, commands, {scan_effect});
    call<MaterialCommands::ReleaseInstance>(
        *client,
        commands,
        {material}
    );
    call<MaterialCommands::UnregisterProgram>(*client, commands, {program});
    call<MeshCommands::ReleaseMesh>(
        *client,
        commands,
        {mesh}
    );
    server->stop();
    thread.join();
    CHECK(server->stats().validation_errors == 0);
    server.reset();
    execution->reset(); // Runtime RAII settles execution after the RenderServer has been destroyed.
    std::printf("clustered scene FIF=%u HDR comparison and CPU list oracle passed\n", fif);
}
