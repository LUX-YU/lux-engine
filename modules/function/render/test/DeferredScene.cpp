#include "MaterialFixture.hpp"
#include "SceneCapture.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/resources/texture/Commands.hpp>
#include <lux/engine/render/resources/lighting/Commands.hpp>
#include <lux/engine/render/resources/material/Commands.hpp>
#include <lux/engine/render/resources/mesh/Commands.hpp>
#include <lux/engine/render/resources/instance/Commands.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <thread>
#include <lux/engine/render/execution/OffscreenTarget.hpp>

using namespace foundation_test;
namespace ipc = lux::platform::ipc;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct SceneParameters
{
    MeshViewParameters mesh;
    DeferredLightingParameters lighting;
    TonemapParameters tone;
    std::array<float, 3> padding{};
};

static_assert(sizeof(SceneParameters) == 272);

static rdesc::ShaderBlock parameterLayout()
{
    auto block = meshViewParameterBlock();
    block.size = sizeof(SceneParameters);
    for (auto field : deferredLightingParameterBlock().fields)
    {
        field.offset += offsetof(SceneParameters, lighting);
        block.fields.push_back(std::move(field));
    }
    auto field = tonemapParameterBlock().fields.front();
    field.offset += offsetof(SceneParameters, tone);
    block.fields.push_back(std::move(field));
    return block;
}

static std::vector<std::byte> parameterBytes(const SceneParameters& values)
{
    auto view = std::as_bytes(std::span(&values, 1));
    return {view.begin(), view.end()};
}

static rdesc::MaterialProgramSet
cook(
    bool lit,
    bool mask = false,
    bool normal_map = false,
    bool shadow_only = false,
    bool double_sided = true,
    bool constant_uv = false
)
{
    using namespace lux::material;
    MaterialGraph graph;
    graph.shading_model = lit ? rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS : rdesc::ELightingTechnique::UNLIT;
    graph.render_state.double_sided = double_sided;
    graph.render_state.alpha_mode = mask ? rdesc::EAlphaMode::MASK : rdesc::EAlphaMode::OPAQUE_SURFACE;
    graph.param_slots = {
        {"albedo", EValueType::VEC4, {0.3F, 0.5F, 0.7F, 1}},
        {"emission", EValueType::VEC4, {0, 0, 0, 0}},
        {"opacity", EValueType::FLOAT, {1, 0, 0, 0}}
    };
    auto surface = material_test::add(graph, MaterialOutputSurface{});
    auto base = material_test::add(graph, MaterialParameter{0, EValueType::VEC4});
    auto emission = material_test::add(graph, MaterialParameter{1, EValueType::VEC4});
    CHECK(material_test::connect(graph, base, 0, surface, 0));
    CHECK(material_test::connect(graph, emission, 0, surface, 5));
    if (mask)
    {
        auto alpha = material_test::add(graph, MaterialParameter{2, EValueType::FLOAT});
        CHECK(material_test::connect(graph, alpha, 0, surface, 1));
    }
    if (normal_map)
    {
        graph.texture_slots.push_back({"normal", lux::asset::AssetId{std::array<std::uint8_t, 16>{1}}});
        auto uv = constant_uv ? material_test::add(graph, MaterialConstant{{}, EValueType::VEC2})
                              : material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        auto sample = material_test::add(graph, MaterialSampleTexture{});
        auto decode = material_test::add(graph, MaterialDecodeNormal{});
        auto tbn = material_test::add(graph, MaterialTbnTransform{});
        CHECK(material_test::connect(graph, uv, 0, sample, 0));
        CHECK(material_test::connect(graph, sample, 0, decode, 0));
        CHECK(material_test::connect(graph, decode, 0, tbn, 0));
        CHECK(material_test::connect(graph, tbn, 0, surface, 4));
    }
    constexpr std::array available{rdesc::EMaterialPurpose::GBUFFER, rdesc::EMaterialPurpose::SHADOW};
    const auto purposes = std::span(available).subspan(shadow_only ? 1 : 0);
    auto cooked = deriveFixtureMaterial(graph, purposes);
    if (!cooked)
    {
        std::fprintf(stderr, "Cook: %s\n", cooked.error().diagnostic.c_str());
    }
    CHECK(cooked);
    CHECK(cooked->material.programs.programs.size() == purposes.size());
    CHECK(cooked->material.programs.find(rdesc::EMaterialPurpose::SHADOW)->shader.interface.entry_points.front().outputs.empty()
    );
    const auto& shadow_use = cooked->material.programs.find(rdesc::EMaterialPurpose::SHADOW)->shader;
    for (const auto& input : shadow_use.interface.entry_points.front().inputs)
    {
        CHECK(input.location == 4);
    }
    if (!mask)
    {
        CHECK(shadow_use.interface.sets.empty());
    }
    graph.render_state.alpha_mode = rdesc::EAlphaMode::BLEND;
    CHECK(!deriveFixtureMaterial(graph, purposes));
    return std::move(cooked->material.programs);
}

static float tonemap(float input, float exposure)
{
    const double x = std::max(0.0, double(input) * std::exp2(exposure));
    return float(std::clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0));
}

static std::array<float, 3> referencePbr(
    std::array<float, 3> base,
    float x,
    float y,
    std::span<const VDirectLight> lights,
    std::array<float, 3> normal,
    float depth = .5F
)
{
    using V = std::array<double, 3>;
    auto dot = [](V a, V b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto unit = [&](V a)
    {
        auto length = std::sqrt(dot(a, a));
        for (auto& v : a)
        {
            v /= length;
        }
        return a;
    };
    const V n = unit({normal[0], normal[1], normal[2]});
    const V position{x, y, depth};
    const V eye = unit({-x, -y, 1 - depth});
    V result{};
    for (const auto& variant : lights)
    {
        std::visit(
            [&](const auto& light)
            {
                V l{};
                double attenuation = 1;
                if constexpr (std::is_same_v<std::decay_t<decltype(light)>, DirectionalLightGpu>)
                {
                    l = {-light.direction[0], -light.direction[1], -light.direction[2]};
                }
                else
                {
                    V delta{
                        light.position_range[0] - position[0],
                        light.position_range[1] - position[1],
                        light.position_range[2] - position[2]
                    };
                    const auto d = std::sqrt(dot(delta, delta));
                    if (d >= light.position_range[3] || d <= 1e-5)
                    {
                        return;
                    }
                    l = unit(delta);
                    const auto& a = [&]() -> const std::array<float, 4>&
                    {
                        if constexpr (std::is_same_v<std::decay_t<decltype(light)>, PointLightGpu>)
                        {
                            return light.attenuation;
                        }
                        else
                        {
                            return light.attenuation_inner;
                        }
                    }();
                    auto window = 1 - std::pow(d / light.position_range[3], 4);
                    attenuation = window * window / (a[0] + a[1] * d + a[2] * d * d);
                    if constexpr (std::is_same_v<std::decay_t<decltype(light)>, SpotLightGpu>)
                    {
                        double cosine =
                            -(l[0] * light.direction_outer[0] + l[1] * light.direction_outer[1] +
                              l[2] * light.direction_outer[2]);
                        auto t = std::clamp(
                            (cosine - light.direction_outer[3]) / (a[3] - light.direction_outer[3]),
                            0.0,
                            1.0
                        );
                        attenuation *= t * t * (3 - 2 * t);
                    }
                }
                const auto nl = std::max(0.0, dot(n, l)), nv = std::max(0.0, dot(n, eye));
                if (nl == 0 || nv == 0)
                {
                    return;
                }
                const auto half = unit({l[0] + eye[0], l[1] + eye[1], l[2] + eye[2]});
                const auto f = .04 + .96 * std::pow(1 - std::max(0.0, dot(half, eye)), 5);
                // Roughness=1: GGX D=1/pi; Schlick G1=2*n/(1+n).
                const auto g = (2 * nl / (1 + nl)) * (2 * nv / (1 + nv));
                for (unsigned c = 0; c < 3; ++c)
                {
                    const double quantized = std::round(base[c] * 255) / 255;
                    result[c] +=
                        ((1 - f) * quantized / 3.141592653589793 + f * g / (3.141592653589793 * (4 * nl * nv + 1e-4))) *
                        light.radiance[c] * attenuation * nl;
                }
            },
            variant
        );
    }
    return {float(result[0]), float(result[1]), float(result[2])};
}

static void verify(
    const CapturedPixels& pixels,
    std::array<float, 3> unlit,
    std::array<float, 3> pbr,
    std::span<const VDirectLight> lights,
    float exposure,
    std::array<float, 3> normal
)
{
    CHECK(!pixels.frames.empty());
    unsigned rendered = 0;
    for (const auto& frame : pixels.frames)
    {
        if (frame[32 * pixels.pitch + 16 * 4 + 3] == std::byte{})
        {
            continue;
        }
        ++rendered;
        for (auto [x, y] : {std::pair{16u, 32u}, std::pair{48u, 32u}})
        {
            auto linear = x < 32 ? unlit : referencePbr(pbr, (x + .5F) / 32 - 1, (y + .5F) / 32 - 1, lights, normal);
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto expected = int(std::lround(tonemap(linear[c], exposure) * 255));
                const auto actual = std::to_integer<int>(frame[y * pixels.pitch + x * 4 + c]);
                if (std::abs(expected - actual) > 3)
                {
                    std::fprintf(
                        stderr,
                        "deferred pixel(%u,%u) c=%u actual=%d expected=%d\n",
                        x,
                        y,
                        c,
                        actual,
                        expected
                    );
                    CHECK(false);
                }
            }
        }
    }
    CHECK(rendered != 0);
}

using DeferredProbeConfiguration = std::tuple<
    GBufferFeatureConfig, DeferredLightingFeatureConfig, TonemapFeatureConfig, CaptureConfig, CaptureConfig, bool
>;

static RenderResult<CapabilityGraphContribution> lowerDeferredProbe(const CapabilityBuildInput& input) noexcept
{
    const auto& [surface, light_config, tonemap, color_capture, command_capture, gpu_visibility] =
        *static_cast<const DeferredProbeConfiguration*>(input.configuration.data());
    const auto& target = input.output;
    const auto& layout = surface.input_layout;
    const auto gbuffer = input.features[0], lighting = input.features[1], tone = input.features[2];
    const auto capture = input.features[3], visibility = input.features[4];
    CapabilityGraphContribution request;
    request.definition.features.push_back({gbuffer, "surfaces", take(ConfigValue::make(surface))});
    request.definition.features.push_back({lighting, "lighting", take(ConfigValue::make(light_config))});
    request.definition.features.push_back({tone, "tonemap", take(ConfigValue::make(tonemap))});
    request.definition.features.push_back({capture, "capture", take(ConfigValue::make(color_capture))});
    for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
    {
        request.definition.connections.push_back({{0, name}, {1, name}});
    }
    request.definition.connections.push_back({{1, "hdr_color"}, {2, "hdr_color"}});
    request.definition.connections.push_back({{2, "color"}, {3, "color"}});
    request.definition.outputs.push_back({{2, "color"}, GraphOutputRequirements{target.format, 1}});
    request.definition.outputs.push_back({{3, "pixels"}, {}});
    {
        MeshVisibilityFeatureConfig selection;
        selection.frustum_culling = gpu_visibility;
        selection.view_parameters = layout;
        request.definition.features.push_back({visibility, "main_visibility", take(ConfigValue::make(selection))});
        for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
        {
            request.definition.connections.push_back({{4, port}, {0, port}});
        }
        auto indirect_capture = command_capture;
        indirect_capture.bytes = kMeshWindingCount * 20;
        request.definition.features.push_back({capture, "indirect", take(ConfigValue::make(indirect_capture))});
        request.definition.connections.push_back({{4, "commands"}, {5, "color"}});
        request.definition.outputs.push_back({{5, "pixels"}, {}});
    }
    publishProbeOutputs(request, input);
    return request;
}

using CoverageProbeConfiguration = std::tuple<MeshFeatureConfig, CaptureConfig>;

static RenderResult<CapabilityGraphContribution> lowerCoverageProbe(const CapabilityBuildInput& input) noexcept
{
    const auto& [depth_config, depth_capture] =
        *static_cast<const CoverageProbeConfiguration*>(input.configuration.data());
    const auto& target = input.output;
    const auto shadow = input.features[0], capture = input.features[1], visibility = input.features[2];
    CapabilityGraphContribution request;
    request.definition.features.push_back({shadow, "coverage", take(ConfigValue::make(depth_config))});
    request.definition.features.push_back(
        {capture, "copy depth", take(ConfigValue::make(depth_capture))}
    );
    request.definition.connections.push_back({{0, "depth"}, {1, "color"}});
    request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.format, 1}});
    request.definition.outputs.push_back({{1, "pixels"}, {}});
    MeshVisibilityFeatureConfig selection;
    selection.view_parameters = meshViewParameterBlock();
    request.definition.features.push_back({visibility, "shadow_visibility", take(ConfigValue::make(selection))});
    for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
    {
        request.definition.connections.push_back({{2, port}, {0, port}});
    }
    publishProbeOutputs(request, input);
    return request;
}

static void descriptorTables()
{
    Validation validation;
    DeviceOptions options;
    options.texture_descriptors = true;
    options.multiple_queues = true;
    options.descriptor_layout_limits = {32, 16, 16};
    options.descriptor_initial_counts = {1, 1, 1};
    auto session = take(DeviceSession::create(options, diagnostics(validation), nativeValidationEnabled()));
    const auto& enabled = session->enabledFeatures();
    std::printf("Descriptor facts: buffers=%u variable=%u queues=%zu\n",
        enabled.buffer_descriptors, enabled.variable_descriptor_count, session->queueCount());
    CHECK(enabled.variable_descriptor_count); // This pressure case needs an actual growth-capable device.
    beginCallCapture(session->nriCore());
    CHECK(call_capture_core);
    auto& core = *call_capture_core;
    static std::uint64_t destroyed_pools = 0;
    static auto destroy_pool = core.DestroyDescriptorPool;
    core.DestroyDescriptorPool = [](nri::DescriptorPool* pool) noexcept
    {
        if (pool) ++destroyed_pools;
        destroy_pool(pool);
    };
    TargetDesc target;
    target.width = target.height = 4;
    target.format = rdesc::ETextureFormat::RGBA8_UNORM;
    target.sampled = true;
    auto image = take(OffscreenTarget::create(*session, target, 1));
    nri::Descriptor* view = nullptr;
    const nri::TextureViewDesc view_desc{image->frame(0).image, nri::TextureView::TEXTURE, nri::Format::RGBA8_UNORM};
    CHECK(core.CreateTextureView(view_desc, view) == nri::Result::SUCCESS);
    auto& table = session->descriptors();
    auto* first = table.descriptors(nri::DescriptorType::TEXTURE);
    const auto ranges = DescriptorTable::layoutRanges({32, 16, 16}, enabled.variable_descriptor_count);
    const nri::DescriptorSetDesc image_set{0, &ranges[0], 1};
    nri::PipelineLayoutDesc layout_desc{};
    layout_desc.descriptorSets = &image_set;
    layout_desc.descriptorSetNum = 1;
    layout_desc.shaderStages = ranges[0].shaderStages;
    layout_desc.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
    nri::PipelineLayout* layout = nullptr;
    CHECK(core.CreatePipelineLayout(session->device(), layout_desc, layout) == nri::Result::SUCCESS);
    for (std::size_t q = 0; q < session->queueCount(); ++q)
    {
        CHECK(submitFixtureCommands(*session, session->physicalQueue(q), [&](nri::CommandBuffer& command, std::uint32_t) noexcept
            -> RenderResult<void>
        {
            if (q == session->queueIndex(session->queue()))
            {
                core.CmdSetPipelineLayout(command, nri::BindPoint::GRAPHICS, *layout);
                core.CmdSetDescriptorSet(command, {0, first, nri::BindPoint::GRAPHICS});
            }
            return {};
        }));
    }
    call_capture_fail_pool = call_capture_counts[0].load() + 1;
    CHECK(!table.allocate(nri::DescriptorType::TEXTURE));
    CHECK(table.descriptors(nri::DescriptorType::TEXTURE) == first);
    CHECK(call_capture_fail_pool.load() == 0);
    call_capture_fail_set = call_capture_counts[4].load() + 1;
    CHECK(!table.allocate(nri::DescriptorType::TEXTURE));
    CHECK(table.descriptors(nri::DescriptorType::TEXTURE) == first);
    CHECK(call_capture_fail_set.load() == 0);
    const auto destroyed_before_growth = destroyed_pools;
    std::vector<DescriptorTable::Slot> slots;
    for (unsigned i = 0; i < 17; ++i)
    {
        auto slot = take(table.allocate(nri::DescriptorType::TEXTURE));
        table.write(nri::DescriptorType::TEXTURE, slot, view);
        slots.push_back(slot);
    }
    CHECK(table.descriptors(nri::DescriptorType::TEXTURE) != first);
    CHECK(destroyed_pools == destroyed_before_growth); // Accepted frontiers have not been dispatched.
    CHECK(session->drain());
    CHECK(destroyed_pools >= destroyed_before_growth + 5);
    const auto removed = slots[3];
    table.release(nri::DescriptorType::TEXTURE, removed);
    slots[3] = take(table.allocate(nri::DescriptorType::TEXTURE));
    CHECK(slots[3].index == removed.index && slots[3].gen != removed.gen);
    table.write(nri::DescriptorType::TEXTURE, slots[3], view);
    while (slots.size() != 31)
    {
        auto slot = take(table.allocate(nri::DescriptorType::TEXTURE));
        table.write(nri::DescriptorType::TEXTURE, slot, view);
        slots.push_back(slot);
    }
    CHECK(!table.allocate(nri::DescriptorType::TEXTURE));
    for (auto slot : slots) table.release(nri::DescriptorType::TEXTURE, slot);
    core.DestroyDescriptor(view);
    core.DestroyPipelineLayout(layout);
    image.reset();
    CHECK(session->drain());

    // Explicit layout requests must fail before exceeding the selected device limit.
    auto invalid_options = options;
    invalid_options.descriptor_layout_limits[1] = session->description().descriptorSet.updateAfterSet.samplerMaxNum + 1;
    auto rejected = DeviceSession::create(invalid_options, diagnostics(validation), nativeValidationEnabled());
    CHECK(!rejected && rejected.error().type == kDeviceCapacity);
    const auto fixed_ranges = DescriptorTable::layoutRanges({32, 16, 16}, false);
    for (const auto& range : fixed_ranges)
        CHECK((range.flags & nri::DescriptorRangeBits::VARIABLE_SIZED_ARRAY) == 0);
    {
        DescriptorTable bounded(*session);
        CHECK(bounded.initialize(options, enabled.buffer_descriptors ? 3 : 2));
        auto slot = take(bounded.allocate(nri::DescriptorType::TEXTURE));
        auto* original = bounded.descriptors(nri::DescriptorType::TEXTURE);
        CHECK(!bounded.allocate(nri::DescriptorType::TEXTURE));
        CHECK(original == bounded.descriptors(nri::DescriptorType::TEXTURE));
        bounded.release(nri::DescriptorType::TEXTURE, slot);
    }
    // Exercise the fixed-allocation branch on the same capable device. No production option/hook is added.
    // All submitted work is drained, and the session's ordinary tables are untouched during this override.
    auto& test_features = const_cast<EnabledDeviceFeatures&>(session->enabledFeatures());
    test_features.variable_descriptor_count = false;
    {
        DescriptorTable fixed(*session);
        CHECK(fixed.initialize(options, 64));
        auto* original = fixed.descriptors(nri::DescriptorType::TEXTURE);
        std::vector<DescriptorTable::Slot> fixed_slots;
        for (unsigned i = 0; i < 32; ++i) fixed_slots.push_back(take(fixed.allocate(nri::DescriptorType::TEXTURE)));
        CHECK(original == fixed.descriptors(nri::DescriptorType::TEXTURE));
        CHECK(!fixed.allocate(nri::DescriptorType::TEXTURE));
        for (auto slot : fixed_slots) fixed.release(nri::DescriptorType::TEXTURE, slot);
    }
    test_features.variable_descriptor_count = true;
    core.DestroyDescriptorPool = destroy_pool;
    session.reset();
    endCallCapture();
    CHECK(validation.errors == 0);
    std::printf("Descriptor tables: growth/reuse/pool-set failure/budget/layout limits PASS\n");
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--descriptor-tables") == 0)
    {
        descriptorTables();
        return 0;
    }
    const auto fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3u;
    auto unlit = cook(false), pbr = cook(true), masked = cook(false, true), normal_mapped = cook(true, false, true);
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(runtime);
    std::atomic<unsigned> diagnostic_count{0};
    RenderServerConfig config{"test.deferred", nativeValidationEnabled()};
    config.diagnostics = {
        &diagnostic_count,
        [](void* context, const RenderDiagnostic& diagnostic) noexcept
        {
            CHECK(diagnostic.error.type == kDeviceDiagnostic);
            CHECK(!diagnostic.message.empty());
            CHECK(!diagnostic.file.empty() && diagnostic.line != 0);
            ++*static_cast<std::atomic<unsigned>*>(context);
            if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            {
                std::fprintf(
                    stderr,
                    "%.*s:%u %.*s\n",
                    int(diagnostic.file.size()),
                    diagnostic.file.data(),
                    diagnostic.line,
                    int(diagnostic.message.size()),
                    diagnostic.message.data()
                );
            }
        }
    };
    config.frames_in_flight = fif;
    // Frame preparation freezes a whole Resource slice; the instance page needs about 1 MiB.
    // Tiny-window asset streaming and capacity rejection are covered by the upload probes.
    config.uploads = TransferOptions{};
    DeviceOptions device{};
    device.multiple_queues = true;
    device.texture_descriptors = true;
    if (std::getenv("LUX_DESCRIPTOR_GROWTH")) device.descriptor_initial_counts.fill(1);
    std::uint32_t draw_groups = 0;
    bool churn_groups = false;
    bool sparse_groups = false;
    bool material_pages = false;
    bool program_cache_probe = false;
    std::uint32_t profile_frames = 0;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view option = argv[i];
        if (option == "--paged")
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
        else if (option == "--groups" && i + 1 < argc)
        {
            draw_groups = static_cast<std::uint32_t>(std::atoi(argv[++i]));
            CHECK(draw_groups == 1 || draw_groups == 64 || draw_groups == 256 || draw_groups == 4096);
        }
        else if (option == "--program-cache" && i + 1 < argc)
        {
            program_cache_probe = true;
            device.cached_unused_programs = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
        else if (option == "--material-pages")
        {
            material_pages = true;
        }
        else if (option == "--sparse")
        {
            sparse_groups = true;
        }
        else if (option == "--churn")
        {
            churn_groups = true;
        }
        else if (option == "--profile-frames" && i + 1 < argc)
        {
            profile_frames = static_cast<std::uint32_t>(std::atoi(argv[++i]));
            CHECK(profile_frames >= 15000);
            CHECK(!std::getenv("LUX_DRAW_CALL_COUNTS"));
            config.validation = false;
        }
        else
        {
            CHECK(false);
        }
    }
    auto server = take(RenderServer::create(config, **runtime, device));
    const auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    const auto gbuffer = take(server->featureId("lux.render.feature.GBufferFeature"));
    const auto lighting = take(server->featureId("lux.render.feature.DeferredLightingFeature"));
    const auto tone = take(server->featureId("lux.render.feature.TonemapFeature"));
    take(server->registerResource<CaptureBuffer>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto shadow = take(server->featureId("lux.render.feature.MeshDepthRaster"));
    const auto deferred_probe = registerRenderProbe<DeferredProbeConfiguration>(
        *server, "test.deferred", {gbuffer, lighting, tone, capture, visibility},
        probePorts({"color"}, {"pixels", "commands"}), lowerDeferredProbe
    );
    const auto coverage_probe = registerRenderProbe<CoverageProbeConfiguration>(
        *server, "test.coverage", {shadow, capture, visibility},
        probePorts({"color"}, {"pixels"}), lowerCoverageProbe
    );

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    CHECK(server->setFrameSource(client->id()));
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto meshes = call<GetOrCreateResource>(
        *client, commands, {GlobalRenderScope{}, kMeshResourceName}
    ).resource_id;
    const auto materials = call<GetOrCreateResource>(
        *client, commands, {GlobalRenderScope{}, kMaterialResourceName}
    ).resource_id;
    const auto instances = call<GetOrCreateResource>(*client, commands, {scene, kInstanceResourceName}).resource_id;
    const auto light_storage = call<GetOrCreateResource>(*client, commands, {scene, kLightResourceName}).resource_id;
    const auto scene_inputs = installInputs(*client, commands, scene);
    const auto instance_updates = scene_inputs.instances;
    const auto light_updates = scene_inputs.lights;

    struct Vertex
    {
        std::array<float, 3> position;
        std::array<float, 3> normal;
        std::array<float, 2> uv{};
        std::array<float, 4> tangent{1, 0, 0, 1};
    };

    // The first two vertices exercise missing and normal-parallel tangents. Their fallback local basis
    // is +X, so the existing mirrored normal-map pixel oracle also verifies the degenerate path.
    const std::array<Vertex, 4> vertices{
        {{{-.48F, -.8F, .5F}, {0, 0, 1}, {}, {0, 0, 0, 0}},
         {{.48F, -.8F, .5F}, {0, 0, 1}, {}, {0, 0, 1, 0}},
         {{.48F, .8F, .5F}, {0, 0, 1}},
         {{-.48F, .8F, .5F}, {0, 0, 1}}}
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
        vertex.tangent = Eigen::Map<const Eigen::Vector3f>(source_vertex.tangent.data());
        vertex.bitangent = {0, 1, 0};
    }
    mesh_data.bounds = lux::math::AABB{{-.48F, -.8F, .5F}, {.48F, .8F, .5F}};
    auto geometry = take(describeIndexedMesh(mesh_data));
    const auto left_mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
    const auto right_mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
    const MaterialParameters left_values{{.2F, .4F, .6F, 1}, {0, 0, 0, 0}, {1, 0, 0, 0}};
    const MaterialParameters right_values{{.3F, .5F, .7F, 1}, {0, 0, 0, 0}, {1, 0, 0, 0}};
    auto left_material = call<MaterialCommands::Create>(*client, commands, {materials, {unlit, left_values}}).material;
    // Distinct records, including later pages, must be addressed through this FIF material reference.
    std::vector<MaterialId> page_materials;
    if (material_pages)
    {
        for (unsigned i = 0; i < 49; ++i)
            page_materials.push_back(
                call<MaterialCommands::Create>(*client, commands, {materials, {pbr, left_values}}).material
            );
        // Holes in earlier pages remain reusable without moving any published record.
        for (unsigned i = 0; i < 16; ++i)
            call<MaterialCommands::Release>(*client, commands, {page_materials[i]});
        page_materials.erase(page_materials.begin(), page_materials.begin() + 16);
    }
    const auto unused = call<MaterialCommands::Create>(*client, commands, {materials, {pbr, left_values}}).material;
    auto right_material = call<MaterialCommands::Create>(*client, commands, {materials, {pbr, right_values}}).material;
    std::array<MeshInstanceData, 2> values{{{left_mesh, left_material}, {right_mesh, right_material}}};
    for (auto& value : values) value.render_enabled = true;
    values[0].transform[12] = -.5F;
    values[1].transform[12] = .5F;
    values[1].transform[0] = -1; // Mirror: normal cofactor sign and double-sided raster must remain correct.
    values[1].transform[5] = .75F;
    auto ids = call<InstanceCommands::CreateInstances>(
                   *client,
                   commands,
                   {instances, values}
    )
                   .instances;
    std::vector<VDirectLight> lights{DirectionalLightGpu{}, PointLightGpu{}, SpotLightGpu{}};
    auto light_ids = call<LightingCommands::CreateLights>(
            *client, commands, {light_storage, {1, lights}}
        ).lights;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    SceneParameters parameters;
    std::array<float, 3> expected_left{.2F, .4F, .6F}, expected_normal{0, 0, 1};
    std::vector<GraphImageDesc> normal_textures;
    std::vector<MaterialId> all_materials{left_material, unused, right_material};
    all_materials.insert(all_materials.end(), page_materials.begin(), page_materials.end());
    const auto layout = parameterLayout();
    bool mixed_winding = false;
    bool churn_active = false;
    bool draw_probe_active = false;
    std::vector<MeshInstanceId> extra_instances;
    std::span<const MeshInstanceUpdate> in_frame_updates;
    std::span<const MeshInstanceId> removed_groups;
    const auto run = [&](float exposure, bool gpu_visibility = true, bool oracle = true)
    {
        CapturedPixels pixels, visible_commands;
        GBufferFeatureConfig surface{EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}};
        surface.front_counter_clockwise = false;
        surface.input_layout = layout;
        DeferredLightingFeatureConfig light_config;
        light_config.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        light_config.extent.extent.source = EImageExtentSource::TARGET;
        light_config.input_layout = layout;
        TonemapFeatureConfig tonemap{
            EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}, EColorEncoding::LINEAR, layout
        };
        auto request = probeRequest(
            target, deferred_probe,
            DeferredProbeConfiguration{surface, light_config, tonemap, CaptureConfig{&pixels},
                CaptureConfig{&visible_commands}, gpu_visibility},
            {"color", "pixels", "commands"}
        );
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {target}).target;
        parameters.tone.exposure = exposure;
        const auto attached =
            call<Attach>(*client, commands, {scene, view, output, effect, parameterBytes(parameters)}).attachment;
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_output = call<CreateTarget>(*client, commands, {target}).target;
        auto second_parameters = parameters;
        if (mixed_winding)
        {
            second_parameters.mesh.view[12] = 4; // Same Effect, independently empty view.
        }
        const auto second = call<Attach>(
                                *client,
                                commands,
                                {scene, second_view, second_output, effect, parameterBytes(second_parameters)}
        )
                                .attachment;
        const auto compiles = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        // Dynamic attachment values must not cause a new Graph/Pipeline compilation.
        auto temporary = second_parameters;
        temporary.tone.exposure = exposure - 1;
        call<UpdateAttachmentInputs>(*client, commands, {second, parameterBytes(temporary)});
        call<UpdateAttachmentInputs>(*client, commands, {second, parameterBytes(second_parameters)});
        CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiles);
        const auto deadline = Clock::now() + (profile_frames ? 300s : draw_groups ? 120s : 10s);
        const auto advance = [&]
        {
            bool resynchronize = false;
            for (auto attachment : {attached, second})
            {
                const auto before = call<QueryExecutionProgress>(*client, commands, attachment)
                                        .progress.count(EExecutionOutcome::EXECUTED);
                while (call<QueryExecutionProgress>(*client, commands, attachment)
                           .progress.count(EExecutionOutcome::EXECUTED) < before + fif + 2)
                {
                    if (churn_active)
                    {
                        const auto failures = call_capture_failed_allocations.load();
                        RenderFrame request{};
                        request.resynchronize = resynchronize;
                        const auto response = call<SubmitFrame>(*client, commands, request, false);
                        if (response.error.type)
                        {
                            CHECK(response.error.type == kDeviceFailure);
                            CHECK(response.error.args[0] == static_cast<std::uint32_t>(nri::Result::OUT_OF_MEMORY));
                            CHECK(call_capture_failed_allocations.load() == failures + 1);
                            // No frame data was applied: the authoritative input remains unchanged.
                            // Retry through the real explicit resynchronization admission contract.
                            resynchronize = response.requires_resynchronization;
                            std::puts("DESCRIPTOR FAILURE delivered; retrying unchanged frame inputs");
                        }
                        else
                        {
                            CHECK(response.outcome == EFrameOutcome::SUBMITTED);
                            resynchronize = false;
                        }
                    }
                    else
                    {
                        renderFrame(*client, commands);
                    }
                    CHECK(Clock::now() < deadline);
                    std::this_thread::sleep_for(1ms);
                }
            }
        };
        if (churn_active)
        {
            // Exercise real preparation failure before the first dynamic private Set is published.
            // A native allocation succeeds before the later Set attempt fails; pool rejection is separate.
            call_capture_fail_pool.store(call_capture_counts[0].load() + 1);
            call_capture_fail_set.store(call_capture_counts[4].load() + 2);
        }
        advance();
        if (churn_active)
        {
            CHECK(call_capture_fail_pool.load() == 0);
            CHECK(call_capture_fail_set.load() == 0);
        }
        if (!removed_groups.empty())
        {
            for (auto id : removed_groups) call<InstanceCommands::ReleaseInstance>(*client, commands, {id});
            removed_groups = {};
            advance();
        }
        if (draw_probe_active && profile_frames)
        {
            for (unsigned frame = 0; frame < 1500; ++frame) renderFrame(*client, commands);
            std::vector<double> intervals;
            intervals.reserve(profile_frames);
            const auto start = Clock::now();
            auto previous = start;
            for (std::uint32_t frame = 0; frame < profile_frames; ++frame)
            {
                renderFrame(*client, commands);
                const auto now = Clock::now();
                intervals.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
                previous = now;
            }
            const auto seconds = std::chrono::duration<double>(previous - start).count();
            std::ranges::sort(intervals);
            std::printf(
                "DRAW_PROFILE groups=%u views=2 culling=%u frames=%u seconds=%.6f fps=%.3f "
                "p50_ms=%.6f p95_ms=%.6f p99_ms=%.6f\n",
                draw_groups, gpu_visibility, profile_frames, seconds, profile_frames / seconds,
                intervals[profile_frames / 2],
                intervals[profile_frames * 95 / 100],
                intervals[profile_frames * 99 / 100]
            );
            std::fflush(stdout);
        }
        if (churn_active)
        {
            CHECK(call_capture_core);
            const auto pools = call_capture_counts[0].load();
            const auto sets = call_capture_counts[4].load();
            const auto views = call_capture_counts[3].load();
            advance();
            CHECK(pools == call_capture_counts[0].load());
            CHECK(sets == call_capture_counts[4].load());
            CHECK(views == call_capture_counts[3].load());
            for (unsigned iteration = 0; iteration < 2; ++iteration)
            {
                const auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
                MeshInstanceData value{mesh, left_material};
                value.render_enabled = false;
                const auto instance = call<InstanceCommands::CreateInstance>(
                    *client, commands, {instances, value}
                ).instance;
                advance();
                CHECK(call_capture_fail_set.load() == 0);
                CHECK(call_capture_fail_pool.load() == 0);
                CHECK(pools == call_capture_counts[0].load());
                CHECK(sets == call_capture_counts[4].load());
                call<InstanceCommands::ReleaseInstance>(*client, commands, {instance});
                advance();
                CHECK(pools == call_capture_counts[0].load());
                CHECK(sets == call_capture_counts[4].load());
                call<MeshCommands::ReleaseMesh>(*client, commands, {mesh});
            }
            std::puts("DESCRIPTOR REUSE PASS stable=zero_create shared_material_churn=zero_sets failures=recovered");
        }
        if (!in_frame_updates.empty())
        {
            updateSceneData(
                *client,
                commands,
                        instance_updates,
                std::span<const MeshInstanceUpdate>{in_frame_updates}
            );
            advance();
        }
        detach(*client, commands, attached);
        detach(*client, commands, second);
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
        while (pixels.ready.load(std::memory_order_acquire) != 2 ||
               (gpu_visibility && visible_commands.ready.load(std::memory_order_acquire) != 2))
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        if (oracle)
        {
            verify(pixels, expected_left, {.3F, .5F, .7F}, lights, exposure, expected_normal);
        }
        if (gpu_visibility && mixed_winding)
        {
            unsigned observed = 0, empty_views = 0;
            for (const auto& frame : visible_commands.frames)
            {
                std::array<std::uint32_t, 10> commands{};
                std::memcpy(commands.data(), frame.data(), sizeof(commands));
                if (!commands[0])
                {
                    continue;
                }
                // One positive and two reflected, all in one geometry/program-table group.
                // Two additional live candidates lie outside the view and must not reach Draw.
                if (commands[1] == 0 && commands[6] == 0)
                {
                    ++empty_views;
                }
                else
                {
                    if (commands[1] != 1 || commands[6] != 2)
                        std::fprintf(stderr, "Winding counts: positive=%u reflected=%u, indices=%u/%u\n",
                            commands[1], commands[6], commands[0], commands[5]);
                    CHECK(commands[1] == 1 && commands[6] == 2);
                    ++observed;
                }
            }
            CHECK(observed != 0 && empty_views != 0);
        }
        call<RemoveEffect>(*client, commands, {effect});
        for (const auto& frame : pixels.frames)
        {
            if (frame[32 * pixels.pitch + (mixed_winding ? 48 : 16) * 4 + (mixed_winding ? 0 : 3)] != std::byte{})
            {
                return frame;
            }
        }
        if (!oracle && !pixels.frames.empty()) return pixels.frames.back();
        CHECK(false);
        return std::vector<std::byte>{};
    };
    if (draw_groups)
    {
        std::vector<MeshId> probe_meshes;
        std::vector<MeshInstanceData> probe_values;
        for (std::uint32_t group = 0; group < draw_groups; ++group)
        {
            const auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
            probe_meshes.push_back(mesh);
            MeshInstanceData value{mesh, left_material};
            value.render_enabled = true;
            value.transform[0] = group % 2 ? -.025F : .025F;
            value.transform[5] = .025F;
            value.transform[12] = -.8F + .1F * static_cast<float>(group % 16);
            value.transform[13] = -.8F + .1F * static_cast<float>(group / 16);
            value.transform[14] = -.1F;
            probe_values.push_back(value);
        }
        auto probe_instances = call<InstanceCommands::CreateInstances>(
            *client, commands,
            {instances, probe_values}
        ).instances;
        churn_active = churn_groups;
        draw_probe_active = true;
        const auto removed = sparse_groups ? draw_groups - std::min(draw_groups, 16u) : 0u;
        removed_groups = std::span(probe_instances).first(removed);
        const auto before = run(0, false, false);
        probe_instances.erase(probe_instances.begin(), probe_instances.begin() + removed);
        CHECK(before == run(0, true, false));
        churn_active = false;
        draw_probe_active = false;
        for (auto id : probe_instances)
        {
            call<InstanceCommands::ReleaseInstance>(*client, commands, {id});
        }
        for (auto mesh : probe_meshes)
        {
            call<MeshCommands::ReleaseMesh>(*client, commands, {mesh});
        }
        std::printf("DRAW GROUPS PASS groups=%u removed=%u views=2 mixed_winding=1\n", draw_groups, removed);
    }
    const auto all_candidates = run(0, false);
    CHECK(all_candidates == run(0));
    if (program_cache_probe)
    {
        CHECK(call_capture_core);
        const auto graphics = call_capture_counts[2].load();
        const auto compute = call_capture_counts[40].load();
        CHECK(all_candidates == run(0));
        if (device.cached_unused_programs >= 256)
        {
            CHECK(call_capture_counts[2].load() == graphics);
            CHECK(call_capture_counts[40].load() == compute);
        }
        else if (device.cached_unused_programs == 0)
        {
            CHECK(call_capture_counts[2].load() > graphics);
            // Resource frame routes own no hidden Effect or cached compute-program use.
            CHECK(call_capture_counts[40].load() > compute);
        }
    }
    if (material_pages)
    {
        // Replacing a record with the same program must refresh the reads source without regrouping.
        const auto previous_material = right_material;
        right_material = call<MaterialCommands::Create>(
            *client, commands, {materials, {pbr, right_values}}
        ).material;
        all_materials.push_back(right_material);
        values[1].material = right_material;
        const std::array replaced_material{MeshInstanceUpdate{ids[1], values[1]}};
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{replaced_material});
        call<MaterialCommands::Release>(*client, commands, {previous_material});
        std::erase(all_materials, previous_material);
        CHECK(all_candidates == run(0));
    }
    // Flags use the existing property stream and survive all FIF replicas. Disabling frustum tests
    // must not bypass render permission; reenabling keeps the same resource/instance identities.
    values[0].render_enabled = values[1].render_enabled = false;
    const std::array hidden{MeshInstanceUpdate{ids[0], values[0]}, MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{hidden});
    const auto hidden_pixels = run(0, false, false);
    CHECK(hidden_pixels == run(0, true, false));
    CHECK(hidden_pixels != all_candidates);
    values[0].render_enabled = values[1].render_enabled = true;
    const std::array shown{MeshInstanceUpdate{ids[0], values[0]}, MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{shown});
    CHECK(all_candidates == run(0));
    auto stale = light_ids.front();
    auto stale_key = extension_detail::ResourceIdentity::local(stale);
    ++stale_key.gen;
    stale = extension_detail::ResourceIdentity::make<decltype(stale)>(
        extension_detail::ResourceIdentity::parent(stale),
        stale_key
    );
    const std::array invalid_light{DirectLightUpdate{stale, lights.front()}};
    auto rejected =
        updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{invalid_light}, false);
    CHECK(rejected.error.type && !rejected.requires_resynchronization);
    CHECK(rejected.outcome == EFrameOutcome::REJECTED);
    // The Resource command rejected the stale identity before a frame was submitted.
    // No frame recovery is needed; the following valid edit still exercises the same live identities.
    std::vector<DirectLightUpdate> restored_lights;
    for (std::size_t i = 0; i < light_ids.size(); ++i)
    {
        restored_lights.push_back({light_ids[i], lights[i]});
    }
    const auto light_recovery =
        updateSceneData(
            *client, commands, light_updates, std::span<const DirectLightUpdate>{restored_lights}
        );
    CHECK(!light_recovery.requires_resynchronization);
    // Update only one light, preserve identities and repair all FIF replicas before subsequent Draws.
    std::get<PointLightGpu>(lights[1]).position_range = {0, 0, 30, .2F};
    const std::array changes{DirectLightUpdate{light_ids[1], lights[1]}};
    const auto changed_lights =
        updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{changes});
    CHECK(changed_lights.error.type == 0);
    run(1);
    const auto texture_storage = call<GetOrCreateResource>(
        *client, commands, {GlobalRenderScope{}, kTextureResourceName}
    ).resource_id;
    rdesc::TextureInfo texture_info{};
    texture_info.width = texture_info.height = texture_info.mip_count = 1;
    texture_info.channel = 4;
    texture_info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    texture_info.color_space = rdesc::ETextureColorSpace::DATA;
    texture_info.mip_ranges[0] = {0, 4, 1, 1};
    const std::array<std::byte, 4> normal_bytes{std::byte{204}, std::byte{128}, std::byte{230}, std::byte{255}};
    auto normal_texture =
        call<TextureCommands::Upload>(*client, commands, {texture_storage, {texture_info, normal_bytes}}).texture;
    pbr = normal_mapped;
    right_material =
        call<MaterialCommands::Create>(*client, commands, {materials, {pbr, right_values, {normal_texture}}}).material;
    all_materials.push_back(right_material);
    values[1].material = right_material;
    const std::array normal_update{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{normal_update});
    normal_textures = {{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
    expected_normal = {-(204 / 255.0F * 2 - 1), 128 / 255.0F * 2 - 1, 230 / 255.0F * 2 - 1};
    const auto normal_reference = run(0);
    // Represent missing UV/tangent channels by zero-valued rdesc::Vertex fields in this fixture.
    // Exercise every vertex, including the mirrored nonuniform instance, against the same pixel oracle.
    auto missing_uv = mesh_data;
    for (auto& vertex : missing_uv.vertices)
    {
        vertex.uv.setZero();
        vertex.tangent.setZero();
        vertex.bitangent.setZero();
    }
    const auto missing_uv_mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &missing_uv}).mesh;
    values[1].mesh = missing_uv_mesh;
    const std::array missing_uv_update{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{missing_uv_update});
    const auto missing_uv_pixels = run(0);
    std::size_t differing_bytes = 0;
    unsigned maximum_difference = 0;
    CHECK(missing_uv_pixels.size() == normal_reference.size());
    for (std::size_t i = 0; i < normal_reference.size(); ++i)
    {
        const auto difference = unsigned(std::abs(
            std::to_integer<int>(missing_uv_pixels[i]) - std::to_integer<int>(normal_reference[i])
        ));
        differing_bytes += difference != 0;
        maximum_difference = std::max(maximum_difference, difference);
    }
    std::printf("MISSING UV compare differing_bytes=%zu maximum_difference=%u\n",
        differing_bytes, maximum_difference);
    std::fflush(stdout);
    if (const auto* path = std::getenv("LUX_UV_COMPARE"))
    {
        auto* output = std::fopen(path, "wb");
        CHECK(output);
        for (const auto* pixels : {&normal_reference, &missing_uv_pixels})
            CHECK(std::fwrite(pixels->data(), 1, pixels->size(), output) == pixels->size());
        std::fclose(output);
    }
    CHECK(missing_uv_pixels == normal_reference);
    const auto no_uv_program = cook(true, false, true, false, true, true);
    const auto& no_uv_inputs = no_uv_program.find(rdesc::EMaterialPurpose::GBUFFER)->shader.interface.entry_points;
    for (const auto& entry : no_uv_inputs)
        CHECK(std::ranges::none_of(entry.inputs, [](const auto& input) { return input.location == 3; }));
    const auto no_uv_material = call<MaterialCommands::Create>(
        *client, commands, {materials, {no_uv_program, right_values, {normal_texture}}}
    ).material;
    all_materials.push_back(no_uv_material);
    values[1].material = no_uv_material;
    const std::array no_uv_input{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{no_uv_input});
    CHECK(run(0) == normal_reference); // Tangent vertex variant without a UV interpolant.
    values[1].mesh = right_mesh;
    values[1].material = right_material;
    const std::array restored_mesh{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored_mesh});
    call<MeshCommands::ReleaseMesh>(*client, commands, {missing_uv_mesh});
    std::puts("MISSING UV PASS: zero UV/tangent/bitangent preserves mirrored normal-map pixels");
    // The readback inspects one geometry/program group. Remove the unrelated unlit group
    // so the first indirect stream is the mixed-winding group checked below.
    call<InstanceCommands::ReleaseInstance>(*client, commands, {ids[0]});
    // Both face modes must support mixed signs within one group and nonuniform normal-mapped transforms.
    for (bool two_sided : {false, true})
    {
        pbr = cook(true, false, true, false, two_sided);
        right_material = call<MaterialCommands::Create>(
                             *client,
                             commands,
                             {materials, {pbr, right_values, {normal_texture}}}
        ).material;
        if (material_pages)
        {
            // The same program draws records in different physical pages in one group.
            for (unsigned i = 0; i < 49; ++i)
                all_materials.push_back(
                    call<MaterialCommands::Create>(
                        *client, commands, {materials, {pbr, right_values, {normal_texture}}}
                    ).material
                );
        }
        const auto other_material = call<MaterialCommands::Create>(
                                        *client,
                                        commands,
                                        {materials, {pbr, left_values, {normal_texture}}}
        ).material;
        all_materials.insert(all_materials.end(), {right_material, other_material});
        values[1].material = right_material;
        const std::array changed{MeshInstanceUpdate{ids[1], values[1]}};
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{changed});
        std::array<MeshInstanceData, 4> additions;
        for (unsigned i = 0; i < additions.size(); ++i)
        {
            additions[i] = {right_mesh, other_material};
            additions[i].render_enabled = true;
            additions[i].transform[0] = i == 1 ? -.3F : .3F;
            additions[i].transform[5] = .25F;
            additions[i].transform[12] = .5F;
            additions[i].transform[13] = i == 0 ? .5F : -.5F;
            additions[i].transform[14] = -.1F;
        }
        additions[2].transform[12] = 4;
        additions[3].transform[14] = 2;
        extra_instances = call<InstanceCommands::CreateInstances>(
                              *client,
                              commands,
                              {instances, additions}
        )
                              .instances;
        mixed_winding = true;
        const auto reference = run(0, false, false);
        const auto culled = run(0, true, false);
        CHECK(reference == culled);
        // Independent interior-pixel oracle: both orientations survive hardware culling, use the
        // second material's parameters and preserve the mirrored tangent-space normal.
        for (auto [y, normal_sign] : {std::pair{48u, 1.F}, {16u, -1.F}})
        {
            const auto color = referencePbr(
                {.2F, .4F, .6F},
                48.5F / 32 - 1,
                (y + .5F) / 32 - 1,
                lights,
                {normal_sign * (204 / 255.F * 2 - 1), 128 / 255.F * 2 - 1, 230 / 255.F * 2 - 1},
                .4F
            );
            for (unsigned c = 0; c < 3; ++c)
            {
                const auto expected = int(std::lround(tonemap(color[c], 0) * 255));
                const auto actual = std::to_integer<int>(reference[(y * 64 + 48) * 4 + c]);
                if (std::abs(actual - expected) > 3)
                    std::fprintf(
                        stderr, "Mixed winding pixel y=%u channel=%u actual=%d expected=%d\n", y, c, actual, expected
                    );
                CHECK(std::abs(actual - expected) <= 3);
            }
        }
        additions[0].transform[0] *= -1;
        additions[1].transform[0] *= -1;
        const std::array flips{
            MeshInstanceUpdate{extra_instances[0], additions[0]},
            MeshInstanceUpdate{extra_instances[1], additions[1]}
        };
        in_frame_updates = flips;
        const auto changed_pixels = run(0, true, false);
        in_frame_updates = {};
        CHECK(changed_pixels == run(0, false, false));
        CHECK(changed_pixels != culled);
        for (auto instance : extra_instances)
        {
            call<InstanceCommands::ReleaseInstance>(
                *client,
                commands,
                {instance}
            );
        }
        mixed_winding = false;
    }
    ids[0] = call<InstanceCommands::CreateInstances>(
        *client, commands, {instances, std::span<const MeshInstanceData>{&values[0], 1}}
    ).instances.front();
    pbr = normal_mapped;
    right_material =
        call<MaterialCommands::Create>(*client, commands, {materials, {pbr, right_values, {normal_texture}}}).material;
    all_materials.push_back(right_material);
    values[1].material = right_material;
    const std::array restored{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored});
    // Resources may upload successfully before a particular Effect prepares their program.
    // A material without GBuffer output fails that visible frame, retaining the old cache entries.
    // Derive the actual requested purpose so unused author parameters/textures are normalized.
    auto incompatible_program = cook(true, false, true, true);
    const auto incompatible_material = call<MaterialCommands::Create>(
        *client, commands, {materials, {incompatible_program, {}, {}}}
    ).material;
    const auto invalid_view = call<CreateView>(*client, commands, {scene}).view;
    const auto invalid_target = call<CreateTarget>(*client, commands, {target}).target;
    CameraInput initial_camera;
    initial_camera.view = parameters.mesh.view;
    initial_camera.projection = parameters.mesh.projection;
    CreateEffectRequest invalid_effect_request{{target.format}};
    for (std::uint32_t i = 0; i < scene_inputs.capabilities.size(); ++i)
    {
        invalid_effect_request.composition.contributions.push_back(
            {i == 0 ? "mesh" : "light", scene_inputs.capabilities[i].type, scene_inputs.implementations[i], {}}
        );
    }
    invalid_effect_request.composition.outputs.push_back({"light", "color"});
    const auto invalid_effect = call<CreateEffect>(*client, commands, std::move(invalid_effect_request)).effect;
    updateSceneData(*client, commands, ViewRequest{scene, invalid_view}, initial_camera);
    const auto invalid_attachment = call<Attach>(
        *client, commands, {scene, invalid_view, invalid_target, invalid_effect}
    ).attachment;
    auto incompatible_instance = values[1];
    incompatible_instance.material = incompatible_material;
    const std::array invalid_update{MeshInstanceUpdate{ids[1], incompatible_instance}};
    const auto rejected_frame = updateSceneData(
        *client,
        commands,
        instance_updates,
        std::span<const MeshInstanceUpdate>{invalid_update},
        false
    );
    CHECK(rejected_frame.error.type == kFeatureContract && !rejected_frame.requires_resynchronization);
    // The standalone edit was already acknowledged; this frame applied no borrowed input packet.
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored});
    detach(*client, commands, invalid_attachment);
    call<RemoveEffect>(*client, commands, {invalid_effect});
    call<RemoveView>(*client, commands, {scene, invalid_view});
    call<RemoveTarget>(*client, commands, invalid_target);
    call<MaterialCommands::Release>(*client, commands, {incompatible_material});
    unlit = masked;
    auto masked_values = left_values;
    masked_values[2][0] = 0;
    left_material = call<MaterialCommands::Create>(*client, commands, {materials, {unlit, masked_values}}).material;
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array mask_update{MeshInstanceUpdate{ids[0], values[0]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{mask_update});
    expected_left = {0, 0, 0};
    run(0);
    const auto check_shadow = [&](float expected)
    {
        CapturedPixels pixels;
        MeshFeatureConfig depth_config{EffectOutputDesc{target.format, target.samples, target.kind, target.sampled}};
        depth_config.front_counter_clockwise = false;
        auto request = probeRequest(
            target, coverage_probe,
            CoverageProbeConfiguration{depth_config, CaptureConfig{&pixels, rdesc::ETextureFormat::D32_SFLOAT}},
            {"color", "pixels"}
        );
        auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        auto view = call<CreateView>(*client, commands, {scene}).view;
        auto output = call<CreateTarget>(*client, commands, {target}).target;
        auto input = meshViewParameterBytes(parameters.mesh);
        auto attachment =
            call<Attach>(*client, commands, {scene, view, output, effect, {input.begin(), input.end()}}).attachment;
        auto before = call<QueryRenderStats>(*client, commands, {}).submissions;
        auto deadline = Clock::now() + 10s;
        while (call<QueryRenderStats>(*client, commands, {}).submissions < before + 20)
        {
            renderFrame(*client, commands);
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        detach(*client, commands, attachment);
        call<RemoveView>(*client, commands, {scene, view});
        while (!pixels.ready.load(std::memory_order_acquire))
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        unsigned rendered = 0;
        for (auto& frame : pixels.frames)
        {
            float background = 0, depth = 0;
            std::memcpy(&background, frame.data(), 4);
            if (background != 1)
            {
                continue;
            }
            std::memcpy(&depth, frame.data() + 32 * pixels.pitch + 16 * 4, 4);
            CHECK(std::abs(depth - expected) < 1e-5F);
            ++rendered;
        }
        CHECK(rendered);
        call<RemoveEffect>(*client, commands, {effect});
    };
    check_shadow(1);
    masked_values[2][0] = 1;
    masked_values[1] = {4, 2, 1, 0};
    call<MaterialCommands::Update>(
        *client,
        commands,
        {{left_material, masked_values, {}}}
    );
    check_shadow(.5F);
    expected_left = {4.2F, 2.4F, 1.6F};
    run(0);
    for (auto id : light_ids)
    {
        call<LightingCommands::ReleaseLight>(
            *client,
            commands,
            {id, 3}
        );
    }
    lights.clear();
    run(0);
    // A Shadow-only cook has no surface table. Create it through the real Storage/Command path
    // and render depth, without borrowing layout from an unrequested GBuffer program.
    unlit = cook(true, false, false, true);
    CHECK(unlit.parameter_defaults.empty() && unlit.texture_count == 0);
    left_material = call<MaterialCommands::Create>(*client, commands, {materials, {unlit, {}}}).material;
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array shadow_update{MeshInstanceUpdate{ids[0], values[0]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{shadow_update});
    check_shadow(.5F);
    call<RemoveScene>(*client, commands, {scene});
    for (auto material : all_materials)
    {
        call<MaterialCommands::Release>(
            *client,
            commands,
            {material}
        );
    }
    for (auto mesh : {left_mesh, right_mesh})
    {
        call<MeshCommands::ReleaseMesh>(
            *client,
            commands,
            {mesh}
        );
    }
    call<TextureCommands::Release>(
        *client,
        commands,
        {normal_texture}
    );
    server->stop();
    thread.join();
    endCallCapture();
    const auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server.reset();
    CHECK(diagnostic_count.load() != 0);
    runtime->reset(); // Runtime RAII settles execution after the RenderServer has been destroyed.
    std::printf(
        "deferred scene: FIF=%u materials=Unlit/PBR Shadow coverage cooked, directional/point/spot, pixel oracle, no "
        "lights, exposure, mirror; validation=%llu warnings=%llu\n",
        fif,
        stats.validation_errors,
        stats.validation_warnings
    );
}
