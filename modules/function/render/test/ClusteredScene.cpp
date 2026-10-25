#include "../../material/test/MaterialTest.hpp"
#include "ClusterScan.hpp"
#include "SceneComparison.hpp"
#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/render/extensions/GraphCommands.hpp>
#include <lux/engine/render/extensions/storage/DirectLighting.hpp>
#include <lux/engine/render/features/ClusteredLightingFeature.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>

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

    static const FeatureGraphOps& graph(bool) noexcept;

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
        ports.storage.attachment.push_back(
            {"pixels",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{bytes},
             EGraphOrigin::IMPORTED,
             {},
             StorageRequirement{
                 StorageName(CaptureDescriptor::descriptor.type.name()),
                 std::string(key),
                 take(ConfigValue::make(capture))
             }}
        );
        return {};
    }

    static RenderResult<void> build(const void* data, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const auto lengths = sizes(static_cast<const ClusterCaptureConfig*>(data)->grid);
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

    static const FeatureGraphOps& graph(bool) noexcept;

    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        for (auto [name, size] : {std::pair{"counts", 129u * 4}, {"offsets", 130u * 4}, {"status", 16u}})
        {
            ports.resources.push_back({name, EFeaturePort::OUTPUT, GraphBufferDesc{size}});
        }
        return {};
    }

    static RenderResult<void> build(const void*, std::string_view key, FeatureBuildContext& context) noexcept
    {
        auto initialized = take(context.graph.addTransferPass(
            featureKey(FeatureName{"test.Scan"}, key, "initialize"),
            TransferWork{
                {FillBuffer{{context.resource("counts")}, UINT32_MAX}, FillBuffer{{context.resource("status")}, 0}}
            }
        ));
        rdesc::SpirvArtifact shader;
        shader.stages.push_back(
            {rdesc::EShaderType::COMPUTE, "main", {std::begin(kClusterScan), std::end(kClusterScan)}}
        );
        CHECK(rdesc::ShaderInfo::deserialize(kClusterScanReflection, shader.interface));
        ComputeWork work;
        work.interface.program = take(context.graph.addProgram(std::move(shader)));
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

inline const FeatureGraphOps& ClusterCapture::graph(bool) noexcept
{
    static const FeatureGraphOps value{ClusterCapture::describe, ClusterCapture::build};
    return value;
}

inline const FeatureGraphOps& ScanProbe::graph(bool) noexcept
{
    static const FeatureGraphOps value{ScanProbe::describe, ScanProbe::build};
    return value;
}

struct Parameters
{
    MeshViewParameters mesh;
    DeferredLightingParameters lighting;
    ClusteredLightingParameters cluster;
};

static auto parameters(bool perspective)
{
    Parameters result;
    result.mesh.view[10] = -1;
    result.mesh.view[14] = 2;
    const float near_z = .1F, far_z = 8, q = far_z / (far_z - near_z), r = near_z * q;
    if (perspective)
    {
        result.mesh.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, q, 1, 0, 0, -r, 0};
        result.lighting
            .inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2 / r, -1 / r, 0, 0, 2 * q / r - 1, q / r};
    }
    else
    {
        result.mesh.projection[10] = 1 / (far_z - near_z);
        result.mesh.projection[14] = -near_z / (far_z - near_z);
        result.lighting
            .inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, near_z - far_z, 0, 0, 0, 2 - near_z, 1};
    }
    result.lighting.camera_position = {0, 0, 2, 0};
    result.cluster = {result.lighting.inverse_view_projection, {0, 0, 2, near_z}, {0, 0, -1, far_z}};
    return result;
}

static auto layout()
{
    auto block = meshViewParameterBlock();
    block.size = sizeof(Parameters);
    for (auto field : deferredLightingParameterBlock().fields)
    {
        field.offset += offsetof(Parameters, lighting);
        block.fields.push_back(std::move(field));
    }
    for (auto field : clusteredLightingParameterBlock().fields)
    {
        field.offset += offsetof(Parameters, cluster);
        block.fields.push_back(std::move(field));
    }
    return block;
}

static std::vector<std::byte> bytes(const Parameters& parameters)
{
    const auto data = std::as_bytes(std::span(&parameters, 1));
    return {data.begin(), data.end()};
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

int main(int argc, char** argv)
{
    const unsigned fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3;
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    RenderServerConfig config{"test.clustered", true};
    config.frames_in_flight = fif;
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
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
    auto server = take(RenderServer::create(config, *execution, device));
    auto gbuffer = take(server->featureId("lux.render.feature.GBufferFeature"));
    auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    auto lighting = take(server->featureId("lux.render.feature.DeferredLightingFeature"));
    auto cluster = take(server->featureId("lux.render.feature.ClusteredLightingFeature"));
    auto capture = take(server->registerFeature<CaptureFeature>());
    auto cluster_capture = take(server->registerFeature<ClusterCapture>());
    auto compare = take(server->registerFeature<SceneComparison>());
    auto scan_probe = take(server->registerFeature<ScanProbe>());
    CommandList bootstrap;
    bootstrap.commands.push_back({std::string(ListCommands::name), take(server->commandId<ListCommands>())});
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    CHECK(server->setFrameSource(client->id()));
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = call<ListCommands>(*client, bootstrap, {});
    const auto meshes = installInput(*client, commands, kIndexedMeshInput);
    const auto materials = installInput(*client, commands, kRasterMaterialInput);

    struct Vertex
    {
        std::array<float, 3> position, normal;
    };

    const std::array<Vertex, 4> vertices{
        {{{-1, -1, 0}, {0, 0, 1}}, {{1, -1, 0}, {0, 0, 1}}, {{1, 1, 0}, {0, 0, 1}}, {{-1, 1, 0}, {0, 0, 1}}}
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
        {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT}, {2, 12, rdesc::ETextureFormat::RGB32_SFLOAT}},
        {{0, 6, 0}}
    };
    const auto mesh = call<CreateMesh>(*client, commands, {meshes, geometry, source}).mesh;
    using namespace lux::material;
    MaterialGraph author;
    author.shading_model = rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;
    author.render_state.double_sided = true;
    const auto output = material_test::add(author, MaterialOutputSurface{});
    author.param_slots = {{"albedo", EValueType::VEC4, {.5F, .6F, .7F, 1}}};
    const auto albedo = material_test::add(author, MaterialParameter{0, EValueType::VEC4});
    CHECK(material_test::connect(author, albedo, 0, output, 0));
    constexpr std::array purposes{rdesc::EMaterialPurpose::GBUFFER};
    auto compiled = compileMaterial(author, purposes);
    CHECK(compiled);
    const auto material = call<CreateMaterial>(
                              *client,
                              commands,
                              {materials, compiled->programs, MaterialParameters{{.5F, .6F, .7F, 1}}}
    ).material;
    RasterMeshGroup group{geometry, compiled->programs, {mesh, material}};
    group.front_counter_clockwise = false;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN};
    const auto block = layout();
    for (unsigned scenario = 0; scenario < 6; ++scenario)
    {
        const bool perspective = scenario % 2 != 0;
        const auto scene = call<CreateScene>(*client, commands, {}).scene;
        auto instances = installInput(*client, commands, kRasterInstancesInput, scene);
        const std::array descriptions{MeshInstanceDescription{mesh, material}};
        call<CreateMeshInstances>(*client, commands, {instances, descriptions});
        auto light_storage = installInput(*client, commands, kDirectLightsInput, scene);
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
            call<CreateDirectLights>(*client, commands, {light_storage, 1, lights});
        }
        ClusterGrid grid{
            scenario < 2 ? std::array<std::uint32_t, 3>{1, 1, 1} : std::array<std::uint32_t, 3>{7, 5, 9},
            scenario >= 4 ? 1u : 65536u,
            perspective ? EClusterDepth::LOGARITHMIC : EClusterDepth::LINEAR
        };
        CapturedPixels differences, lists;
        GBufferFeatureConfig surface{target};
        surface.groups = {group};
        surface.input_layout = block;
        surface.visible_capacity = 8;
        DeferredLightingFeatureConfig reference;
        reference.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        reference.extent.extent.source = EImageExtentSource::TARGET;
        reference.input_layout = block;
        auto optimized = reference;
        optimized.clusters = grid;
        ClusteredLightingFeatureConfig clustered;
        clustered.grid = grid;
        clustered.input_layout = block;
        CaptureConfig capture_diff{&differences};
        capture_diff.bytes = 64 * 64 * 4;
        GraphEffectRequest request{target};
        request.definition.features.push_back({gbuffer, "surface", take(ConfigValue::make(surface))});
        request.definition.features.push_back({lighting, "reference", take(ConfigValue::make(reference))});
        request.definition.features.push_back({cluster, "cluster", take(ConfigValue::make(clustered))});
        request.definition.features.push_back({lighting, "optimized", take(ConfigValue::make(optimized))});
        request.definition.features.push_back({compare, "compare", {}});
        request.definition.features.push_back({capture, "differences", take(ConfigValue::make(capture_diff))});
        request.definition.features.push_back(
            {cluster_capture, "lists", take(ConfigValue::make(ClusterCaptureConfig{grid, &lists}))}
        );
        auto& connections = request.definition.connections;
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
        request.definition.outputs = {{{5, "pixels"}, {}}, {{6, "pixels"}, {}}};
        MeshVisibilityFeatureConfig main_visibility;
        for (const auto& group : surface.groups)
        {
            main_visibility.groups.push_back(group.group);
        }
        main_visibility.candidate_capacity = surface.visible_capacity;
        main_visibility.view_parameters = block;
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
        auto effect = call<CreateGraphEffect>(*client, commands, std::move(request)).effect;
        std::array<AttachmentId, 2> attachments;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto input = parameters(perspective);
            if (i)
            {
                input.mesh.view[12] = -.3F;
                input.lighting.camera_position[0] = .3F;
                for (unsigned c = 0; c < 4; ++c)
                {
                    input.lighting.inverse_view_projection[c * 4] +=
                        .3F * input.lighting.inverse_view_projection[c * 4 + 3];
                }
                input.cluster.inverse_view_projection = input.lighting.inverse_view_projection;
                input.cluster.position_near[0] = .3F;
            }
            auto view = call<CreateView>(*client, commands, {scene}).view;
            auto output = call<CreateTarget>(*client, commands, {target}).target;
            attachments[i] = call<Attach>(*client, commands, {scene, view, output, effect, bytes(input)}).attachment;
        }
        const auto deadline = Clock::now() + 30s;
        for (auto attachment : attachments)
        {
            while (call<QueryExecutionProgress>(*client, commands, attachment)
                       .progress.count(EExecutionOutcome::EXECUTED) < fif + 2)
            {
                renderFrame(*client, commands);
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            detach(*client, commands, attachment);
        }
        while (differences.ready.load() != 2 || lists.ready.load() != 2)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        verifyEqualPixels(differences);
        verifyLists(lists, grid, lights, perspective);
        call<RemoveEffect>(*client, commands, {effect});
        call<RemoveScene>(*client, commands, {scene});
    }
    // Inject unrepresentable count sums into the actual production Scan shader, not a CPU imitation.
    CapturedPixels offsets, status;
    CaptureConfig offset_config{&offsets};
    offset_config.bytes = 130 * 4;
    CaptureConfig status_config{&status};
    status_config.bytes = 16;
    GraphEffectRequest scan_request{target};
    scan_request.definition.features.push_back({scan_probe, "scan", {}});
    scan_request.definition.features.push_back({capture, "offsets", take(ConfigValue::make(offset_config))});
    scan_request.definition.features.push_back({capture, "status", take(ConfigValue::make(status_config))});
    scan_request.definition.connections = {{{0, "offsets"}, {1, "color"}}, {{0, "status"}, {2, "color"}}};
    scan_request.definition.outputs = {{{1, "pixels"}, {}}, {{2, "pixels"}, {}}};
    auto scan_effect = call<CreateGraphEffect>(*client, commands, std::move(scan_request)).effect;
    auto scan_scene = call<CreateScene>(*client, commands, {}).scene;
    auto scan_view = call<CreateView>(*client, commands, {scan_scene}).view;
    auto scan_target = call<CreateTarget>(*client, commands, {target}).target;
    auto attachment = call<Attach>(*client, commands, {scan_scene, scan_view, scan_target, scan_effect, {}}).attachment;
    const auto deadline = Clock::now() + 20s;
    while (call<QueryExecutionProgress>(*client, commands, attachment).progress.count(EExecutionOutcome::EXECUTED) <
           fif + 2)
    {
        renderFrame(*client, commands);
        CHECK(Clock::now() < deadline);
        std::this_thread::yield();
    }
    detach(*client, commands, attachment);
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
    call<RemoveScene>(*client, commands, {scan_scene});
    call<ReleaseMaterial>(*client, commands, material);
    call<ReleaseMesh>(*client, commands, mesh);
    server->stop();
    thread.join();
    CHECK(server->stats().validation_errors == 0);
    server.reset();
    execution->requestStop();
    CHECK(execution->join());
    std::printf("clustered scene FIF=%u HDR comparison and CPU list oracle passed\n", fif);
}
