#include "MaterialFixture.hpp"
#include "ClusterScan.hpp"
#include "ClusterCompare.hpp"
#include "SceneComparison.hpp"
#include <cmath>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/capabilities/ClusterGrid.hpp>
#include <lux/engine/render/resources/lighting/Commands.hpp>
#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/effect/EffectBundles.hpp>
#include <lux/engine/render/effect/ClusterLists.hpp>
#include <lux/engine/render/effect/GBuffer.hpp>
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/MeshVisibility.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#include <numeric>

namespace graph = lux::graph;

static constexpr auto outputs = ClusterListBundle::members;
static constexpr std::uint32_t difference_bytes = 64 * 64 * sizeof(std::uint32_t);
static constexpr std::uint32_t scan_offset_bytes = 130 * sizeof(std::uint32_t);
static constexpr std::uint32_t scan_status_bytes = sizeof(ClusterStatusGpu);
static constexpr std::array<std::string_view, 2> readback_bindings{"first", "second"};
static constexpr auto first_bytes = effectPortSemantic("first");
static constexpr auto second_bytes = effectPortSemantic("second");
static constexpr auto reference_hdr = effectPortSemantic("reference");
static constexpr auto candidate_hdr = effectPortSemantic("candidate");
static constexpr std::string_view cluster_inspection = "test.cluster.inspect";
static constexpr std::string_view scan_inspection = "test.cluster.scan";

static auto sizes(const ClusterGrid& grid)
{
    const auto n = static_cast<std::uint32_t>(grid.count());
    return std::array<std::uint32_t, 6>{128, n * 32, n * 4, (n + 1) * 4, grid.index_capacity * 4, 16};
}

struct ClusterReadbackDescriptor final
{
    using Config = void;
    using Ops = void;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{"test.cluster.readback"}, 0, 0, {}, "View-owned Clustered Lighting readback.",
        EResourceDomain::VIEW
    };
};

struct ClusterReadback final
{
    using Descriptor = ClusterReadbackDescriptor;
    using Configuration = std::array<CaptureConfig, 2>;

    // The fixture sets this before synchronous Resource creation. Each owner copies its own configuration.
    static inline std::atomic<const Configuration*> creation_input{};

    explicit ClusterReadback(ResourceCreateContext& context, const Configuration& config) noexcept
        : storage(context), configuration(config)
    {
    }

    static RenderResult<std::unique_ptr<ClusterReadback>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        const auto* config = creation_input.load();
        if (!config) return lux::cxx::unexpected(RenderError{kInvalidConfig});
        return std::make_unique<ClusterReadback>(context, *config);
    }

    static ResourceRegistration factory() noexcept
    {
        static const StorageBindingOps binding{
            .describeGraphImport = [](const void* object, std::uint32_t projection) noexcept
                -> RenderResult<StorageGraphImport>
            {
                const auto& readback = *static_cast<const ClusterReadback*>(object);
                if (projection >= readback.configuration.size())
                    return lux::cxx::unexpected(RenderError{kStorageContract});
                return StorageGraphImport::bufferOutput(
                    readback.configuration[projection].bytes, EGraphUse::TRANSFER_DESTINATION
                );
            },
            .resolveGraphImport = [](
                void* object, std::uint32_t projection, const VGraphResourceDesc& description,
                std::span<ResourceInput> output
            ) noexcept
            {
                auto& readback = *static_cast<ClusterReadback*>(object);
                return CaptureBuffer::resolve(
                    &readback.storage, description, &readback.configuration[projection], output
                );
            }
        };
        return makeResourceRegistration<Descriptor, ClusterReadback, &create>(nullptr, &binding);
    }

    CaptureBuffer storage;
    Configuration configuration;
};

static RenderResult<void> copyReadback(
    EffectBuildContext& context, std::uint32_t projection, std::span<const ResourceVersion> sources
) noexcept
{
    std::uint64_t bytes = 0;
    auto& builder = context.builder();
    TransferWork work;
    for (const auto source : sources)
    {
        const auto* description = builder.resourceDescription(source.resource);
        const auto* buffer = description ? std::get_if<GraphBufferDesc>(description) : nullptr;
        if (!buffer) return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        work.operations.push_back(CopyBuffer{{source}, {{}, GraphBufferRange{bytes, buffer->size}}});
        bytes += buffer->size;
    }
    const auto bound = context.importFinalBuffer(
        readback_bindings[projection], {ResourceName{ClusterReadbackDescriptor::descriptor.type.name()}},
        {"", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}, projection
    );
    if (!bound) return lux::cxx::unexpected(bound.error());
    for (auto& operation : work.operations) std::get<CopyBuffer>(operation).destination.version = *bound;
    const auto copied = builder.addTransferPass(std::string{readback_bindings[projection]}, std::move(work));
    if (!copied) return lux::cxx::unexpected(copied.error());
    const auto name = projection == 0 ? first_bytes : second_bytes;
    return context.publish(name, {{name, copied->written.front()}});
}

static CapabilityDefinition inspectionDefinition(std::string_view name, bool scene)
{
    CapabilityDefinition definition;
    definition.identity = {graph::nodeTypeId(name), std::string{name}, 1};
    definition.ports = {
        {first_bytes, "first", {"test.cluster.readback-bytes", 1}, graph::EPinDirection::OUTPUT},
        {second_bytes, "second", {"test.cluster.readback-bytes", 1}, graph::EPinDirection::OUTPUT}
    };
    if (scene)
    {
        const EffectValueType hdr{std::string{HdrColorBundle::type}, HdrColorBundle::version};
        definition.ports.push_back({reference_hdr, "reference", hdr, graph::EPinDirection::INPUT, 1, true});
        definition.ports.push_back({candidate_hdr, "candidate", hdr, graph::EPinDirection::INPUT, 1, true});
        definition.ports.push_back({
            Lighting::cluster_lists, "cluster_lists", {std::string{ClusterListBundle::type}, ClusterListBundle::version},
            graph::EPinDirection::INPUT, 1, true
        });
    }
    return definition;
}

static RenderResult<void> compareClustered(EffectBuildContext& context, std::string_view) noexcept
{
    const auto reference = context.inputs(reference_hdr);
    const auto candidate = context.inputs(candidate_hdr);
    const auto lists = context.inputs(Lighting::cluster_lists);
    CHECK(reference.size() == 1 && reference.front().size() == 1 && reference.front().front().member == Lighting::hdr);
    CHECK(candidate.size() == 1 && candidate.front().size() == 1 && candidate.front().front().member == Lighting::hdr);
    CHECK(lists.size() == 1 && lists.front().size() == outputs.size());
    auto& builder = context.builder();
    const auto difference = take(builder.declareResource({"HDR differences", GraphBufferDesc{difference_bytes}}));
    ComputeWork work;
    work.interface.program = take(builder.addProgram(GraphProgram{kClusterCompare.load(context.shaderTarget())}));
    work.interface.resources = {
        {"reference_image", {reference.front().front().version}},
        {"candidate_image", {candidate.front().front().version}},
        {"differences", {{difference, 0}, {}, EGraphAccess::WRITE}}
    };
    work.dispatch = DispatchArguments{{8, 8, 1}};
    const auto compared = take(builder.addComputePass("compare HDR", std::move(work)));
    if (auto copied = copyReadback(context, 0, compared.written); !copied) return copied;

    std::array<ResourceVersion, outputs.size()> ordered;
    for (std::size_t i = 0; i < ordered.size(); ++i)
    {
        const auto semantic = effectPortSemantic(outputs[i]);
        const auto member = std::ranges::find(lists.front(), semantic, &EffectResourceValue::member);
        CHECK(member != lists.front().end());
        ordered[i] = member->version;
    }
    return copyReadback(context, 1, ordered);
}

static RenderResult<void> testScanOverflow(EffectBuildContext& context, std::string_view) noexcept
{
    auto& builder = context.builder();
    const auto counts = take(builder.declareResource({"counts", GraphBufferDesc{129 * sizeof(std::uint32_t)}}));
    const auto offsets = take(builder.declareResource({"offsets", GraphBufferDesc{scan_offset_bytes}}));
    const auto status = take(builder.declareResource({"status", GraphBufferDesc{scan_status_bytes}}));
    const auto initialized = take(builder.addTransferPass("initialize scan inputs", TransferWork{{
        FillBuffer{{{counts, 0}}, UINT32_MAX}, FillBuffer{{{status, 0}}, 0}
    }}));
    ComputeWork work;
    work.interface.program = take(builder.addProgram(GraphProgram{kClusterScan.load(context.shaderTarget())}));
    work.interface.resources = {
        {"counts", {initialized.written[0]}},
        {"offsets", {{offsets, 0}, {}, EGraphAccess::WRITE}},
        {"status", {initialized.written[1], {}, EGraphAccess::READ_WRITE}}
    };
    const std::array<std::uint32_t, 2> constants{129, 32};
    const auto bytes = std::as_bytes(std::span{constants});
    work.interface.parameters.values.assign(bytes.begin(), bytes.end());
    work.dispatch = DispatchArguments{{1, 1, 1}};
    const auto scanned = take(builder.addComputePass("scan", std::move(work)));
    const auto written = std::span{scanned.written};
    if (auto copied = copyReadback(context, 0, written.first(1)); !copied) return copied;
    return copyReadback(context, 1, written.subspan(1, 1));
}

static void
registerInspection(RenderServer& server, std::string_view name, bool scene, EffectImplementation::Build build)
{
    CHECK(server.registerEffectCapability(inspectionDefinition(name, scene)));
    EffectImplementation implementation;
    implementation.identity = {std::string{name}, 1};
    implementation.capability = graph::nodeTypeId(name);
    implementation.supported_outputs = {first_bytes, second_bytes};
    implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
    {
        return {};
    };
    implementation.build = build;
    CHECK(server.registerEffectImplementation(std::move(implementation)));
}

static ViewGpuParameters parameters(bool perspective)
{
    ViewGpuParameters result;
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
    result.position_near = {0, 0, 2, near_z};
    result.direction_far = {0, 0, -1, far_z};
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

static CreateEffectRequest inspectionEffect(const ClusterGrid* grid = nullptr)
{
    const bool scene = grid != nullptr;
    const auto name = scene ? cluster_inspection : scan_inspection;
    CreateEffectRequest request;
    request.output.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto inspection = take(edit.addNode(inspectionDefinition(name, scene), {std::string{name}, 1}));
    if (grid)
    {
        const auto surface = take(edit.addNode(
            GBuffer::definition(), {std::string{GBuffer::indexed_clockwise}, GBuffer::version}
        ));
        const auto reference = take(edit.addNode(
            Lighting::definition(), {std::string{Lighting::direct}, Lighting::version}
        ));
        const auto args = "x=" + std::to_string(grid->cells[0]) + " y=" + std::to_string(grid->cells[1]) +
            " z=" + std::to_string(grid->cells[2]) + " index-capacity=" + std::to_string(grid->index_capacity) +
            (grid->depth == EClusterDepth::LINEAR ? " depth=linear" : " depth=logarithmic");
        const auto clustered = take(edit.addNode(
            Lighting::definition(), {std::string{Lighting::clustered}, Lighting::version}, args
        ));
        const auto gbuffer = edit.pinId(surface, GBuffer::surface);
        CHECK(edit.connect(gbuffer, edit.pinId(reference, Lighting::surface)));
        CHECK(edit.connect(gbuffer, edit.pinId(clustered, Lighting::surface)));
        CHECK(edit.connect(edit.pinId(reference, Lighting::hdr), edit.pinId(inspection, reference_hdr)));
        CHECK(edit.connect(edit.pinId(clustered, Lighting::hdr), edit.pinId(inspection, candidate_hdr)));
        CHECK(edit.connect(
            edit.pinId(clustered, Lighting::cluster_lists), edit.pinId(inspection, Lighting::cluster_lists)
        ));
    }
    CHECK(edit.setOutput("first", edit.pinId(inspection, first_bytes)));
    CHECK(edit.setOutput("second", edit.pinId(inspection, second_bytes)));
    CHECK(edit.commit());
    return request;
}

static void bindReadback(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId session, ViewRequest view,
    const ClusterReadback::Configuration& config, JobBinding& binding
)
{
    ClusterReadback::creation_input.store(&config);
    const auto readback = call<GetOrCreateResource>(client, commands, {
        session, view, ClusterReadbackDescriptor::descriptor.type
    }).resource_id;
    ClusterReadback::creation_input.store(nullptr);
    for (std::uint32_t projection = 0; projection < readback_bindings.size(); ++projection)
        binding.resources.push_back({std::string{readback_bindings[projection]}, readback, projection});
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
    auto server = take(RenderServer::create(config, **execution));
    take(server->registerResource(ClusterReadback::factory()));
    registerInspection(*server, cluster_inspection, true, compareClustered);
    registerInspection(*server, scan_inspection, false, testScanOverflow);

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, device).device;
    const auto meshes = call<GetOrCreateResource>(
        *client, commands, {session, DeviceScope{}, kMeshResourceName}
    ).resource_id;
    const auto materials = call<GetOrCreateResource>(
        *client, commands, {session, DeviceScope{}, kMaterialResourceName}
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
        const auto instances = call<GetOrCreateResource>(
            *client, commands, {session, scene, kInstanceResourceName}
        ).resource_id;
        const auto storage = sceneTransformStorage(*client, commands, session, scene);
        const auto transform = createTransform(*client, commands, storage);
        const std::array descriptions{MeshInstanceData{mesh, material, 0, transform, true}};
        call<InstanceCommands::CreateInstances>(
            *client,
            commands,
            {instances, descriptions}
        );
        const auto light_storage = call<GetOrCreateResource>(
            *client, commands, {session, scene, kLightResourceName}
        ).resource_id;
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
        const auto lengths = sizes(grid);
        const ClusterReadback::Configuration capture{{
            {.result = &differences, .bytes = difference_bytes},
            {.result = &lists, .bytes = std::accumulate(lengths.begin(), lengths.end(), 0u)}
        }};
        const auto effect = createPreparedEffect(*client, commands, session, inspectionEffect(&grid));
        std::array<RenderJobId, 2> attachments;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto input = parameters(perspective);
            if (i)
            {
                input.view[12] = -.3F;
                input.position_near[0] = .3F;
                for (unsigned c = 0; c < 4; ++c)
                {
                    input.inverse_view_projection[c * 4] +=
                        .3F * input.inverse_view_projection[c * 4 + 3];
                }
            }
            auto view = call<CreateView>(*client, commands, {scene}).view;
            auto output = call<CreateTarget>(*client, commands, {session, target}).target;
            JobBinding binding;
            const ViewRequest scope{scene, view};
            binding.view_parameters = call<GetOrCreateResource>(*client, commands, {
                session, scope, kViewParameterResourceName
            }).resource_id;
            call<ViewParameterCommands::Update>(*client, commands, {
                binding.view_parameters, std::span{&input, 1}
            });
            bindReadback(*client, commands, session, scope, capture, binding);
            const auto transforms = device.buffer_device_address ?
                ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
            const auto properties = device.buffer_device_address ?
                EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
            binding.resources.push_back({
                std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT
            });
            binding.resources.push_back({
                std::string{MeshVisibility::transforms_binding}, storage, static_cast<std::uint32_t>(transforms)
            });
            binding.resources.push_back({
                std::string{MeshVisibility::properties_binding}, instances, static_cast<std::uint32_t>(properties)
            });
            if (!device.buffer_device_address)
            {
                binding.resources.push_back({
                    std::string{MeshVisibility::alive_binding}, instances,
                    static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)
                });
            }
            binding.resources.push_back({
                std::string{GBuffer::geometry_binding}, meshes, 0, EJobResourceUse::RASTER_MESHES
            });
            binding.resources.push_back({
                std::string{GBuffer::materials_binding}, materials, 0, EJobResourceUse::RASTER_MATERIALS
            });
            for (std::uint32_t light = 0; light < Lighting::light_bindings.size(); ++light)
                binding.resources.push_back({std::string{Lighting::light_bindings[light]}, light_storage, light});
            attachments[i] = startRenderJob(
                *client, commands, session, scene, view, output, effect, std::move(binding)
            );
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
        verifyEqualPixels(differences, scenario != 0);
        verifyLists(lists, grid, lights, perspective);
        call<RemoveEffect>(*client, commands, {effect});
    }
    // Inject unrepresentable count sums into the actual production Scan shader, not a CPU imitation.
    CapturedPixels offsets, status;
    const ClusterReadback::Configuration scan_capture{{
        {.result = &offsets, .bytes = scan_offset_bytes},
        {.result = &status, .bytes = scan_status_bytes}
    }};
    auto scan_effect = createPreparedEffect(*client, commands, session, inspectionEffect());
    auto scan_scene = call<CreateScene>(*client, commands, {}).scene;
    auto scan_view = call<CreateView>(*client, commands, {scan_scene}).view;
    auto scan_target = call<CreateTarget>(*client, commands, {session, target}).target;
    JobBinding scan_binding;
    bindReadback(*client, commands, session, ViewRequest{scan_scene, scan_view}, scan_capture, scan_binding);
    const auto attachment = startRenderJob(
        *client, commands, session, scan_scene, scan_view, scan_target, scan_effect, std::move(scan_binding)
    );
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
