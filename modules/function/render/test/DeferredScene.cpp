#include "../../material/test/MaterialTest.hpp"
#include "SceneCapture.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/material/Compiler.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/features/DeferredLightingFeature.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/features/GBufferFeature.hpp>
#include <lux/engine/render/features/MeshDepthRaster.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/Registration.hpp>
#include <lux/engine/render/storage/DirectLighting.hpp>
#include <thread>

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

static rdesc::MaterialProgramSet cook(bool lit, bool mask = false, bool normal_map = false, bool shadow_only = false)
{
    using namespace lux::material;
    MaterialGraph graph;
    graph.shading_model = lit ? rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS : rdesc::ELightingTechnique::UNLIT;
    graph.render_state.double_sided = true;
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
        auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
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
    auto cooked = compileMaterial(graph, purposes);
    if (!cooked)
    {
        std::fprintf(stderr, "Cook: %s\n", cooked.error().message.c_str());
    }
    CHECK(cooked);
    CHECK(cooked->programs.programs.size() == purposes.size());
    CHECK(cooked->programs.find(rdesc::EMaterialPurpose::SHADOW)->shader.interface.entry_points.front().outputs.empty()
    );
    const auto& shadow_use = cooked->programs.find(rdesc::EMaterialPurpose::SHADOW)->shader;
    for (const auto& input : shadow_use.interface.entry_points.front().inputs)
    {
        CHECK(input.location == 4);
    }
    if (!mask)
    {
        CHECK(shadow_use.interface.sets.empty());
    }
    graph.render_state.alpha_mode = rdesc::EAlphaMode::BLEND;
    CHECK(!compileMaterial(graph, purposes));
    return std::move(cooked->programs);
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
    std::array<float, 3> normal
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
    const V position{x, y, .5};
    const V eye = unit({-x, -y, .5});
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

int main(int argc, char** argv)
{
    const auto fif = argc > 1 ? unsigned(std::atoi(argv[1])) : 3u;
    auto unlit = cook(false), pbr = cook(true), masked = cook(false, true), normal_mapped = cook(true, false, true);
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.deferred", true};
    config.frames_in_flight = fif;
    config.uploads = TransferOptions{65536, 2, 2 * 1024 * 1024};
    DeviceOptions device{};
    device.multiple_queues = true;
    device.texture_descriptors = true;
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
        else
        {
            CHECK(false);
        }
    }
    auto server = take(createRenderServer(config, *runtime, device));
    const auto gbuffer = take(server->registerFeature<GBufferFeatureDescriptor>());
    const auto lighting = take(server->registerFeature<DeferredLightingFeatureDescriptor>());
    const auto tone = take(server->registerFeature<TonemapFeatureDescriptor>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto shadow = take(server->registerFeature<MeshDepthRasterDescriptor>());
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
        std::array<float, 3> position;
        std::array<float, 3> normal;
        std::array<float, 2> uv{};
        std::array<float, 4> tangent{1, 0, 0, 1};
    };

    const std::array<Vertex, 4> vertices{
        {{{-.48F, -.8F, .5F}, {0, 0, 1}},
         {{.48F, -.8F, .5F}, {0, 0, 1}},
         {{.48F, .8F, .5F}, {0, 0, 1}},
         {{-.48F, .8F, .5F}, {0, 0, 1}}}
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
         {1, 24, rdesc::ETextureFormat::RG32_SFLOAT},
         {3, 32, rdesc::ETextureFormat::RGBA32_SFLOAT}},
        {{0, 6, 0}}
    };
    const auto left_mesh = call<CreateMesh>(*client, commands, {meshes, geometry, source}).mesh;
    const auto right_mesh = call<CreateMesh>(*client, commands, {meshes, geometry, source}).mesh;
    const MaterialParameters left_values{{.2F, .4F, .6F, 1}, {0, 0, 0, 0}, {1, 0, 0, 0}};
    const MaterialParameters right_values{{.3F, .5F, .7F, 1}, {0, 0, 0, 0}, {1, 0, 0, 0}};
    auto left_material = call<CreateMaterial>(*client, commands, {materials, unlit, left_values}).material;
    // A real second slot in the same cooked layout table must be addressed through instance material_index.
    const auto unused = call<CreateMaterial>(*client, commands, {materials, pbr, left_values}).material;
    auto right_material = call<CreateMaterial>(*client, commands, {materials, pbr, right_values}).material;
    std::array<MeshInstanceDescription, 2> values{{{left_mesh, left_material}, {right_mesh, right_material}}};
    values[0].transform[12] = -.5F;
    values[1].transform[12] = .5F;
    values[1].transform[0] = -1; // Mirror: normal cofactor sign and double-sided raster must remain correct.
    values[1].transform[5] = .75F;
    auto ids = call<CreateMeshInstances>(*client, commands, {instances, values}).instances;
    std::vector<VDirectLight> lights{DirectionalLightGpu{}, PointLightGpu{}, SpotLightGpu{}};
    auto light_ids = call<CreateDirectLights>(*client, commands, {light_storage, 1, lights}).lights;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    SceneParameters parameters;
    std::array<float, 3> expected_left{.2F, .4F, .6F}, expected_normal{0, 0, 1};
    std::vector<GraphImageDesc> normal_textures;
    std::vector<MaterialId> all_materials{left_material, unused, right_material};
    const auto layout = parameterLayout();
    const auto run = [&](float exposure)
    {
        CapturedPixels pixels;
        GBufferFeatureConfig surface{target};
        surface.input_layout = layout;
        surface.groups = {{geometry, unlit, {left_mesh, left_material}}, {geometry, pbr, {right_mesh, right_material}}};
        surface.groups[1].textures = normal_textures;
        DeferredLightingFeatureConfig light_config;
        light_config.extent = {64, 64, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        light_config.extent.extent.target_relative = true;
        light_config.input_layout = layout;
        TonemapFeatureConfig tonemap{target, EColorEncoding::LINEAR, layout};
        CreateEffectRequest request{target};
        request.definition.features.push_back({gbuffer, "surfaces", take(ConfigValue::make(surface))});
        request.definition.features.push_back({lighting, "lighting", take(ConfigValue::make(light_config))});
        request.definition.features.push_back({tone, "tonemap", take(ConfigValue::make(tonemap))});
        request.definition.features.push_back({capture, "capture", take(ConfigValue::make(CaptureConfig{&pixels}))});
        for (auto name : {"albedo_metallic", "normal_roughness", "emissive_ao", "depth"})
        {
            request.definition.connections.push_back({{0, name}, {1, name}});
        }
        request.definition.connections.push_back({{1, "hdr_color"}, {2, "hdr_color"}});
        request.definition.connections.push_back({{2, "color"}, {3, "color"}});
        request.definition.outputs.push_back({{2, "color"}, GraphOutputRequirements{target.format, 1}});
        request.definition.outputs.push_back({{3, "pixels"}, {}});
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {target}).target;
        parameters.tone.exposure = exposure;
        const auto attached =
            call<Attach>(*client, commands, {scene, view, output, effect, parameterBytes(parameters)}).attachment;
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_output = call<CreateTarget>(*client, commands, {target}).target;
        const auto second =
            call<Attach>(*client, commands, {scene, second_view, second_output, effect, parameterBytes(parameters)})
                .attachment;
        const auto compiles = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        // Dynamic attachment values must not cause a new Graph/Pipeline compilation.
        auto temporary = parameters;
        temporary.tone.exposure = exposure - 1;
        call<UpdateAttachmentInputs>(*client, commands, {second, parameterBytes(temporary)});
        call<UpdateAttachmentInputs>(*client, commands, {second, parameterBytes(parameters)});
        CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiles);
        const auto before = call<QueryRenderStats>(*client, commands, {}).submissions;
        const auto deadline = Clock::now() + 10s;
        while (call<QueryRenderStats>(*client, commands, {}).submissions < before + 20)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        detach(*client, commands, attached);
        detach(*client, commands, second);
        while (pixels.ready.load(std::memory_order_acquire) != 2)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        verify(pixels, expected_left, {.3F, .5F, .7F}, lights, exposure, expected_normal);
        call<RemoveEffect>(*client, commands, {effect});
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
    };
    run(0);
    auto stale = light_ids.front();
    ++stale.local.gen;
    const std::array invalid_light{DirectLightUpdate{stale, lights.front()}};
    auto rejected = call<UpdateDirectLights>(*client, commands, {light_storage, 8, invalid_light}, false);
    CHECK(rejected.error.type && rejected.sequence == 8);
    // Update only one light, preserve identities and repair all FIF replicas before subsequent Draws.
    std::get<PointLightGpu>(lights[1]).position_range = {0, 0, 30, .2F};
    const std::array changes{DirectLightUpdate{light_ids[1], lights[1]}};
    CHECK(call<UpdateDirectLights>(*client, commands, {light_storage, 2, changes}).changed == 1);
    run(1);
    const auto texture_storage = prepare(*client, commands, kTextureStorageType, "textures");
    rdesc::TextureInfo texture_info{};
    texture_info.width = texture_info.height = texture_info.mip_count = 1;
    texture_info.channel = 4;
    texture_info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    texture_info.color_space = rdesc::ETextureColorSpace::DATA;
    texture_info.mip_ranges[0] = {0, 4, 1, 1};
    const std::array<std::byte, 4> normal_bytes{std::byte{204}, std::byte{128}, std::byte{230}, std::byte{255}};
    auto normal_texture = call<CreateTexture>(*client, commands, {texture_storage, texture_info, normal_bytes}).texture;
    pbr = normal_mapped;
    right_material = call<CreateMaterial>(*client, commands, {materials, pbr, right_values, {normal_texture}}).material;
    all_materials.push_back(right_material);
    values[1].material = right_material;
    const std::array normal_update{MeshInstanceUpdate{ids[1], values[1]}};
    call<UpdateMeshInstances>(*client, commands, {instances, 4, normal_update});
    normal_textures = {{1, 1, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM}};
    expected_normal = {-(204 / 255.0F * 2 - 1), 128 / 255.0F * 2 - 1, 230 / 255.0F * 2 - 1};
    run(0);
    // Invalid normal/tangent input is rejected by the author contract before compiling a Pipeline.
    GBufferFeatureConfig invalid{target};
    invalid.groups = {{geometry, pbr, {right_mesh, right_material}}};
    invalid.groups.front().textures = normal_textures;
    invalid.groups.front().geometry.attributes.pop_back();
    FeaturePorts ignored;
    CHECK(!GBufferFeatureDescriptor::descriptor.describe_ports(&invalid, {}, ignored));
    unlit = masked;
    auto masked_values = left_values;
    masked_values[2][0] = 0;
    left_material = call<CreateMaterial>(*client, commands, {materials, unlit, masked_values}).material;
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array mask_update{MeshInstanceUpdate{ids[0], values[0]}};
    call<UpdateMeshInstances>(*client, commands, {instances, 5, mask_update});
    expected_left = {0, 0, 0};
    run(0);
    const auto check_shadow = [&](float expected)
    {
        CapturedPixels pixels;
        MeshFeatureConfig depth_config{target, {geometry, unlit, {left_mesh, left_material}}};
        CreateEffectRequest request{target};
        request.definition.features.push_back({shadow, "coverage", take(ConfigValue::make(depth_config))});
        request.definition.features.push_back(
            {capture, "copy depth", take(ConfigValue::make(CaptureConfig{&pixels, rdesc::ETextureFormat::D32_SFLOAT}))}
        );
        request.definition.connections.push_back({{0, "depth"}, {1, "color"}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.format, 1}});
        request.definition.outputs.push_back({{1, "pixels"}, {}});
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
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        detach(*client, commands, attachment);
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
        call<RemoveView>(*client, commands, {scene, view});
    };
    check_shadow(1);
    masked_values[2][0] = 1;
    masked_values[1] = {4, 2, 1, 0};
    call<UpdateMaterial>(*client, commands, {left_material, masked_values, {}});
    check_shadow(.5F);
    expected_left = {4.2F, 2.4F, 1.6F};
    run(0);
    for (auto id : light_ids)
    {
        call<RemoveDirectLight>(*client, commands, {id, 3});
    }
    lights.clear();
    run(0);
    // A Shadow-only cook has no surface table. Create it through the real Storage/Command path
    // and render depth, without borrowing layout from an unrequested GBuffer program.
    unlit = cook(true, false, false, true);
    CHECK(unlit.parameter_defaults.empty() && unlit.texture_count == 0);
    left_material = call<CreateMaterial>(*client, commands, {materials, unlit, {}}).material;
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array shadow_update{MeshInstanceUpdate{ids[0], values[0]}};
    call<UpdateMeshInstances>(*client, commands, {instances, 6, shadow_update});
    check_shadow(.5F);
    call<RemoveScene>(*client, commands, {scene});
    for (auto material : all_materials)
    {
        call<ReleaseMaterial>(*client, commands, material);
    }
    for (auto mesh : {left_mesh, right_mesh})
    {
        call<ReleaseMesh>(*client, commands, mesh);
    }
    call<ReleaseTexture>(*client, commands, normal_texture);
    server->stop();
    thread.join();
    const auto stats = server->stats();
    CHECK(stats.validation_errors == 0);
    server.reset();
    runtime->requestStop();
    CHECK(runtime->join());
    std::printf(
        "deferred scene: FIF=%u materials=Unlit/PBR Shadow coverage cooked, directional/point/spot, pixel oracle, no "
        "lights, exposure, mirror; validation=%llu warnings=%llu\n",
        fif,
        stats.validation_errors,
        stats.validation_warnings
    );
}
