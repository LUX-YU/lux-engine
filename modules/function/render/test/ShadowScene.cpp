#include <lux/engine/render/runtime/commands/ViewParameterCommands.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include "MaterialFixture.hpp"
#include "FullscreenVertex.hpp"
#include "GraphShadowSampler.hpp"
#include "ClusterCompare.hpp"
#include "ShadowReadback.hpp"
#include "SceneCapture.hpp"
#include "SceneComparison.hpp"
#include <lux/engine/render/effect/builtin/ClusterLists.hpp>
#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>
#include <lux/engine/render/effect/builtin/Shadow.hpp>
#include <cmath>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/commands/MeshCommands.hpp>
#include <lux/engine/render/runtime/commands/InstanceCommands.hpp>
#include <lux/engine/render/resource/mesh/Binding.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>

struct SamplerProbe final
{
    static constexpr std::string_view name = "test.graph.sampler_probe";
    static constexpr auto color = effectPortSemantic("color");

    static EffectNodeDefinition definition()
    {
        return {
            .identity = {lux::graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {{color, "color", {std::string{ToneMapping::display_type}, 1}, lux::graph::EPinDirection::OUTPUT}}
        };
    }

    static void registerWith(RenderServer& server)
    {
        const auto capability = definition();
        CHECK(server.registerEffectCapability(capability));
        EffectImplementation implementation;
        implementation.capability = capability.identity.id;
        implementation.identity = {std::string{name}, 1};
        implementation.supported_outputs = {color};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
            -> RenderResult<void> { return {}; };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

private:
    static RenderResult<void> build(EffectCompileContext& context, std::string_view config) noexcept
    {
        GraphImageDesc color_image{64, 64, 1, 1, context.target().format};
        color_image.extent.source = EImageExtentSource::TARGET;
        const auto output = take(context.declareResource({"sampler output", color_image}));
        const auto depths = take(context.declareResource(
            {"sampler depths", GraphImageDesc{64, 64, 1, 2, rdesc::ETextureFormat::D32_SFLOAT}}
        ));
        auto depth = ResourceVersion{depths, 0};
        // A dead declaration precedes both live inputs and consumes no compiled sampler slot.
        (void)context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS});
        for (std::uint32_t layer = 0; layer < 2; ++layer)
        {
            const auto cleared = context.addTransferPass(
                "clear sampler depth " + std::to_string(layer),
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
        fixed_work.interface.program = take(context.addProgram(std::move(shaders)));
        fixed_work.interface.resources = {{"depths", {depth}}};
        auto ordinary = context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE});
        auto comparison =
            context.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE, EDepthCompare::LESS_EQUAL});
        if (config == "undeclared") comparison = 32;
        if (config == "wrong comparison") comparison = ordinary;
        fixed_work.interface.samplers = {
            {"ordinary", {ordinary}},
            {"comparison", {comparison}, kGraphInvalidIndex, true}
        };
        work.colors.push_back({{{output, 0}}});
        const auto drawn = context.addRasterPass("sample shadow layers", std::move(work));
        if (!drawn)
        {
            return lux::cxx::unexpected(drawn.error());
        }
        return context.publish(color, {{color, drawn->written.front()}});
    }
};


static void verifySamplers(RenderServer& server, const RenderServerConfig& config, const DeviceOptions& device)
{
    SamplerProbe::registerWith(server);
    ColorCapture::registerWith(server);

    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread thread([&] { CHECK(server.run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, device).device;
    CHECK(call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, kTextureResourceName}, false)
        .error.type == kStorageCapability);
    CapturedPixels pixels;
    const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto probe = take(edit.addNode(SamplerProbe::definition(), {std::string{SamplerProbe::name}, 1}));
    const auto capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
    const auto color = edit.pinId(probe, SamplerProbe::color);
    CHECK(edit.connect(color, edit.pinId(capture, ColorCapture::color)));
    CHECK(edit.setOutput("color", color, true));
    CHECK(edit.setOutput("pixels", edit.pinId(capture, ColorCapture::pixels)));
    CHECK(edit.commit());
    const auto effect = createPreparedEffect(*client, commands, session, std::move(request));
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto output = call<CreateTarget>(*client, commands, {session, target}).target;
    for (const auto config_text : {"undeclared", "wrong comparison"})
    {
        CreateEffectRequest invalid;
        invalid.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit invalid_edit{*invalid.graph};
        const auto node = take(invalid_edit.addNode(
            SamplerProbe::definition(), {std::string{SamplerProbe::name}, 1}, config_text
        ));
        CHECK(invalid_edit.setOutput("color", invalid_edit.pinId(node, SamplerProbe::color), true));
        CHECK(invalid_edit.commit());
        const auto invalid_effect = call<CreateEffect>(*client, commands, std::move(invalid)).effect;
        const auto invalid_job = call<CreateRenderJob>(*client, commands, {scene, view, invalid_effect}).job;
        const auto before = server.stats().compiled_graphs;
        const auto rejected = call<PrepareEffect>(*client, commands, {invalid_effect, session}, false);
        CHECK(rejected.error.type == kGraphInvalidDescriptor && server.stats().compiled_graphs == before);
        call<RemoveRenderJob>(*client, commands, invalid_job);
        call<RemoveEffect>(*client, commands, {invalid_effect});
    }
    JobBinding binding;
    const auto readback = GraphCaptureBuffer::bind({*client, commands, session}, {scene, view}, {&pixels});
    binding.resources.push_back({std::string{ColorCapture::name}, readback});
    const auto attachment = startRenderJob(*client, commands, session, scene, view, output, effect, std::move(binding));
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

struct ShadowReadback final
{
    static constexpr std::string_view name = "test.shadow.readback";
    static constexpr auto readback = effectPortSemantic("readback");
    static constexpr auto reference = effectPortSemantic("reference");
    static constexpr auto candidate = effectPortSemantic("candidate");

    // Projection zero belongs to ColorCapture. Each remaining projection has its own FIF backings.
    static constexpr std::array<std::string_view, 6> bindings{
        "views", "shadow_commands", "surface_color", "surface_depth", "differences", "cluster_status"
    };
    static constexpr std::uint32_t maximum_views = 9;

    static EffectNodeDefinition definition()
    {
        using enum lux::graph::EPinDirection;
        return {
            .identity = {lux::graph::nodeTypeId(name), std::string{name}, 1},
            .ports = {
                {GBuffer::surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version}, INPUT, 1, true},
                {Shadow::shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version}, INPUT, 1, true},
                {Shadow::draws, "draws", {std::string{MeshVisibility::draws_type}, 1}, INPUT, 1, true},
                {reference, "reference", {std::string{HdrColorBundle::type}, HdrColorBundle::version}, INPUT, 1},
                {candidate, "candidate", {std::string{HdrColorBundle::type}, HdrColorBundle::version}, INPUT, 1},
                {Lighting::cluster_lists, "cluster_lists", {std::string{ClusterListBundle::type}, ClusterListBundle::version}, INPUT, 1},
                {readback, "readback", {"test.shadow.readback.bytes", 1}, OUTPUT}
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
        implementation.supported_outputs = {readback};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept
            -> RenderResult<void> { return {}; };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

private:
    static ResourceVersion member(EffectCompileContext& context, lux::graph::PinSemanticId pin,
        lux::graph::PinSemanticId semantic) noexcept
    {
        const auto input = context.inputs(pin);
        CHECK(input.size() == 1);
        const auto found = std::ranges::find(input.front(), semantic, &EffectResourceValue::member);
        CHECK(found != input.front().end());
        return found->version;
    }

    static RenderResult<void> build(EffectCompileContext& context, std::string_view) noexcept
    {
        const bool compare = !context.inputs(reference).empty();
        const std::uint32_t count = compare ? bindings.size() : 4;
        std::array<ResourceVersion, bindings.size()> sinks;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto buffer = context.importFinalBuffer(
                bindings[i], {ResourceName{GraphCaptureDescriptor::descriptor.type.name()}},
                {"", GraphBufferDesc{GraphCaptureBuffer::image_bytes}, EGraphOrigin::IMPORTED, {},
                    EGraphUse::TRANSFER_DESTINATION}, i + 1
            );
            if (!buffer) return lux::cxx::unexpected(buffer.error());
            sinks[i] = *buffer;
        }

        constexpr std::uint32_t views_bytes = maximum_views * sizeof(MeshVisibilityView);
        constexpr std::uint32_t commands_bytes = maximum_views * kMeshWindingCount * sizeof(nri::DrawIndexedDesc);
        static_assert(sizeof(MeshVisibilityView) == 44 * sizeof(std::uint32_t));
        static_assert(kMeshWindingCount * sizeof(nri::DrawIndexedDesc) == 10 * sizeof(std::uint32_t));
        const auto views = take(context.declareResource({"readback views", GraphBufferDesc{views_bytes}}));
        const auto commands = take(context.declareResource({"readback indirect", GraphBufferDesc{commands_bytes}}));
        ComputeWork snapshot;
        snapshot.interface.program = take(context.addProgram(GraphProgram{kShadowReadback.load(context.shaderTarget())}));
        snapshot.interface.resources = {
            {"shadow_views", {member(context, Shadow::shadows, effectPortSemantic(ShadowBundle::members[0]))}},
            {"shadow_commands", {member(context, Shadow::draws, MeshVisibility::commands)}},
            {"views", {{views, 0}, {}, EGraphAccess::WRITE}},
            {"commands", {{commands, 0}, {}, EGraphAccess::WRITE}}
        };
        constexpr auto threads_bytes = 64 * sizeof(std::uint32_t);
        snapshot.dispatch = DispatchArguments{{(std::max(views_bytes, commands_bytes) + threads_bytes - 1) /
            threads_bytes, 1, 1}};
        const auto sampled = take(context.addComputePass("sample shadow buffers", std::move(snapshot)));
        const auto copy_buffer = [&](std::uint32_t index, ResourceVersion source, std::uint32_t bytes)
        {
            const auto cleared = take(context.addTransferPass(
                "clear " + std::string{bindings[index]}, TransferWork{{FillBuffer{{sinks[index]}, 0}}}
            ));
            const auto copied = take(context.addTransferPass(
                "copy " + std::string{bindings[index]}, TransferWork{{CopyBuffer{
                    {source, GraphBufferRange{0, bytes}},
                    {cleared.written.front(), GraphBufferRange{0, bytes}, EGraphAccess::READ_WRITE}
                }}}
            ));
            sinks[index] = copied.written.front();
        };
        copy_buffer(0, sampled.written[0], views_bytes);
        copy_buffer(1, sampled.written[1], commands_bytes);

        for (std::uint32_t i : {2u, 3u})
        {
            const bool depth = i == 3;
            const auto semantic = effectPortSemantic(depth ? GBufferBundle::members.back() : GBufferBundle::members[0]);
            const auto source = member(context, GBuffer::surface, semantic);
            const auto copied = take(context.addTransferPass("copy " + std::string{bindings[i]},
                TransferWork{{CopyImageBuffer{
                    {source, GraphImageRange{depth ? 2u : 1u}}, {sinks[i]}, 256, GraphCaptureBuffer::image_bytes,
                    ImageCopyRegion{0, 0, 64, 64}
                }}}
            ));
            sinks[i] = copied.written.front();
        }
        if (compare)
        {
            const auto difference = take(context.declareResource(
                {"HDR differences", GraphBufferDesc{GraphCaptureBuffer::image_bytes}}
            ));
            ComputeWork comparison;
            comparison.interface.program = take(context.addProgram(GraphProgram{kClusterCompare.load(context.shaderTarget())}));
            comparison.interface.resources = {
                {"reference_image", {member(context, reference, Lighting::hdr)}},
                {"candidate_image", {member(context, candidate, Lighting::hdr)}},
                {"differences", {{difference, 0}, {}, EGraphAccess::WRITE}}
            };
            comparison.dispatch = DispatchArguments{{8, 8, 1}};
            const auto compared = take(context.addComputePass("compare shadow lighting HDR", std::move(comparison)));
            copy_buffer(4, compared.written.front(), GraphCaptureBuffer::image_bytes);
            copy_buffer(5, member(context, Lighting::cluster_lists, effectPortSemantic(ClusterListBundle::members.back())),
                sizeof(ClusterStatusGpu));
        }
        EffectResourceBundle outputs;
        for (std::uint32_t i = 0; i < count; ++i)
            outputs.push_back({effectPortSemantic(bindings[i]), sinks[i]});
        return context.publish(readback, std::move(outputs));
    }
};

static void runPresets(
    ipc::IpcRequester& client,
    const CommandList& commands,
    DeviceSessionId session,
    SceneId scene,
    ResourceInstanceId lights,
    const JobBinding& scene_binding,
    const TargetDesc& target,
    ViewGpuParameters camera,
    unsigned fif,
    std::uint32_t profile_frames
)
{
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
        const bool shadows = (variant & 1) != 0;
        const bool clustered = (variant & 2) != 0;
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto surface = take(edit.addNode(GBuffer::definition(), {std::string{GBuffer::indexed}, GBuffer::version}));
        const auto lighting_impl = clustered ? Lighting::clustered : Lighting::direct;
        const auto lighting = take(edit.addNode(Lighting::definition(), {std::string{lighting_impl}, Lighting::version},
            clustered ? "x=7 y=5 z=9 index-capacity=4096 depth=linear" : ""));
        const auto tone = take(edit.addNode(ToneMapping::definition(),
            {std::string{ToneMapping::linear}, ToneMapping::version}, "exposure=0"));
        CHECK(edit.connect(edit.pinId(surface, GBuffer::surface), edit.pinId(lighting, Lighting::surface)));
        CHECK(edit.connect(edit.pinId(lighting, Lighting::hdr), edit.pinId(tone, ToneMapping::hdr_color)));
        const auto display = edit.pinId(tone, ToneMapping::color);
        CHECK(edit.setOutput("color", display, true));
        if (shadows)
        {
            const auto shadow = take(edit.addNode(Shadow::definition(), {std::string{Shadow::indexed}, Shadow::version},
                "resolution=64 distance=3 pcf-radius=0"));
            CHECK(edit.connect(edit.pinId(shadow, Shadow::shadows), edit.pinId(lighting, Lighting::shadows)));
        }
        CapturedPixels pixels;
        if (!profile_frames)
        {
            const auto capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
            CHECK(edit.connect(display, edit.pinId(capture, ColorCapture::color)));
            CHECK(edit.setOutput("pixels", edit.pinId(capture, ColorCapture::pixels)));
        }
        CHECK(edit.commit());
        const auto effect = createPreparedEffect(client, commands, session, std::move(request));
        const auto view = call<CreateView>(client, commands, {scene}).view;
        const auto image = call<CreateTarget>(client, commands, {session, target}).target;
        const ViewScope view_scope{scene, view};
        JobBinding binding = scene_binding;
        const auto camera_resource = call<GetOrCreateResource>(client, commands,
            {session, view_scope, kViewParameterResourceName}).resource_id;
        binding.resources.push_back({
            std::string{ViewParameterResource::snapshot_binding}, camera_resource, 0, EJobResourceUse::CPU_SNAPSHOT
        });
        call<ViewParameterCommands::Update>(client, commands, {camera_resource, std::span{&camera, 1}});
        if (shadows)
            binding.resources.push_back({std::string{Shadow::lights_binding}, lights, 0, EJobResourceUse::CPU_SNAPSHOT});
        if (!profile_frames)
        {
            const auto readback = GraphCaptureBuffer::bind({client, commands, session}, view_scope, {&pixels, target.format});
            binding.resources.push_back({std::string{ColorCapture::name}, readback});
        }
        const auto attached = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
        if (variant == 0)
        {
            auto unsupported = target;
            unsupported.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
            const auto wrong_format = call<CreateTarget>(client, commands, {session, unsupported}).target;
            CHECK(call<StartRenderJob>(client, commands, {session, attached, wrong_format, binding}, false).error.type ==
                kDeviceArgument);
            call<RemoveTarget>(client, commands, wrong_format);
        }
        call<StartRenderJob>(client, commands, {session, attached, image, std::move(binding)});
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
    bool report_warnings = std::getenv("LUX_TEST_NATIVE_WARNINGS") != nullptr;
    config.diagnostics = {&report_warnings, [](void* context, const RenderDiagnostic& diagnostic) noexcept
    {
        const bool is_error = diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE;
        const bool is_requested_warning = diagnostic.severity == ERenderDiagnosticSeverity::WARNING &&
            *static_cast<const bool*>(context);
        if (is_error || is_requested_warning)
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
    ColorCapture::registerWith(*server);
    ShadowReadback::registerWith(*server);

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
    const auto instance_updates = instances;
    const auto light_updates = light_storage;

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
    std::array matrices{kIdentityTransform, kIdentityTransform, kIdentityTransform, kIdentityTransform};
    matrices[1][0] = -.25F; // Mirrored caster with nonuniform scale.
    matrices[1][5] = .2F;
    matrices[1][14] = .5F;
    matrices[2][12] = 100;
    matrices[3][0] = matrices[3][5] = .08F;
    matrices[3][12] = matrices[3][13] = .8F;
    matrices[3][14] = .2F;
    const auto transform_storage = sceneTransformStorage(*client, commands, session, scene);
    bindTransforms(*client, commands, transform_storage, descriptions, matrices);
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
    ViewGpuParameters camera;
    camera.view[10] = -1;
    camera.view[14] = 2;
    camera.projection[10] = 1 / 2.9F;
    camera.projection[14] = -.1F / 2.9F;
    camera.inverse_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2.9F, 0, 0, 0, 1.9F, 1};
    camera.position_near[2] = 2;
    camera.direction_far[3] = 3;
    camera.background = {0, 0, 0, 1};
    const auto transform_projection = device.buffer_device_address ?
        ETransformGraphProjection::PAGE_ADDRESSES : ETransformGraphProjection::RECORDS;
    const auto instance_projection = device.buffer_device_address ?
        EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
    JobBinding scene_binding;
    scene_binding.resources = {
        {std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT},
        {std::string{MeshVisibility::transforms_binding}, transform_storage, static_cast<std::uint32_t>(transform_projection)},
        {std::string{MeshVisibility::properties_binding}, instances, static_cast<std::uint32_t>(instance_projection)},
        {std::string{GBuffer::geometry_binding}, meshes, 0, EJobResourceUse::DRAW_RESOURCE},
        {std::string{GBuffer::materials_binding}, materials, 0, EJobResourceUse::DRAW_RESOURCE}
    };
    if (!device.buffer_device_address)
    {
        scene_binding.resources.push_back({
            std::string{MeshVisibility::alive_binding}, instances, static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)
        });
    }
    for (std::uint32_t i = 0; i < Lighting::light_bindings.size(); ++i)
        scene_binding.resources.push_back({std::string{Lighting::light_bindings[i]}, light_storage, i});
    if (presets || profile_frames)
    {
        runPresets(*client, commands, session, scene, light_storage, scene_binding, target, camera, fif, profile_frames);
    }
    const auto update_camera = [&](ResourceInstanceId resource, const ViewGpuParameters& input)
    {
        call<ViewParameterCommands::Update>(*client, commands, {resource, std::span{&input, 1}});
    };
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
        updateSceneData(*client, commands, light_updates, std::span<const DirectLightUpdate>{shadow_updates});
        CapturedPixels pixels, view_bytes, surface_pixels, depth_pixels;
        CapturedPixels indirect, differences, cluster_status;
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto surface = take(edit.addNode(
            GBuffer::definition(), {std::string{GBuffer::indexed_clockwise}, GBuffer::version}
        ));
        const auto shadow = take(edit.addNode(
            Shadow::definition(), {std::string{Shadow::indexed_clockwise}, Shadow::version},
            "resolution=64 distance=3 pcf-radius=" + std::to_string(radius) + (cull ? "" : " frustum-culling=false")
        ));
        const auto direct = take(edit.addNode(Lighting::definition(), {std::string{Lighting::direct}, Lighting::version}));
        const auto tone = take(edit.addNode(
            ToneMapping::definition(), {std::string{ToneMapping::linear}, ToneMapping::version}, "exposure=0"
        ));
        const auto color_capture = take(edit.addNode(ColorCapture::definition(), {std::string{ColorCapture::name}, 1}));
        const auto diagnostic = take(edit.addNode(ShadowReadback::definition(), {std::string{ShadowReadback::name}, 1}));
        const auto surface_port = edit.pinId(surface, GBuffer::surface);
        const auto shadows_port = edit.pinId(shadow, Shadow::shadows);
        const auto direct_hdr = edit.pinId(direct, Lighting::hdr);
        const auto display = edit.pinId(tone, ToneMapping::color);
        CHECK(edit.connect(surface_port, edit.pinId(direct, Lighting::surface)));
        CHECK(edit.connect(shadows_port, edit.pinId(direct, Lighting::shadows)));
        CHECK(edit.connect(direct_hdr, edit.pinId(tone, ToneMapping::hdr_color)));
        CHECK(edit.connect(display, edit.pinId(color_capture, ColorCapture::color)));
        CHECK(edit.connect(surface_port, edit.pinId(diagnostic, GBuffer::surface)));
        CHECK(edit.connect(shadows_port, edit.pinId(diagnostic, Shadow::shadows)));
        CHECK(edit.connect(edit.pinId(shadow, Shadow::draws), edit.pinId(diagnostic, Shadow::draws)));
        if (clustered)
        {
            const auto candidate = take(edit.addNode(
                Lighting::definition(), {std::string{Lighting::clustered}, Lighting::version},
                radius ? "x=7 y=5 z=9 index-capacity=1 depth=logarithmic" : "x=7 y=5 z=9 index-capacity=65536 depth=logarithmic"
            ));
            CHECK(edit.connect(surface_port, edit.pinId(candidate, Lighting::surface)));
            CHECK(edit.connect(shadows_port, edit.pinId(candidate, Lighting::shadows)));
            CHECK(edit.connect(direct_hdr, edit.pinId(diagnostic, ShadowReadback::reference)));
            CHECK(edit.connect(edit.pinId(candidate, Lighting::hdr), edit.pinId(diagnostic, ShadowReadback::candidate)));
            CHECK(edit.connect(
                edit.pinId(candidate, Lighting::cluster_lists), edit.pinId(diagnostic, Lighting::cluster_lists)
            ));
        }
        CHECK(edit.setOutput("color", display, true));
        CHECK(edit.setOutput("pixels", edit.pinId(color_capture, ColorCapture::pixels)));
        CHECK(edit.setOutput("diagnostics", edit.pinId(diagnostic, ShadowReadback::readback)));
        CHECK(edit.commit());
        auto effect = createPreparedEffect(*client, commands, session, std::move(request));
        const auto make_binding = [&](ViewId view, const ViewGpuParameters& input)
        {
            const ViewScope scope{scene, view};
            auto binding = scene_binding;
            const auto camera_resource = call<GetOrCreateResource>(
                *client, commands, {session, scope, kViewParameterResourceName}
            ).resource_id;
            binding.resources.push_back({
                std::string{ViewParameterResource::snapshot_binding}, camera_resource, 0, EJobResourceUse::CPU_SNAPSHOT
            });
            update_camera(camera_resource, input);
            const std::array configurations{
                CaptureConfig{&pixels}, CaptureConfig{&view_bytes}, CaptureConfig{&indirect},
                CaptureConfig{&surface_pixels}, CaptureConfig{&depth_pixels, rdesc::ETextureFormat::D32_SFLOAT},
                CaptureConfig{&differences}, CaptureConfig{&cluster_status}
            };
            const auto capture_count = clustered ? configurations.size() : configurations.size() - 2;
            const auto readback = GraphCaptureBuffer::bind(
                {*client, commands, session}, scope, std::span{configurations}.first(capture_count)
            );
            binding.resources.push_back({std::string{Shadow::lights_binding}, light_storage, 0, EJobResourceUse::CPU_SNAPSHOT});
            binding.resources.push_back({std::string{ColorCapture::name}, readback});
            for (std::uint32_t i = 0; i < capture_count - 1; ++i)
                binding.resources.push_back({std::string{ShadowReadback::bindings[i]}, readback, i + 1});
            return binding;
        };
        const auto view = call<CreateView>(*client, commands, {scene}).view;
        const auto output = call<CreateTarget>(*client, commands, {session, target}).target;
        const auto binding = make_binding(view, camera);
        const auto camera_resource = call<QueryResource>(*client, commands, {
            session, ViewScope{scene, view}, kViewParameterResourceName
        }).resource_id;
        const auto attachment = startRenderJob(*client, commands, session, scene, view, output, effect, binding);
        const auto second_view = call<CreateView>(*client, commands, {scene}).view;
        const auto second_target = call<CreateTarget>(*client, commands, {session, target}).target;
        auto second_camera = camera;
        second_camera.projection[0] = 1.02F;
        second_camera.inverse_view_projection[0] = 1.F / second_camera.projection[0];
        const auto second = startRenderJob(
            *client, commands, session, scene, second_view, second_target, effect, make_binding(second_view, second_camera)
        );
        const auto compile_count = call<QueryRenderStats>(*client, commands, {}).compiled_graphs;
        auto moving = camera;
        moving.position_near[0] = -.2F;
        moving.view[12] = .2F;
        moving.inverse_view_projection[12] = -.2F;
        update_camera(camera_resource, moving);
        update_camera(camera_resource, camera);
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
            // Alternating two Jobs can keep each on a subset of the device's frame slots.
            // Overwrite every replica this Job could have written while the light was moved.
            std::vector<std::uint8_t> restored(fif);
            auto remaining = fif;
            const auto restore_deadline = Clock::now() + 10s;
            while (remaining)
            {
                const auto frame = call<RequestRenderJob>(*client, commands, {attachment});
                CHECK(frame.job == attachment && frame.slot_index < restored.size() && Clock::now() < restore_deadline);
                auto& refreshed = restored[frame.slot_index];
                if (!refreshed)
                {
                    refreshed = 1;
                    --remaining;
                }
            }
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
                update_camera(camera_resource, moved_camera ? moving : camera);
                for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, attachment);
                CHECK(copy_bytes() > before + stable_bytes);
                std::printf("PROJECTION END %s\n", name);
                std::fflush(stdout);
            }
            // This Job's last capture preceded the dense light removal.
            for (unsigned frame = 0; frame < fif; ++frame) renderFrame(*client, commands, second);
        }
        stopAndRemoveJob(*client, commands, attachment);
        stopAndRemoveJob(*client, commands, second);
        call<RemoveView>(*client, commands, {scene, view});
        call<RemoveView>(*client, commands, {scene, second_view});
        while (pixels.ready.load(std::memory_order_acquire) != 2 ||
               view_bytes.ready.load(std::memory_order_acquire) != 2 ||
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
        for (const auto& frame : indirect.frames)
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
        call<RemoveTarget>(*client, commands, output);
        call<RemoveTarget>(*client, commands, second_target);
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
