#include "NativeDevice.hpp"
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
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/effect/EffectBundles.hpp>
#include <lux/engine/render/effect/GBuffer.hpp>
#include <lux/engine/render/effect/DepthPyramid.hpp>
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/MeshRaster.hpp>
#include <lux/engine/render/effect/MeshVisibility.hpp>
#include <lux/engine/render/effect/Shadow.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/lighting/Binding.hpp>
#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <thread>
#include <lux/engine/render/target/OffscreenTarget.hpp>

using namespace foundation_test;
namespace ipc = lux::platform::ipc;
namespace rdesc = lux::rdesc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
    namespace graph = lux::graph;
    constexpr auto gbuffer_capture_bytes = 64u * 256u;
    constexpr auto gbuffer_capture_total = 4u * gbuffer_capture_bytes;
    constexpr auto gbuffer_bytes = effectPortSemantic("bytes");

    struct GBufferCaptureDescriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.gbuffer.capture"}, 0, 0, {},
            "GBuffer color and depth readback", EResourceDomain::VIEW
        };
    };

    struct GBufferCapture final
    {
        using Descriptor = GBufferCaptureDescriptor;

        explicit GBufferCapture(ResourceCreateContext& context) noexcept : storage(context) {}

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps ops{
                .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    if (projection) return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport{
                        GraphBufferDesc{gbuffer_capture_total}, {}, EGraphUse::TRANSFER_DESTINATION
                    };
                },
                .resolveGraphImport = [](
                    void* object, std::uint32_t, const VGraphResourceDesc& desc, StorageSubscription& subscription,
                    std::span<ResourceInput> output
                ) noexcept
                {
                    auto& capture = *static_cast<GBufferCapture*>(object);
                    const CaptureConfig config{.result = &pixels, .bytes = gbuffer_capture_total};
                    return CaptureBuffer::resolve(&capture.storage, desc, &config, subscription, output);
                }
            };
            return makeResourceRegistration<GBufferCaptureDescriptor, GBufferCapture, &GBufferCapture::create>(
                nullptr, &ops
            );
        }

        static RenderResult<std::unique_ptr<GBufferCapture>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            if (observe_shadow)
            {
                auto& core = const_cast<nri::CoreInterface&>(context.device.nriCore());
                native_bias = core.CmdSetDepthBias;
                core.CmdSetDepthBias = [](nri::CommandBuffer& command, const nri::DepthBiasDesc& bias) noexcept
                {
                    if (bias.constant == 2.F && bias.slope == 1.F) ++directional_bias;
                    if (bias.constant == 7.F && bias.slope == 2.F) ++point_bias;
                    native_bias(command, bias);
                };
            }
            return std::make_unique<GBufferCapture>(context);
        }

        static inline CapturedPixels pixels;
        static inline bool observe_shadow = false;
        static inline decltype(nri::CoreInterface{}.CmdSetDepthBias) native_bias;
        static inline std::atomic_uint directional_bias{}, point_bias{};
        CaptureBuffer storage;
    };

    CapabilityDefinition gbufferCaptureDefinition(bool shadow)
    {
        constexpr std::string_view name = "test.gbuffer.readback";
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(name), std::string{name}, 1};
        definition.ports = {
            {GBuffer::surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version},
                graph::EPinDirection::INPUT, 1, true},
            {gbuffer_bytes, "bytes", {"test.readback_bytes", 1}, graph::EPinDirection::OUTPUT}
        };
        if (shadow)
        {
            definition.ports[0] = {ToneMapping::color, "color", {std::string{ToneMapping::display_type}, 1},
                graph::EPinDirection::INPUT, 1, true};
            definition.ports.push_back({Shadow::shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version},
                graph::EPinDirection::INPUT, 1, true});
        }
        return definition;
    }

    constexpr std::string_view gbuffer_capture_impl = "test.gbuffer.readback.copy";

    void registerGBufferCapture(RenderServer& server, bool shadow)
    {
        take(server.registerResource<GBufferCapture>());
        GBufferCapture::observe_shadow = shadow;
        auto capture = gbufferCaptureDefinition(shadow);
        CHECK(server.registerEffectCapability(capture));
        EffectImplementation implementation;
        implementation.capability = capture.identity.id;
        implementation.identity = {std::string{gbuffer_capture_impl}, 1};
        implementation.supported_outputs = {gbuffer_bytes};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
        {
            return {};
        };
        implementation.build = [](EffectBuildContext& context, std::string_view args) noexcept -> RenderResult<void>
        {
            const ResourceRequirement owner{ResourceName{GBufferCaptureDescriptor::descriptor.type.name()}};
            auto buffer = take(context.importResource("readback", owner, {
                "", GraphBufferDesc{gbuffer_capture_total}, EGraphOrigin::IMPORTED, {},
                EGraphUse::TRANSFER_DESTINATION
            }));
            auto& builder = context.builder();
            if (args == "shadow")
            {
                const auto color = context.inputs(ToneMapping::color);
                const auto shadow = context.inputs(Shadow::shadows);
                CHECK(color.size() == 1 && color.front().size() == 1 && shadow.size() == 1);
                const auto depth = std::ranges::find(
                    shadow.front(), effectPortSemantic("shadow_depth"), &EffectResourceValue::member
                );
                CHECK(depth != shadow.front().end());
                const auto initial = take(builder.addTransferPass("clear shadow readback", TransferWork{{
                    FillBuffer{{buffer}, 0}
                }}));
                const auto composed = take(builder.addTransferPass("copy shadow lighting", TransferWork{{
                    CopyImageBuffer{
                        {color.front().front().version}, {initial.written.front(), GraphBufferRange{0, gbuffer_capture_bytes}},
                        256, gbuffer_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
                    }
                }}));
                const auto copied = take(builder.addTransferPass("copy first shadow layer", TransferWork{{
                    CopyImageBuffer{
                        {depth->version},
                        {composed.written.front(), GraphBufferRange{gbuffer_capture_bytes, gbuffer_capture_bytes}},
                        256, gbuffer_capture_bytes, ImageCopyRegion{0, 0, 64, 64}
                    }
                }}));
                return context.publish(gbuffer_bytes, {{gbuffer_bytes, copied.written.front()}});
            }
            const auto inputs = context.inputs(GBuffer::surface);
            CHECK(inputs.size() == 1);
            constexpr std::array members{0u, 3u, 1u};
            constexpr std::array names{"copy albedo", "copy depth", "copy normals"};
            std::uint64_t destination = 0;
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const bool depth = members[i] == 3;
                const auto pitch = members[i] == 1 ? 512u : 256u;
                const auto bytes = pitch * 64u;
                const auto semantic = effectPortSemantic(GBufferBundle::members[members[i]]);
                const auto input = std::ranges::find(inputs.front(), semantic, &EffectResourceValue::member);
                CHECK(input != inputs.front().end());
                const GraphBufferRange range{destination, bytes};
                const auto copy = take(builder.addTransferPass(names[i], TransferWork{{
                    CopyImageBuffer{
                        {input->version, GraphImageRange{depth ? 2u : 1u, 0, 1, 0, 1}},
                        {buffer, range}, pitch, bytes, ImageCopyRegion{0, 0, 64, 64}
                    }
                }}));
                buffer = copy.written.front();
                destination += bytes;
            }
            return context.publish(gbuffer_bytes, {{gbuffer_bytes, buffer}});
        };
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }
}

static rdesc::MaterialProgramSet
cook(
    rdesc::EShaderTarget target,
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
    auto cooked = deriveFixtureMaterial(graph, purposes, target);
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
        const bool is_material = target == rdesc::EShaderTarget::DXIL
            ? input.semantic == "TEXCOORD" && input.semantic_index == 4 : input.location == 4;
        CHECK(is_material);
    }
    if (!mask)
    {
        CHECK(shadow_use.interface.sets.empty());
    }
    graph.render_state.alpha_mode = rdesc::EAlphaMode::BLEND;
    CHECK(!deriveFixtureMaterial(graph, purposes, target));
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

struct DeferredCommandCapture final
{
    static constexpr std::string_view name = "test.deferred.commands";
    static constexpr auto pixels = effectPortSemantic("commands");

    static CapabilityDefinition definition()
    {
        return {
            .identity = {lux::graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {
                {MeshVisibility::draws, "draws", {std::string{MeshVisibility::draws_type}, 1},
                    lux::graph::EPinDirection::INPUT, 1, true},
                {pixels, "commands", {"test.graph.pixels", 1}, lux::graph::EPinDirection::OUTPUT}
            }
        };
    }

    static void registerWith(RenderServer& server)
    {
        const auto capability = definition();
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.capability = capability.identity.id;
        implementation.identity = {std::string{name}, 1};
        implementation.supported_outputs = {pixels};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
            -> RenderResult<void> { return {}; };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

private:
    static RenderResult<void> build(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto input = context.inputs(MeshVisibility::draws);
        CHECK(input.size() == 1);
        const auto commands = std::ranges::find(input.front(), MeshVisibility::commands, &EffectResourceValue::member);
        CHECK(commands != input.front().end());
        const auto readback = context.importResource(
            name, {ResourceName{GraphCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
                EGraphUse::TRANSFER_DESTINATION},
            1
        );
        if (!readback) return lux::cxx::unexpected(readback.error());
        auto& graph = context.builder();
        const auto initialized = take(graph.addTransferPass("initialize indirect readback", TransferWork{{
            FillBuffer{{*readback}, 0}
        }}));
        constexpr auto bytes = kMeshWindingCount * sizeof(nri::DrawIndexedDesc);
        const auto copied = take(graph.addTransferPass("copy indirect draws", TransferWork{{CopyBuffer{
            {commands->version}, {initialized.written.front(), GraphBufferRange{0, bytes}}
        }}}));
        return context.publish(pixels, {{pixels, copied.written.front()}});
    }
};

struct DepthCoverageCapture final
{
    static constexpr std::string_view name = "test.deferred.depth_coverage";
    static constexpr auto pixels = effectPortSemantic("pixels");

    static CapabilityDefinition definition()
    {
        return {
            .identity = {lux::graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {
                {MeshRaster::depth, "depth", {std::string{DepthPyramid::depth_type}, 1},
                    lux::graph::EPinDirection::INPUT, 1, true},
                {pixels, "pixels", {"test.graph.pixels", 1}, lux::graph::EPinDirection::OUTPUT}
            }
        };
    }

    static void registerWith(RenderServer& server)
    {
        const auto capability = definition();
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.capability = capability.identity.id;
        implementation.identity = {std::string{name}, 1};
        implementation.supported_outputs = {pixels};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
            -> RenderResult<void> { return {}; };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

private:
    static RenderResult<void> build(EffectBuildContext& context, std::string_view) noexcept
    {
        const auto input = context.inputs(MeshRaster::depth);
        CHECK(input.size() == 1 && input.front().size() == 1);
        const auto readback = context.importResource(
            name, {ResourceName{GraphCaptureDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
                EGraphUse::TRANSFER_DESTINATION}
        );
        if (!readback) return lux::cxx::unexpected(readback.error());
        const auto copied = take(context.builder().addTransferPass(
            "copy coverage depth", TransferWork{{CopyImageBuffer{
                {input.front().front().version, GraphImageRange{2}}, {*readback}, 256, GraphCaptureBuffer::image_bytes,
                ImageCopyRegion{0, 0, 64, 64}
            }}}
        ));
        return context.publish(pixels, {{pixels, copied.written.front()}});
    }
};

struct DeferredNodes final
{
    lux::graph::NodeId surface, tone;
};

static DeferredNodes addDeferredNodes(EffectGraphEdit& edit, bool frustum_culling, std::string exposure)
{
    const auto surface = take(edit.addNode(
        GBuffer::definition(), {std::string{GBuffer::indexed_clockwise}, GBuffer::version},
        frustum_culling ? "" : "frustum-culling=false"
    ));
    const auto lighting = take(edit.addNode(
        Lighting::definition(), {std::string{Lighting::direct}, Lighting::version}
    ));
    const auto tone = take(edit.addNode(
        ToneMapping::definition(), {std::string{ToneMapping::linear}, ToneMapping::version}, exposure.empty() ? "" : "exposure=" + exposure
    ));
    CHECK(edit.connect(edit.pinId(surface, GBuffer::surface), edit.pinId(lighting, Lighting::surface)));
    CHECK(edit.connect(edit.pinId(lighting, Lighting::hdr), edit.pinId(tone, ToneMapping::hdr_color)));
    CHECK(edit.setOutput("color", edit.pinId(tone, ToneMapping::color), true));
    return {surface, tone};
}

static void descriptorTables()
{
    Validation validation;
    DeviceOptions options;
    options.texture_descriptors = true;
    options.multiple_queues = true;
    options.descriptor_layout_limits = {32, 16, 16};
    options.descriptor_initial_counts = {1, 1, 1};
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
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
    auto image = std::optional{take(OffscreenTarget::create(*session, target, 1))};
    nri::Descriptor* view = nullptr;
    const nri::TextureViewDesc view_desc{image->frame(0).image, nri::TextureView::TEXTURE, nri::Format::RGBA8_UNORM};
    CHECK(core.CreateTextureView(view_desc, view) == nri::Result::SUCCESS);
    auto& table = session->descriptors();
    auto* first = table.descriptors(nri::DescriptorType::TEXTURE);
    const auto first_generation = table.nativeBindingGeneration();
    const auto first_content = table.contentRevision();
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
    CHECK(table.nativeBindingGeneration() == first_generation && table.contentRevision() == first_content);
    const auto destroyed_before_growth = destroyed_pools;
    std::vector<DescriptorTable::Slot> slots;
    for (unsigned i = 0; i < 17; ++i)
    {
        auto slot = take(table.allocate(nri::DescriptorType::TEXTURE));
        table.write(nri::DescriptorType::TEXTURE, slot, view);
        slots.push_back(slot);
    }
    CHECK(table.descriptors(nri::DescriptorType::TEXTURE) != first);
    CHECK(table.nativeBindingGeneration() > first_generation && table.contentRevision() == first_content + slots.size());
    CHECK(destroyed_pools == destroyed_before_growth); // Accepted frontiers have not been dispatched.
    CHECK(session->drain());
    CHECK(destroyed_pools >= destroyed_before_growth + 5);
    const auto removed = slots[3];
    const auto stable_generation = table.nativeBindingGeneration();
    const auto previous_content = table.contentRevision();
    table.release(nri::DescriptorType::TEXTURE, removed);
    CHECK(table.contentRevision() == previous_content + 1);
    slots[3] = take(table.allocate(nri::DescriptorType::TEXTURE));
    CHECK(slots[3].index == removed.index && slots[3].gen != removed.gen);
    table.write(nri::DescriptorType::TEXTURE, slots[3], view);
    CHECK(table.contentRevision() == previous_content + 2);
    CHECK(table.nativeBindingGeneration() == stable_generation);
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
    auto rejection_host = take(NativeExecution::create());
    auto rejected = DeviceSession::create(
        invalid_options, diagnostics(validation), nativeValidationEnabled(), rejection_host->scheduler()
    );
    CHECK(!rejected && rejected.error().type == kDescriptorLimitExceeded);
    CHECK(rejected.error().args[0] > rejected.error().args[1]);
    CHECK(rejected.error().args[2] == static_cast<std::uint64_t>(EDescriptorLimit::SAMPLER));
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
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(runtime);
    std::atomic<unsigned> diagnostic_count{0};
    RenderServerConfig config{"test.deferred", nativeValidationEnabled()};
    enum class EResponseGate { OPEN, ARMED, BLOCKED };
    std::atomic<EResponseGate> response_gate{EResponseGate::OPEN};
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
    bool transform_growth = false;
    std::uint32_t profile_frames = 0;
    std::uint32_t profile_warmup = 1500;
    bool profile_samples = false;
    bool graph_surface = false;
    bool graph_shadow = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view option = argv[i];
        if (option == "--paged")
        {
            device.buffer_device_address = true;
        }
        else if (option == "--d3d12")
        {
            device.backend = EGraphicsBackend::D3D12;
        }
        else if (option == "--effect-graph")
        {
            graph_surface = true;
        }
        else if (option == "--effect-shadow")
        {
            graph_surface = graph_shadow = true;
        }
        else if (option == "--transform-growth")
        {
            transform_growth = true;
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
            CHECK(profile_frames > 0);
            CHECK(!std::getenv("LUX_DRAW_CALL_COUNTS"));
            config.validation = false;
        }
        else if (option == "--profile-warmup" && i + 1 < argc)
        {
            profile_warmup = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        }
        else if (option == "--profile-samples")
        {
            profile_samples = true;
        }
        else
        {
            CHECK(false);
        }
    }
    config.response_context = &response_gate;
    config.responses_available = [](void* context) noexcept
    {
        auto& gate = *static_cast<std::atomic<EResponseGate>*>(context);
        if (gate.load() != EResponseGate::ARMED) return;
        auto expected = EResponseGate::ARMED;
        if (gate.compare_exchange_strong(expected, EResponseGate::BLOCKED)) gate.wait(EResponseGate::BLOCKED);
    };
    auto server = take(RenderServer::create(config, **runtime));
    if (graph_surface) registerGBufferCapture(*server, graph_shadow);
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto unlit = cook(shader_target, false);
    auto pbr = cook(shader_target, true);
    auto masked = cook(shader_target, false, true);
    auto normal_mapped = cook(shader_target, true, false, true);
    ColorCapture::registerWith(*server);
    DeferredCommandCapture::registerWith(*server);
    DepthCoverageCapture::registerWith(*server);

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
    const auto transform_storage = sceneTransformStorage(*client, commands, session, scene);
    const auto light_storage = call<GetOrCreateResource>(*client, commands, {session, scene, kLightResourceName}).resource_id;
    const auto texture_storage = call<GetOrCreateResource>(
        *client, commands, {session, DeviceScope{}, kTextureResourceName}
    ).resource_id;
    const auto mesh_binding = [&]
    {
        const bool paged = device.buffer_device_address;
        const auto transforms = paged ? ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
        const auto properties = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
        JobBinding binding;
        binding.resources = {
            {std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{MeshVisibility::transforms_binding}, transform_storage, static_cast<std::uint32_t>(transforms)},
            {std::string{MeshVisibility::properties_binding}, instances, static_cast<std::uint32_t>(properties)},
            {std::string{GBuffer::geometry_binding}, meshes, 0, EJobResourceUse::RASTER_MESHES},
            {std::string{GBuffer::materials_binding}, materials, 0, EJobResourceUse::RASTER_MATERIALS},
            {std::string{GBuffer::textures_binding}, texture_storage, 0, EJobResourceUse::RASTER_TEXTURES}
        };
        if (!paged)
        {
            binding.resources.push_back({
                std::string{MeshVisibility::alive_binding}, instances, static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)
            });
        }
        return binding;
    };
    const auto set_view = [&](ViewRequest view, const ViewGpuParameters& camera)
    {
        const auto owner = call<GetOrCreateResource>(*client, commands, {session, view, kViewParameterResourceName});
        call<ViewParameterCommands::Update>(*client, commands, {owner.resource_id, std::span{&camera, 1}});
        return owner.resource_id;
    };
    const auto instance_updates = instances;
    const auto light_updates = light_storage;

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
    std::vector<MaterialProgramId> registered;
    const auto registerProgram = [&](rdesc::MaterialProgramSet program)
    {
        const auto id = registerMaterialProgram(*client, commands, materials, std::move(program));
        registered.push_back(id);
        return id;
    };
    const auto createInstance = [&](MaterialProgramId program, MaterialParameters values, std::vector<TextureId> textures = {})
    {
        return createMaterialInstance(*client, commands, program, std::move(values), std::move(textures));
    };
    auto left_material = createInstance(registerProgram(unlit), left_values);
    auto pbr_id = registerProgram(pbr);
    // Distinct records, including later pages, must be addressed through this FIF material reference.
    std::vector<MaterialId> page_materials;
    if (material_pages)
    {
        for (unsigned i = 0; i < 49; ++i)
            page_materials.push_back(createInstance(pbr_id, left_values));
        // Holes in earlier pages remain reusable without moving any published record.
        for (unsigned i = 0; i < 16; ++i)
            call<MaterialCommands::ReleaseInstance>(*client, commands, {page_materials[i]});
        page_materials.erase(page_materials.begin(), page_materials.begin() + 16);
    }
    const auto unused = createInstance(pbr_id, left_values);
    auto right_material = createInstance(pbr_id, right_values);
    std::array<MeshInstanceData, 2> values{{{left_mesh, left_material}, {right_mesh, right_material}}};
    for (auto& value : values) value.render_enabled = true;
    CHECK(call<InstanceCommands::CreateInstance>(
        *client, commands, {instances, values[0]}, false
    ).error.type == kStorageIdentity);
    std::array base_matrices{kIdentityTransform, kIdentityTransform};
    base_matrices[0][12] = -.5F;
    base_matrices[1][12] = .5F;
    base_matrices[1][0] = -1; // Mirror: normal cofactor sign and double-sided raster must remain correct.
    base_matrices[1][5] = .75F;
    bindTransforms(*client, commands, transform_storage, values, base_matrices);
    auto ids = call<InstanceCommands::CreateInstances>(
                   *client,
                   commands,
                   {instances, values}
    )
                   .instances;
    if (graph_surface)
    {
        const auto textures = call<GetOrCreateResource>(*client, commands, {
            session, DeviceScope{}, kTextureResourceName
        }).resource_id;
        rdesc::TextureInfo texture_info{};
        texture_info.width = texture_info.height = texture_info.mip_count = 1;
        texture_info.channel = 4;
        texture_info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
        texture_info.color_space = rdesc::ETextureColorSpace::DATA;
        texture_info.mip_ranges[0] = {0, 4, 1, 1};
        const std::array normal_bytes{std::byte{204}, std::byte{128}, std::byte{230}, std::byte{255}};
        const auto normal = call<TextureCommands::Upload>(*client, commands, {
            textures, {texture_info, normal_bytes}
        }).texture;
        values[1].material = createInstance(registerProgram(normal_mapped), right_values, {normal});
        const MeshInstanceUpdate textured{ids[1], values[1]};
        call<InstanceCommands::Update>(*client, commands, {instances, std::span{&textured, 1}});
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto target = call<CreateTarget>(*client, commands, {
            session, {64, 64, rdesc::ETextureFormat::RGBA8_UNORM}
        }).target;
        const auto owner = [&](ResourceNameView name)
        {
            return call<GetOrCreateResource>(*client, commands, {session, ViewRequest{scene, view}, name}).resource_id;
        };
        JobBinding binding;
        binding.view_parameters = owner(kViewParameterResourceName);
        const auto paged = device.buffer_device_address;
        const auto transforms = paged ? ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
        const auto properties = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
        binding.resources = {
            {"readback", owner(GBufferCaptureDescriptor::descriptor.type)},
            {std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{MeshVisibility::transforms_binding}, transform_storage, static_cast<std::uint32_t>(transforms)},
            {std::string{MeshVisibility::properties_binding}, instances, static_cast<std::uint32_t>(properties)},
            {std::string{GBuffer::geometry_binding}, meshes, 0, EJobResourceUse::RASTER_MESHES},
            {std::string{GBuffer::materials_binding}, materials, 0, EJobResourceUse::RASTER_MATERIALS},
            {std::string{GBuffer::textures_binding}, textures, 0, EJobResourceUse::RASTER_TEXTURES}
        };
        if (!paged)
        {
            binding.resources.push_back({
                std::string{MeshVisibility::alive_binding}, instances, static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)
            });
        }
        std::array<VDirectLight, 2> shadow_lights{
            DirectionalLightGpu{{1, 1, 1, 0}, {.8F, 0, -.6F, 0}},
            PointLightGpu{{0, 0, 1, 4}, {0, 0, 0, 0}, {1, 0, 0, 0}}
        };
        LightId point_light;
        if (graph_shadow)
        {
            binding.resources.push_back({std::string{Shadow::lights_binding}, light_storage, 0, EJobResourceUse::CPU_SNAPSHOT});
            for (std::uint32_t i = 0; i < Lighting::light_bindings.size(); ++i)
                binding.resources.push_back({std::string{Lighting::light_bindings[i]}, light_storage, i});
            ViewGpuParameters camera;
            // Observe the +Z-facing receiver from the lit side. The orthographic depth is 1 - world Z.
            camera.view[14] = -1.1F;
            camera.projection[10] = -1.F;
            camera.projection[14] = -.1F;
            camera.inverse_view_projection[10] = -1.F;
            camera.inverse_view_projection[14] = 1.F;
            camera.position_near = {0, 0, 1.1F, .1F};
            camera.direction_far = {0, 0, -1, 1.1F};
            call<ViewParameterCommands::Update>(*client, commands, {binding.view_parameters, std::span{&camera, 1}});
            base_matrices[0][12] = -.6F;
            base_matrices[0][14] = .25F;
            base_matrices[1][12] = .1F;
            const std::array transforms{
                TransformRecordUpdate{values[0].transform_id, base_matrices[0]},
                TransformRecordUpdate{values[1].transform_id, base_matrices[1]}
            };
            call<TransformCommands::Update>(*client, commands, {transform_storage, transforms});
            values[1].cast_shadow = false;
            const MeshInstanceUpdate receiver{ids[1], values[1]};
            call<InstanceCommands::Update>(*client, commands, {instances, std::span{&receiver, 1}});
            std::array<LightShadowSettings, 2> settings;
            settings[0].enabled = true;
            settings[0].cascade_count = 2;
            settings[0].cascade_splits = {.7F, 1.F};
            settings[0].depth_bias_constant = 2.F;
            settings[0].depth_bias_slope = 1.F;
            point_light = call<LightingCommands::CreateLights>(*client, commands, {
                light_storage, {1, shadow_lights, settings}
            }).lights[1];
        }
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto surface = take(edit.addNode(
            GBuffer::definition(), {std::string{GBuffer::indexed_clockwise}, GBuffer::version}
        ));
        const auto capture = take(edit.addNode(gbufferCaptureDefinition(graph_shadow),
            {std::string{gbuffer_capture_impl}, 1}, graph_shadow ? "shadow" : ""));
        if (graph_shadow)
        {
            const auto shadow = take(edit.addNode(Shadow::definition(), {
                std::string{Shadow::indexed_clockwise}, Shadow::version
            }, "resolution=64 distance=3 pcf-radius=1"));
            const auto lighting = take(edit.addNode(Lighting::definition(), {std::string{Lighting::direct}, Lighting::version}));
            const auto tone = take(edit.addNode(ToneMapping::definition(), {std::string{ToneMapping::linear}, ToneMapping::version}));
            CHECK(edit.connect(edit.pinId(surface, GBuffer::surface), edit.pinId(lighting, Lighting::surface)));
            CHECK(edit.connect(edit.pinId(shadow, Shadow::shadows), edit.pinId(lighting, Lighting::shadows)));
            CHECK(edit.connect(edit.pinId(shadow, Shadow::shadows), edit.pinId(capture, Shadow::shadows)));
            CHECK(edit.connect(edit.pinId(lighting, Lighting::hdr), edit.pinId(tone, ToneMapping::hdr_color)));
            CHECK(edit.connect(edit.pinId(tone, ToneMapping::color), edit.pinId(capture, ToneMapping::color)));
        }
        else
        {
            CHECK(edit.connect(edit.pinId(surface, GBuffer::surface), edit.pinId(capture, GBuffer::surface)));
        }
        CHECK(edit.setOutput("bytes", edit.pinId(capture, gbuffer_bytes)));
        CHECK(edit.commit());
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto job = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
        call<StartRenderJob>(*client, commands, {session, job, target, binding});
        const auto first = call<RequestRenderJob>(*client, commands, {job});
        if (graph_shadow)
        {
            LightShadowSettings point;
            point.enabled = true;
            point.depth_bias_constant = 7.F;
            point.depth_bias_slope = 2.F;
            const DirectLightUpdate enabled{point_light, shadow_lights[1], point};
            call<LightingCommands::Update>(*client, commands, {light_storage, std::span{&enabled, 1}});
            const auto expanded = call<RequestRenderJob>(*client, commands, {job});
            values[0].cast_shadow = false;
            const MeshInstanceUpdate no_caster{ids[0], values[0]};
            call<InstanceCommands::Update>(*client, commands, {instances, std::span{&no_caster, 1}});
            const auto lit = call<RequestRenderJob>(*client, commands, {job});
            stopAndRemoveJob(*client, commands, job);
            call<RemoveView>(*client, commands, {scene, view});
            server->stop();
            thread.join();
            CHECK(server->stats().validation_errors == 0);
            const auto& pixels = GBufferCapture::pixels;
            CHECK(pixels.ready.load() == 1 && pixels.frames.size() == 3);
            CHECK(first.slot_index != expanded.slot_index && expanded.slot_index != lit.slot_index);
            CHECK(first.slot_index != lit.slot_index);
            CHECK(GBufferCapture::directional_bias >= 2 && GBufferCapture::point_bias >= 6);
            const auto min_depth = [&](std::uint32_t slot)
            {
                float minimum = 1;
                const auto data = std::span{pixels.frames[slot]}.subspan(gbuffer_capture_bytes, gbuffer_capture_bytes);
                for (std::size_t offset = 0; offset < data.size(); offset += sizeof(float))
                {
                    float depth;
                    std::memcpy(&depth, data.data() + offset, sizeof(depth));
                    minimum = std::min(minimum, depth);
                }
                return minimum;
            };
            CHECK(min_depth(first.slot_index) < 1.F && min_depth(expanded.slot_index) < 1.F);
            CHECK(min_depth(lit.slot_index) == 1.F);
            const auto pixel = [&](std::uint32_t slot, std::uint32_t x)
            {
                return std::to_integer<unsigned>(pixels.frames[slot][32 * pixels.pitch + x * 4 + 1]);
            };
            const auto occluded = pixel(first.slot_index, 36), illuminated = pixel(lit.slot_index, 36);
            if (illuminated <= occluded + 3)
                std::fprintf(stderr, "Shadow lighting readback at x36 %u lit and %u occluded\n", illuminated, occluded);
            CHECK(illuminated > occluded + 3);
            CHECK(pixel(expanded.slot_index, 36) == occluded);
            return 0;
        }
        const MaterialColorUpdate update{left_material, 0, {.8F, .1F, .25F, 1}};
        call<MaterialCommands::UpdateColor>(*client, commands, {materials, std::span{&update, 1}});
        const auto updated = call<RequestRenderJob>(*client, commands, {job});
        stopAndRemoveJob(*client, commands, job);
        call<RemoveView>(*client, commands, {scene, view});
        server->stop();
        thread.join();
        CHECK(server->stats().validation_errors == 0);
        const auto& pixels = GBufferCapture::pixels;
        CHECK(pixels.ready.load() == 1 && first.slot_index != updated.slot_index);
        const auto verify = [&](std::uint32_t slot, std::uint32_t x, const std::array<int, 3>& expected)
        {
            const auto& data = pixels.frames[slot];
            const auto offset = 32 * pixels.pitch + x * 4;
            for (std::size_t component = 0; component < expected.size(); ++component)
            {
                const auto actual = std::to_integer<int>(data[offset + component]);
                if (std::abs(actual - expected[component]) > 1)
                    std::fprintf(stderr, "GBuffer slot %u pixel %u component %zu got %d expected %d\n",
                        slot, x, component, actual, expected[component]);
                CHECK(std::abs(actual - expected[component]) <= 1);
            }
            float depth;
            std::memcpy(&depth, data.data() + gbuffer_capture_bytes + offset, sizeof(depth));
            CHECK(depth == .5F);
            std::array<std::uint16_t, 4> normal{};
            const auto normal_offset = 2 * gbuffer_capture_bytes + 32 * 512 + x * 8;
            std::memcpy(normal.data(), data.data() + normal_offset, sizeof(normal));
            const bool textured = x == 48;
            CHECK(normal[2] == (textured ? 0x3c00u : 0u)); // GBuffer's exact half-float shading model tag.
            if (textured) CHECK((normal[0] & 0x7fffu) > 0x3000u);
        };
        verify(first.slot_index, 16, {51, 102, 153});
        verify(first.slot_index, 48, {77, 128, 179});
        verify(updated.slot_index, 16, {204, 26, 64});
        verify(updated.slot_index, 48, {77, 128, 179});
        return 0;
    }
    std::vector<VDirectLight> lights{DirectionalLightGpu{}, PointLightGpu{}, SpotLightGpu{}};
    auto light_ids = call<LightingCommands::CreateLights>(
            *client, commands, {light_storage, {1, lights}}
        ).lights;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    ViewGpuParameters camera;
    camera.position_near[2] = 1;
    camera.background = {0, 0, 0, 1};
    std::array<float, 3> expected_left{.2F, .4F, .6F}, expected_normal{0, 0, 1};
    std::vector<GraphImageDesc> normal_textures;
    std::vector<MaterialId> all_materials{left_material, unused, right_material};
    all_materials.insert(all_materials.end(), page_materials.begin(), page_materials.end());
    bool mixed_winding = false;
    bool churn_active = false;
    bool draw_probe_active = false;
    std::vector<MeshInstanceId> extra_instances;
    std::span<const TransformRecordUpdate> in_frame_transforms;
    std::span<const TransformRecordUpdate> in_frame_transform_records;
    std::span<const TransformRecordUpdate> following_transforms;
    const MeshInstanceUpdate* combined_instance = nullptr;
    std::span<const MeshInstanceId> removed_groups;
    const auto run = [&](float exposure, bool gpu_visibility = true, bool oracle = true,
                         bool reject_transform_packet = false, bool bind_view_parameters = false)
    {
        CapturedPixels pixels, visible_commands;
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto nodes = addDeferredNodes(edit, gpu_visibility, std::to_string(exposure - 1));
        const auto color_capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
        const auto command_capture = take(edit.addNode(DeferredCommandCapture::definition(),
            {std::string{DeferredCommandCapture::name}, 1}));
        CHECK(edit.connect(edit.pinId(nodes.tone, ToneMapping::color), edit.pinId(color_capture, ColorCapture::color)));
        CHECK(edit.connect(
            edit.pinId(nodes.surface, GBuffer::draws), edit.pinId(command_capture, MeshVisibility::draws)
        ));
        CHECK(edit.setOutput("pixels", edit.pinId(color_capture, ColorCapture::pixels)));
        CHECK(edit.setOutput("commands", edit.pinId(command_capture, DeferredCommandCapture::pixels)));
        CHECK(edit.commit());
        const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {session, target}).target;
        const ViewRequest view_scope{scene, view};
        const auto capture_binding = [&](ViewRequest scope, const ViewGpuParameters& input)
        {
            auto binding = mesh_binding();
            for (std::uint32_t i = 0; i < Lighting::light_bindings.size(); ++i)
                binding.resources.push_back({std::string{Lighting::light_bindings[i]}, light_storage, i});
            binding.view_parameters = set_view(scope, input);
            const auto readback = GraphCaptureBuffer::bind(
                *client, commands, session, scope, {&pixels},
                {.result = &visible_commands, .bytes = kMeshWindingCount * sizeof(nri::DrawIndexedDesc)}
            );
            binding.resources.push_back({std::string{ColorCapture::name}, readback, 0});
            binding.resources.push_back({std::string{DeferredCommandCapture::name}, readback, 1});
            return binding;
        };
        const auto binding = capture_binding(view_scope, camera);
        const auto& view_parameters = camera;
        const auto attached = startRenderJob(*client, commands, session, scene, view, output, effect, binding);
        if (bind_view_parameters)
        {
            const auto schema = call<QueryRenderJobInputLayout>(*client, commands, attached).layout;
            using Resources = std::tuple<MaterialResource, InstanceResource, TransformResource, LightResource, ViewParameterResource>;
            auto packet = take(FrameData::create<Resources>(schema));
            auto moved = view_parameters;
            moved.view[12] = 4;
            auto invalid = view_parameters;
            invalid.direction_far[3] = 0;
            const std::array invalid_batch{moved, invalid};
            const auto rejected = call<ViewParameterCommands::Update>(
                *client, commands, {binding.view_parameters, invalid_batch}, false
            );
            CHECK(rejected.error.type == kFrameContract);
            CHECK(packet.addData<ViewParameterResource>(moved));
            CHECK(packet.addData<ViewParameterResource>(invalid));
            CHECK(packet.seal());
            CHECK(call<UpdateFrameData>(*client, commands, {attached, &packet}, false).error.type == kFrameContract);
            packet.release();

            packet = take(FrameData::create<Resources>(schema));
            CHECK(packet.addData<ViewParameterResource>(moved));
            CHECK(packet.addData<ViewParameterResource>(view_parameters));
            CHECK(packet.seal());
            CHECK(call<RequestRenderJob>(*client, commands, {attached, &packet}).job == attached);
            packet.release();
        }
        if (!in_frame_transform_records.empty())
        {
            using Resources =
                std::tuple<MaterialResource, InstanceResource, TransformResource, LightResource, ViewParameterResource>;
            const auto schema = call<QueryRenderJobInputLayout>(*client, commands, attached).layout;
            auto packet = take(FrameData::create<Resources>(schema));
            for (const auto& update : in_frame_transform_records)
                CHECK(packet.addData<TransformResource>(update));
            CHECK(packet.seal());
            CHECK(call<RequestRenderJob>(*client, commands, {attached, &packet}).job == attached);
            packet.release();
        }
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_output = call<CreateTarget>(*client, commands, {session, target}).target;
        auto second_camera = camera;
        if (mixed_winding)
        {
            second_camera.view[12] = 4; // Same Effect, independently empty view.
        }
        const auto second = startRenderJob(
            *client, commands, session, scene, second_view, second_output, effect,
            capture_binding({scene, second_view}, second_camera)
        );
        const auto compiles = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        // The same Effect exposure feeds both Views without compiling new plans.
        call<SetToneMappingExposure>(*client, commands, {effect, nodes.tone, exposure});
        CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiles);
        const auto deadline = Clock::now() + (profile_frames ? 300s : draw_groups ? 120s : 10s);
        const auto advance = [&]
        {
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
                        const auto response = call<RequestRenderJob>(*client, commands, {attachment}, false);
                        if (response.error.type)
                        {
                            CHECK(response.error.type == kDeviceFailure);
                            CHECK(response.error.args[0] == static_cast<std::uint32_t>(nri::Result::OUT_OF_MEMORY));
                            CHECK(call_capture_failed_allocations.load() == failures + 1);
                            std::puts("DESCRIPTOR FAILURE delivered; retrying unchanged frame inputs");
                        }

                    }
                    else
                    {
                        renderFrame(*client, commands, attachment);
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
            for (std::uint32_t frame = 0; frame < profile_warmup; ++frame)
                renderFrame(*client, commands, attached);
            std::vector<double> intervals;
            intervals.reserve(profile_frames);
            const auto submitted_before = server->stats().submissions;
            const auto development_before = server->developmentStats();
            const auto start = Clock::now();
            auto previous = start;
            for (std::uint32_t frame = 0; frame < profile_frames; ++frame)
            {
                renderFrame(*client, commands, attached);
                const auto now = Clock::now();
                intervals.push_back(std::chrono::duration<double, std::milli>(now - previous).count());
                previous = now;
            }
            const auto seconds = std::chrono::duration<double>(previous - start).count();
            const auto submissions = server->stats().submissions - submitted_before;
            if (const auto after = server->developmentStats(); development_before && after)
            {
                const auto& before = *development_before;
                std::printf(
                    "DRAW_COUNTERS culling=%u rounds=%llu slot_cpu_hold_ns=%llu "
                    "program_preparations_holding_slot=%llu program_preparation_ns=%llu\n",
                    gpu_visibility,
                    static_cast<unsigned long long>(after->rounds_opened - before.rounds_opened),
                    static_cast<unsigned long long>(after->slot_cpu_hold_ns - before.slot_cpu_hold_ns),
                    static_cast<unsigned long long>(
                        after->program_preparations_holding_slot - before.program_preparations_holding_slot
                    ),
                    static_cast<unsigned long long>(after->program_preparation_ns - before.program_preparation_ns)
                );
            }
            if (profile_samples)
            {
                for (std::size_t frame = 0; frame < intervals.size(); ++frame)
                    std::printf("DRAW_SAMPLE frame=%zu request_ms=%.6f\n", frame, intervals[frame]);
            }
            std::ranges::sort(intervals);
            std::printf(
                "DRAW_PROFILE groups=%u views=2 culling=%u warmup=%u frames=%u submissions=%llu "
                "seconds=%.6f fps=%.3f "
                "p50_ms=%.6f p95_ms=%.6f p99_ms=%.6f\n",
                draw_groups, gpu_visibility, profile_warmup, profile_frames,
                static_cast<unsigned long long>(submissions), seconds, profile_frames / seconds,
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
                value.transform_id = values[0].transform_id;
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
        if (!in_frame_transforms.empty())
        {
            const auto schema = call<QueryRenderJobInputLayout>(*client, commands, attached).layout;
            using Resources =
                std::tuple<MaterialResource, InstanceResource, TransformResource, LightResource, ViewParameterResource>;
            auto packet = take(FrameData::create<Resources>(schema));
            for (const auto& update : in_frame_transforms) CHECK(packet.addData<TransformResource>(update));
            if (reject_transform_packet) CHECK(packet.addData<TransformResource>(TransformRecordUpdate{}));
            CHECK(packet.seal());
            if (reject_transform_packet)
            {
                const auto reply = call<UpdateFrameData>(*client, commands, {attached, &packet}, false);
                CHECK(reply.error.type == kStorageIdentity);
            }
            else if (!following_transforms.empty())
            {
                CHECK(combined_instance);
                auto combined = take(FrameData::create<Resources>(schema));
                CHECK(combined.addData<TransformResource>(following_transforms.front()));
                CHECK(combined.addData<InstanceResource>(*combined_instance));
                CHECK(combined.seal());
                call<UpdateFrameData>(*client, commands, {attached, &combined});
                combined.release();

                auto duplicate = take(FrameData::create<Resources>(schema));
                for (const auto& update : in_frame_transforms) CHECK(duplicate.addData<TransformResource>(update));
                CHECK(duplicate.seal());
                response_gate.store(EResponseGate::ARMED);
                call<QueryRenderStats>(*client, commands, {});
                const auto gate_deadline = Clock::now() + 10s;
                while (response_gate.load() != EResponseGate::BLOCKED) CHECK(Clock::now() < gate_deadline);
                const auto send = [&]<class Command>(typename Command::Request value)
                {
                    const auto entry = std::ranges::find(commands.commands, Command::name, &CommandInfo::name);
                    CHECK(entry != commands.commands.end());
                    ipc::IpcMessage message;
                    message.emplace<typename Command::Request>(std::move(value));
                    const auto request = nextSceneRequest();
                    CHECK(client->trySend(commandTag(entry->id), request, message));
                    return request;
                };
                const auto first = send.template operator()<RequestRenderJob>({attached, &packet});
                const auto following = send.template operator()<TransformCommands::Update>({
                    transform_storage, following_transforms
                });
                const auto competing = send.template operator()<RequestRenderJob>({attached, &duplicate});
                response_gate.store(EResponseGate::OPEN);
                response_gate.notify_one();
                bool first_done = false, following_done = false, competing_done = false;
                while (!first_done || !following_done || !competing_done)
                {
                    ipc::IpcMessage message;
                    std::uint64_t correlation = 0;
                    CHECK(Clock::now() < gate_deadline);
                    if (client->tryReceive(correlation, message) != ipc::EIpcReceive::RECEIVED)
                    {
                        std::this_thread::yield();
                        continue;
                    }
                    if (correlation == following)
                    {
                        CHECK(!following_done && !message.get<AckResponse>().error.type);
                        following_done = true;
                    }
                    else if (correlation == competing)
                    {
                        CHECK(!competing_done && message.get<JobSubmitted>().error.type == kJobBusy);
                        competing_done = true;
                    }
                    else
                    {
                        CHECK(correlation == first && !first_done && message.get<JobSubmitted>().job == attached);
                        first_done = true;
                    }
                }
                duplicate.release();
            }
            else
            {
                CHECK(call<RequestRenderJob>(*client, commands, {attached, &packet}).job == attached);
            }
            packet.release(); // The raw requester has received the terminal response.
            advance();
        }
        stopAndRemoveJob(*client, commands, attached);
        stopAndRemoveJob(*client, commands, second);
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
        while (pixels.ready.load(std::memory_order_acquire) != 2 ||
               visible_commands.ready.load(std::memory_order_acquire) != 2)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        if (oracle)
        {
            verify(pixels, expected_left, {.3F, .5F, .7F}, lights, exposure, expected_normal);
        }
        if (bind_view_parameters)
        {
            CHECK(std::ranges::all_of(pixels.frames, [&](const auto& frame) noexcept
            {
                return frame[32 * pixels.pitch + 16 * 4 + 3] != std::byte{};
            }));
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
            CHECK(observed == fif && empty_views == fif);
        }
        call<RemoveEffect>(*client, commands, {effect});
        call<RemoveTarget>(*client, commands, output);
        call<RemoveTarget>(*client, commands, second_output);
        for (const auto& frame : pixels.frames)
        {
            if (frame[32 * pixels.pitch + (mixed_winding ? 48 : 16) * 4 + (mixed_winding ? 0 : 3)] != std::byte{})
            {
                if (!in_frame_transforms.empty()) CHECK(std::ranges::count(pixels.frames, frame) == fif);
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
        std::vector<TransformMatrix> probe_matrices;
        for (std::uint32_t group = 0; group < draw_groups; ++group)
        {
            const auto mesh = call<MeshCommands::UploadMesh>(*client, commands, {meshes, &mesh_data}).mesh;
            probe_meshes.push_back(mesh);
            MeshInstanceData value{mesh, left_material};
            value.render_enabled = true;
            auto matrix = kIdentityTransform;
            matrix[0] = group % 2 ? -.025F : .025F;
            matrix[5] = .025F;
            matrix[12] = -.8F + .1F * static_cast<float>(group % 16);
            matrix[13] = -.8F + .1F * static_cast<float>(group / 16);
            matrix[14] = -.1F;
            probe_values.push_back(value);
            probe_matrices.push_back(matrix);
        }
        bindTransforms(*client, commands, transform_storage, probe_values, probe_matrices);
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
        std::printf("PROGRAM CACHE PASS unused_limit=%u\n", device.cached_unused_programs);
    }
    if (material_pages)
    {
        auto invalid_abi = pbr;
        invalid_abi.programs.front().shader.interface.entry_points.front().name += "_invalid";
        const auto rejected = call<MaterialCommands::RegisterProgram>(
            *client, commands, {materials, std::move(invalid_abi)}, false
        );
        CHECK(rejected.error.type == kStorageContract);

        // Distinct registrations with an equal shader and ABI reuse the existing native PSO.
        const auto previous_material = right_material;
        auto same_table = pbr;
        CHECK(!same_table.parameter_defaults.empty());
        same_table.parameter_defaults.front()[0] += 1;
        const auto duplicate = registerProgram(std::move(same_table));
        CHECK(duplicate != pbr_id);
        const auto pipelines = call_capture_counts[2].load();
        right_material = createInstance(duplicate, right_values);
        all_materials.push_back(right_material);
        values[1].material = right_material;
        const std::array replaced_material{MeshInstanceUpdate{ids[1], values[1]}};
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{replaced_material});
        call<MaterialCommands::ReleaseInstance>(*client, commands, {previous_material});
        std::erase(all_materials, previous_material);
        CHECK(all_candidates == run(0));
        if (program_cache_probe && device.cached_unused_programs >= 256)
        {
            CHECK(call_capture_counts[2].load() == pipelines);
            std::puts("MATERIAL_REGISTER_CACHE equal_content_distinct_ids=1 additional_psos=0");
        }
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
    auto displaced = kIdentityTransform;
    displaced[12] = 4;
    const std::array matrices{displaced, base_matrices[0]};
    const auto transforms = call<TransformCommands::Create>(*client, commands, {transform_storage, matrices}).transforms;
    const auto shared_transform = transforms.front();
    auto left_transform = transforms.back();
    if (transform_growth)
    {
        // Reserve enough independent records to cross a native Transform page without growing Mesh.
        std::vector<TransformMatrix> second_page(kMeshInstancePageSlots + 1, base_matrices[0]);
        const auto expanded = call<TransformCommands::Create>(
            *client, commands, {transform_storage, second_page}
        ).transforms;
        call<TransformCommands::Release>(*client, commands, left_transform);
        left_transform = expanded.back();
    }

    auto extra = values[0];
    extra.transform_id = shared_transform;
    const auto attached_extra = call<InstanceCommands::CreateInstance>(*client, commands, {instances, extra}).instance;
    CHECK(call<TransformCommands::Release>(*client, commands, shared_transform, false).error.type == kDeviceBusy);
    call<InstanceCommands::ReleaseInstance>(*client, commands, {attached_extra});

    auto shared_values = values;
    for (auto& value : shared_values) value.transform_id = shared_transform;
    const std::array share_pose{
        MeshInstanceUpdate{ids[0], shared_values[0]}, MeshInstanceUpdate{ids[1], shared_values[1]}
    };
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{share_pose});
    CHECK(run(0, true, false) == hidden_pixels);
    const auto foreign_scene = call<CreateScene>(*client, commands, {}).scene;
    const auto foreign_storage = call<GetOrCreateResource>(
        *client, commands, {session, foreign_scene, kTransformResourceName}
    ).resource_id;
    const auto foreign = call<TransformCommands::Create>(*client, commands, {foreign_storage, matrices}).transforms[0];
    const std::array invalid_updates{
        TransformRecordUpdate{shared_transform, base_matrices[0]}, TransformRecordUpdate{foreign, displaced}
    };
    CHECK(call<TransformCommands::Update>(
        *client, commands, {transform_storage, invalid_updates}, false
    ).error.type == kStorageIdentity);
    auto invalid_bind = share_pose;
    invalid_bind.back().description.transform_id = foreign;
    CHECK(updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{invalid_bind}, false)
        .error.type == kStorageIdentity);
    call<RemoveScene>(*client, commands, {foreign_scene});
    CHECK(run(0, true, false) == hidden_pixels);

    const std::array move_shared{TransformRecordUpdate{shared_transform, base_matrices[0]}};
    call<TransformCommands::Update>(*client, commands, {transform_storage, move_shared});
    CHECK(run(0, true, false) != hidden_pixels);
    const std::array hide_shared{TransformRecordUpdate{shared_transform, displaced}};
    in_frame_transform_records = hide_shared;
    CHECK(run(0, true, false) == hidden_pixels);
    in_frame_transform_records = {};

    shared_values[0].transform_id = left_transform;
    const std::array rebind_left{MeshInstanceUpdate{ids[0], shared_values[0]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{rebind_left});
    CHECK(call<TransformCommands::Release>(*client, commands, shared_transform, false).error.type == kDeviceBusy);
    const std::array restore_right{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restore_right});
    call<TransformCommands::Release>(*client, commands, shared_transform);
    const auto replacement = call<TransformCommands::Create>(
        *client, commands, {transform_storage, std::span{&displaced, 1}}
    ).transforms.front();
    CHECK(replacement != shared_transform);
    CHECK(call<TransformCommands::Update>(
        *client, commands, {transform_storage, move_shared}, false
    ).error.type == kStorageIdentity);
    call<TransformCommands::Release>(*client, commands, replacement);
    CHECK(run(0) == all_candidates);
    if (transform_growth)
        std::puts("TRANSFORM GPU GROWTH PASS: sampled a second-page matrix from an existing Mesh slot");
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{shown}.first(1));
    call<TransformCommands::Release>(*client, commands, left_transform);
    std::puts("TRANSFORM RESOURCE PASS: shared record, Command and FrameData, rebind, ownership and stale identity");
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
    CHECK(rejected.error.type);
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
    CHECK(!light_recovery.error.type);
    // Update only one light, preserve identities and repair all FIF replicas before subsequent Draws.
    std::get<PointLightGpu>(lights[1]).position_range = {0, 0, 30, .2F};
    const std::array changes{DirectLightUpdate{light_ids[1], lights[1]}};
    const auto changed_lights =
        updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{changes});
    CHECK(changed_lights.error.type == 0);
    run(1, true, true, false, true);
    rdesc::TextureInfo texture_info{};
    texture_info.width = texture_info.height = texture_info.mip_count = 1;
    texture_info.channel = 4;
    texture_info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
    texture_info.color_space = rdesc::ETextureColorSpace::DATA;
    texture_info.mip_ranges[0] = {0, 4, 1, 1};
    const std::array<std::byte, 4> normal_bytes{std::byte{204}, std::byte{128}, std::byte{230}, std::byte{255}};
    auto normal_texture =
        call<TextureCommands::Upload>(*client, commands, {texture_storage, {texture_info, normal_bytes}}).texture;
    TextureId alternate_normal;
    if (material_pages)
    {
        const std::array neutral{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}};
        alternate_normal = call<TextureCommands::Upload>(
            *client, commands, {texture_storage, {texture_info, neutral}}
        ).texture;
    }
    pbr = normal_mapped;
    const auto normal_mapped_id = registerProgram(pbr);
    right_material = createInstance(normal_mapped_id, right_values, {normal_texture});
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
    const auto no_uv_program = cook(shader_target, true, false, true, false, true, true);
    const auto& no_uv_inputs = no_uv_program.find(rdesc::EMaterialPurpose::GBUFFER)->shader.interface.entry_points;
    for (const auto& entry : no_uv_inputs)
    {
        const auto is_uv = [shader_target](const rdesc::ShaderIoVariable& input) noexcept
        {
            return shader_target == rdesc::EShaderTarget::DXIL
                ? input.semantic == "TEXCOORD" && input.semantic_index == 3 : input.location == 3;
        };
        CHECK(std::ranges::none_of(entry.inputs, is_uv));
    }
    const auto buffers_before = call_capture_placed_buffers.load();
    const auto no_uv_material = createInstance(registerProgram(no_uv_program), right_values, {normal_texture});
    if (material_pages && call_capture_core)
    {
        CHECK(!pbr.hasSameStructure(no_uv_program) && pbr.parameters == no_uv_program.parameters);
        CHECK(call_capture_placed_buffers.load() == buffers_before);
        std::puts("MATERIAL ABI PASS: distinct programs with the same ABI shared an existing FIF page");

        // The opaque Shader leaves the opacity parameter unused. A shorter projection must
        // nevertheless use its own page, even with the same block shape and ABI hash bucket.
        auto shorter = pbr;
        shorter.parameter_defaults.pop_back();
        auto shorter_values = right_values;
        shorter_values.pop_back();
        const auto separate = createInstance(registerProgram(std::move(shorter)), std::move(shorter_values), {normal_texture});
        CHECK(call_capture_placed_buffers.load() == buffers_before + fif);
        call<MaterialCommands::ReleaseInstance>(*client, commands, {separate});
        std::puts("MATERIAL PROJECTION PASS: same stride and hash bucket kept distinct ABI pages");
    }
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
        pbr = cook(shader_target, true, false, true, false, two_sided);
        pbr_id = registerProgram(pbr);
        right_material = createInstance(pbr_id, right_values, {normal_texture});
        if (material_pages)
        {
            // The same program draws records in different physical pages in one group.
            for (unsigned i = 0; i < 49; ++i)
                all_materials.push_back(createInstance(pbr_id, right_values, {normal_texture}));
        }
        const auto other_material = createInstance(pbr_id, left_values, {normal_texture});
        all_materials.insert(all_materials.end(), {right_material, other_material});
        values[1].material = right_material;
        const std::array changed{MeshInstanceUpdate{ids[1], values[1]}};
        updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{changed});
        std::array<MeshInstanceData, 4> additions;
        std::array addition_matrices{kIdentityTransform, kIdentityTransform, kIdentityTransform, kIdentityTransform};
        for (unsigned i = 0; i < additions.size(); ++i)
        {
            additions[i] = {right_mesh, other_material};
            additions[i].render_enabled = true;
            addition_matrices[i][0] = i == 1 ? -.3F : .3F;
            addition_matrices[i][5] = .25F;
            addition_matrices[i][12] = .5F;
            addition_matrices[i][13] = i == 0 ? .5F : -.5F;
            addition_matrices[i][14] = -.1F;
        }
        addition_matrices[2][12] = 4;
        addition_matrices[3][14] = 2;
        const auto extra_transforms = bindTransforms(*client, commands, transform_storage, additions, addition_matrices);
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
        if (material_pages)
        {
            const auto switched = createInstance(pbr_id, left_values, {alternate_normal});
            auto replacement = additions[0];
            replacement.material = switched;
            const std::array changed_texture{MeshInstanceUpdate{extra_instances[0], replacement}};
            updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{changed_texture});
            const auto switched_pixels = run(0, true, false);
            CHECK(switched_pixels != reference);
            const auto pipelines = call_capture_counts[2].load();
            auto dead_key = extension_detail::ResourceIdentity::local(alternate_normal);
            ++dead_key.gen;
            const auto dead_texture = extension_detail::ResourceIdentity::make<TextureId>(
                extension_detail::ResourceIdentity::parent(alternate_normal), dead_key
            );
            const auto rejected = call<MaterialCommands::Update>(
                *client, commands, {{switched, right_values, {dead_texture}}}, false
            );
            CHECK(rejected.error.type == kStorageIdentity);
            CHECK(run(0, true, false) == switched_pixels);
            call<MaterialCommands::Update>(*client, commands, {{switched, left_values, {normal_texture}}});
            CHECK(run(0, true, false) == reference);
            call<MaterialCommands::Update>(*client, commands, {{switched, right_values, {normal_texture}}});
            CHECK(run(0, true, false) != reference);
            call<MaterialCommands::Update>(*client, commands, {{switched, left_values, {normal_texture}}});
            CHECK(run(0, true, false) == reference);
            if (program_cache_probe) CHECK(call_capture_counts[2].load() == pipelines);
            const std::array restored{MeshInstanceUpdate{extra_instances[0], additions[0]}};
            updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored});
            call<MaterialCommands::ReleaseInstance>(*client, commands, {switched});
            std::puts("MATERIAL UPDATE PASS: member, texture and parameter changes reached GPU pixels");
        }
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
        const std::array restore{
            TransformRecordUpdate{extra_transforms[0], addition_matrices[0]},
            TransformRecordUpdate{extra_transforms[1], addition_matrices[1]}
        };
        const MeshInstanceUpdate combined{extra_instances[0], additions[0]};
        addition_matrices[0][0] *= -1;
        addition_matrices[1][0] *= -1;
        const std::array flips{
            TransformRecordUpdate{extra_transforms[0], addition_matrices[0]},
            TransformRecordUpdate{extra_transforms[1], addition_matrices[1]}
        };
        in_frame_transforms = flips;
        CHECK(run(0, true, false, true) == culled);
        following_transforms = restore;
        combined_instance = &combined;
        CHECK(run(0, true, false) == culled);
        following_transforms = {};
        combined_instance = nullptr;
        std::puts("FRAME ADOPTION PASS: packet before later command, duplicate credit rejected, disjoint columns adopted");
        const auto changed_pixels = run(0, true, false);
        in_frame_transforms = {};
        CHECK(changed_pixels == run(0, false, false));
        CHECK(changed_pixels != culled);
        std::printf("GPU MESH PASS views=2 FIF=%u indirect_instances=1/2,0/0 transform_packet=1\n", fif);
        for (auto instance : extra_instances)
        {
            call<InstanceCommands::ReleaseInstance>(
                *client,
                commands,
                {instance}
            );
        }
        for (auto transform : extra_transforms) call<TransformCommands::Release>(*client, commands, transform);
        mixed_winding = false;
    }
    ids[0] = call<InstanceCommands::CreateInstances>(
        *client, commands, {instances, std::span<const MeshInstanceData>{&values[0], 1}}
    ).instances.front();
    pbr = normal_mapped;
    right_material = createInstance(normal_mapped_id, right_values, {normal_texture});
    all_materials.push_back(right_material);
    values[1].material = right_material;
    const std::array restored{MeshInstanceUpdate{ids[1], values[1]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored});
    // Resources may upload successfully before a particular Effect prepares their program.
    // A material without GBuffer output fails that visible frame, retaining the old cache entries.
    // Derive the actual requested purpose so unused author parameters/textures are normalized.
    auto incompatible_program = cook(shader_target, true, false, true, true);
    const auto incompatible_material = createInstance(registerProgram(std::move(incompatible_program)), {});
    const auto invalid_view = call<CreateView>(*client, commands, {scene}).view;
    const auto invalid_target = call<CreateTarget>(*client, commands, {session, target}).target;
    const auto initial_camera = camera;
    CreateEffectRequest invalid_effect_request;
    invalid_effect_request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit invalid_edit{*invalid_effect_request.graph};
    const auto invalid_nodes = addDeferredNodes(invalid_edit, true, "");
    CHECK(invalid_edit.commit());
    const auto invalid_effect = call<CreateEffect>(*client, commands, std::move(invalid_effect_request)).effect;
    auto invalid_binding = mesh_binding();
    for (std::uint32_t i = 0; i < Lighting::light_bindings.size(); ++i)
        invalid_binding.resources.push_back({std::string{Lighting::light_bindings[i]}, light_storage, i});
    invalid_binding.view_parameters = set_view({scene, invalid_view}, initial_camera);
    const auto invalid_attachment = startRenderJob(
        *client, commands, session, scene, invalid_view, invalid_target, invalid_effect, invalid_binding
    );
    // One Effect shares exposure while two Views keep independent cameras; neither update recompiles the Plan.
    const auto shared_view = call<CreateView>(*client, commands, {scene}).view;
    const auto shared_target = call<CreateTarget>(*client, commands, {session, target}).target;
    auto other_camera = initial_camera;
    other_camera.background = {.1F, .2F, .3F, 1};
    invalid_binding.view_parameters = set_view({scene, shared_view}, other_camera);
    const auto shared_attachment = startRenderJob(
        *client, commands, session, scene, shared_view, shared_target, invalid_effect, invalid_binding
    );
    const auto compiled_before_sources = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
    call<SetToneMappingExposure>(*client, commands, {invalid_effect, invalid_nodes.tone, -1.F});
    other_camera.position_near = {2, 0, 0, .1F};
    set_view({scene, shared_view}, other_camera);
    renderFrame(*client, commands, invalid_attachment);
    renderFrame(*client, commands, shared_attachment);
    CHECK(call<QueryRenderStats>(*client, commands, {}).compiled_graphs == compiled_before_sources);
    stopAndRemoveJob(*client, commands, shared_attachment);
    call<RemoveTarget>(*client, commands, shared_target);
    call<RemoveView>(*client, commands, {scene, shared_view});
    auto incompatible_instance = values[1];
    incompatible_instance.material = incompatible_material;
    const std::array invalid_update{MeshInstanceUpdate{ids[1], incompatible_instance}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{invalid_update});
    const auto rejected_frame = call<RequestRenderJob>(*client, commands, {invalid_attachment}, false);
    CHECK(rejected_frame.error.type == kGraphInvalidDescriptor);
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{restored});
    stopAndRemoveJob(*client, commands, invalid_attachment);
    call<RemoveEffect>(*client, commands, {invalid_effect});
    call<RemoveView>(*client, commands, {scene, invalid_view});
    call<RemoveTarget>(*client, commands, invalid_target);
    call<MaterialCommands::ReleaseInstance>(*client, commands, {incompatible_material});
    unlit = masked;
    auto masked_values = left_values;
    masked_values[2][0] = 0;
    left_material = createInstance(registerProgram(unlit), masked_values);
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array mask_update{MeshInstanceUpdate{ids[0], values[0]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{mask_update});
    expected_left = {0, 0, 0};
    run(0);
    const auto check_shadow = [&](float expected)
    {
        CapturedPixels pixels;
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto raster = take(edit.addNode(MeshRaster::definition(), {
            std::string{MeshRaster::depth_only_clockwise}, MeshRaster::version
        }));
        const auto capture = take(edit.addNode(DepthCoverageCapture::definition(),
            {std::string{DepthCoverageCapture::name}, 1}));
        CHECK(edit.connect(edit.pinId(raster, MeshRaster::depth), edit.pinId(capture, MeshRaster::depth)));
        CHECK(edit.setOutput("depth", edit.pinId(capture, DepthCoverageCapture::pixels)));
        CHECK(edit.commit());
        auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
        auto view = call<CreateView>(*client, commands, {scene}).view;
        auto output = call<CreateTarget>(*client, commands, {session, target}).target;
        auto binding = mesh_binding();
        binding.view_parameters = set_view({scene, view}, camera);
        const auto readback = GraphCaptureBuffer::bind(
            *client, commands, session, {scene, view}, {&pixels, rdesc::ETextureFormat::D32_SFLOAT}
        );
        binding.resources.push_back({std::string{DepthCoverageCapture::name}, readback});
        auto attachment = startRenderJob(*client, commands, session, scene, view, output, effect, std::move(binding));
        auto before = call<QueryRenderStats>(*client, commands, {}).submissions;
        auto deadline = Clock::now() + 10s;
        while (call<QueryRenderStats>(*client, commands, {}).submissions < before + 20)
        {
            renderFrame(*client, commands, attachment);
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        stopAndRemoveJob(*client, commands, attachment);
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
        call<RemoveTarget>(*client, commands, output);
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
    unlit = cook(shader_target, true, false, false, true);
    CHECK(unlit.parameter_defaults.empty() && unlit.texture_count == 0);
    const auto constant_buffers = call_capture_placed_buffers.load();
    left_material = createInstance(registerProgram(unlit), {});
    if (material_pages && call_capture_core)
        CHECK(call_capture_placed_buffers.load() == constant_buffers);
    all_materials.push_back(left_material);
    values[0].material = left_material;
    const std::array shadow_update{MeshInstanceUpdate{ids[0], values[0]}};
    updateSceneData(*client, commands, instance_updates, std::span<const MeshInstanceUpdate>{shadow_update});
    check_shadow(.5F);
    call<RemoveScene>(*client, commands, {scene});
    for (auto material : all_materials)
    {
        call<MaterialCommands::ReleaseInstance>(
            *client,
            commands,
            {material}
        );
    }
    for (const auto program : registered) call<MaterialCommands::UnregisterProgram>(*client, commands, {program});
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
    if (!alternate_normal.isNull()) call<TextureCommands::Release>(*client, commands, {alternate_normal});
    server->stop();
    thread.join();
    endCallCapture();
    const auto stats = server->stats();
    if (const auto development = server->developmentStats())
    {
        std::printf(
            "RUN_COUNTERS rounds=%llu slot_cpu_hold_ns=%llu program_preparations_holding_slot=%llu "
            "program_preparation_ns=%llu\n",
            static_cast<unsigned long long>(development->rounds_opened),
            static_cast<unsigned long long>(development->slot_cpu_hold_ns),
            static_cast<unsigned long long>(development->program_preparations_holding_slot),
            static_cast<unsigned long long>(development->program_preparation_ns)
        );
    }
    CHECK(stats.validation_errors == 0);
    server.reset();
    if (material_pages)
    {
        RenderServerConfig constant_config{"test.material.constant", config.validation};
        DeviceOptions constant_device;
        constant_device.backend = device.backend;
        auto constant_server = take(RenderServer::create(constant_config, **runtime));
        auto connection = ipc::IpcCore::instance().createRequester(constant_config.channel_name);
        CHECK(connection);
        auto& requester = *connection;
        std::thread worker([&] { CHECK(constant_server->run()); });
        const auto directory = queryCommands(requester);
        const auto session = call<CreateDeviceSession>(requester, directory, constant_device).device;
        const auto resource = call<GetOrCreateResource>(
            requester, directory, {session, DeviceScope{}, kMaterialResourceName}
        ).resource_id;
        const auto program = registerMaterialProgram(requester, directory, resource, unlit);
        const auto constant = createMaterialInstance(requester, directory, program);
        CHECK(!constant.isNull());
        call<MaterialCommands::ReleaseInstance>(requester, directory, {constant});
        const auto reused = createMaterialInstance(requester, directory, program);
        const auto before = extension_detail::ResourceIdentity::local(constant);
        const auto after = extension_detail::ResourceIdentity::local(reused);
        CHECK(before.index == after.index && before.gen != after.gen);
        const auto stale = call<MaterialCommands::Update>(requester, directory, {{constant, {}, {}}}, false);
        CHECK(stale.error.type == kStorageIdentity);
        call<MaterialCommands::ReleaseInstance>(requester, directory, {reused});
        call<MaterialCommands::UnregisterProgram>(requester, directory, {program});
        constant_server->stop();
        worker.join();
        CHECK(constant_server->stats().validation_errors == 0);
        std::puts("MATERIAL CONSTANT PASS: no Uploader, valid sentinel program and stale instance generation rejected");
    }
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
