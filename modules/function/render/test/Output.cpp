#include "NativeDevice.hpp"
#include "SceneCapture.hpp"
#include <lux/engine/render/features/mesh/HzbFeature.hpp>
#include <lux/engine/render/features/postprocess/TonemapFeature.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <Extensions/NRIWrapperD3D12.h>
#include <Extensions/NRIWrapperVK.h>
#include <future>
#include <vulkan/vulkan.h>
#pragma comment(lib, "user32.lib")
#endif
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include "FrameHarness.hpp"
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <optional>
#include <string>
#include <string_view>
using namespace foundation_test;

static void copyBuffer(DeviceSession& session, nri::Buffer& source, nri::Buffer& output, std::uint64_t bytes)
{
    CHECK(submitFixtureCommands(session, session.queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            const nri::BufferBarrierDesc source_ready{
                &source,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
            };
            session.nriCore().CmdBarrier(command, {nullptr, 0, &source_ready, 1});
            session.nriCore().CmdCopyBuffer(command, output, 0, source, 0, bytes);
            return {};
        }
    ));
}

static void graphOutput(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 3);
    const TargetDesc target{16, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{4096}}));
    const auto copied = take(builder.declareResource({"copied", GraphBufferDesc{4096}}));
    auto clear = take(builder.addTransferPass("clear", TransferWork{{ClearImage{{{color, 0}}, {1, 0, 1, 1}}}}));
    const auto initialized =
        take(builder.addTransferPass("initialize copy padding", TransferWork{{FillBuffer{{{pixels, 0}}, 0xababababu}}})
        );
    auto copy = take(builder.addTransferPass(
        "copy",
        TransferWork{{CopyImageBuffer{
            {clear.written.front()},
            {initialized.written.front(), {}, EGraphAccess::READ_WRITE},
            256,
            4096
        }}}
    ));
    auto copied_pass =
        take(builder.addTransferPass("copy-buffer", TransferWork{{CopyBuffer{{copy.written.front()}, {{copied, 0}}}}}));
    CHECK(builder.exportOutput({clear.written.front(), {}, GraphOutputRequirements{target.format, target.samples}}));
    CHECK(builder.exportOutput({copy.written.front()}));
    CHECK(builder.exportOutput({copied_pass.written.front()}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(definition));
    const EffectOutputDesc output_description{target.format, target.samples, target.kind, target.sampled};
    auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), definition, analysis, &output_description
    ));
    CHECK(ready_result);
    auto graph = take(std::move(std::get<0>(*ready_result)));
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, frames->frameCount()))};
    auto execution = std::make_unique<RenderGraphInstance>(*session, frames->frameCount(), graph, physical->output());
    CHECK(execution->initialize());
    auto incompatible_output = physical->output();
    incompatible_output.description.format = lux::rdesc::ETextureFormat::BGRA8_UNORM;
    auto rejected_resize = execution->prepareResize(incompatible_output, {});
    CHECK(!rejected_resize && rejected_resize.error().type == kDeviceArgument);
    auto missing_output = execution->prepareResize({});
    CHECK(!missing_output && missing_output.error().type == kDeviceArgument);
    CHECK(execution->target() == target); // Rejection must not withdraw the adopted output.
    const std::array contexts{execution.get()};
    std::vector<StorageFrameUse> storage_preparation;
    nri::Buffer* output = nullptr;
    const nri::BufferDesc description{4096, 0, nri::BufferUsageBits::NONE};
    CHECK(
        session->nriCore().CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, output) ==
        nri::Result::SUCCESS
    );
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 0);
    CHECK(session->drain());
    for (auto id : {pixels, copied})
    {
        copyBuffer(
            *session,
            *execution->nativeBuffer(id.index, execution->progress().last_submitted_slot),
            *output,
            4096
        );
        CHECK(session->drain());
        auto* mapped = static_cast<const std::byte*>(session->nriCore().MapBuffer(*output, 0, 4096));
        CHECK(mapped);
        CHECK(session->invalidateMappedRange(*output, 0, 4096) == nri::Result::SUCCESS);
        for (std::size_t row = 0; row < 16; ++row)
        {
            for (std::size_t column = 0; column < 16; ++column)
            {
                const auto i = row * 256 + column * 4;
                CHECK(mapped[i] == std::byte{255} && mapped[i + 1] == std::byte{0});
                CHECK(mapped[i + 2] == std::byte{255} && mapped[i + 3] == std::byte{255});
            }
        }
        session->nriCore().UnmapBuffer(*output);
    }
    auto prior_uses = session->makeFrontiers();
    physical->appendUses(prior_uses);
    CHECK(take(runTask(frames->render(contexts, storage_preparation))) == 1);
    auto last_uses = session->makeFrontiers();
    physical->appendUses(last_uses);
    CHECK(last_uses != prior_uses);
    execution.reset(); // Physical resources outlive the business Context through Session cleanup.
    physical.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(output);
    std::puts("GPU RETIREMENT PASS: readback pixels and Offscreen release use accepted frontiers");
}

static void importedIdentityReuse(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 1);
    RenderGraphBuilder builder;
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    RasterWork work;
    work.program = RasterPassTemplate{"identity regression"};
    work.colors = {{{{color, 0}}}};
    const auto pass = take(builder.addRasterPass("clear with dynamic reads", std::move(work)));
    CHECK(builder.exportOutput({pass.written.front()}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
        *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis, nullptr
    ));
    CHECK(ready_result);
    auto plan = take(std::move(std::get<0>(*ready_result)));
    std::string dump;
    const DumpSink sink{&dump, [](void* destination, std::string_view text) noexcept
    {
        static_cast<std::string*>(destination)->append(text);
        return true;
    }};
    const auto number = [&dump](std::string_view key)
    {
        const auto found = dump.find(key);
        CHECK(found != std::string::npos);
        std::uint64_t value = 0;
        CHECK(std::from_chars(dump.data() + found + key.size(), dump.data() + dump.size(), value).ec ==
              std::errc{});
        return value;
    };
    const auto revision = [&](const RenderGraphInstance& instance)
    {
        dump.clear();
        CHECK(instance.dumpResources(sink));
        return number("\"imported_revision\":");
    };
    const auto rebuilds = [&]()
    {
        dump.clear();
        CHECK(frames->dumpSubmissions(sink));
        return number("\"dependency_rebuilds\":");
    };
    std::optional<RenderGraphInstance> instance;
    instance.emplace(*session, 1, plan);
    CHECK(instance->initialize());
    const auto initial_revision = revision(*instance);
    std::uint64_t creation_revisions = 0;
    {
        RenderGraphInstance probe{*session, 1, plan};
        CHECK(probe.initialize());
        creation_revisions = revision(probe) - initial_revision;
    }
    CHECK(creation_revisions != 0);
    CHECK(session->drain());
    nri::Buffer* buffer = nullptr;
    CHECK(session->nriCore().CreatePlacedBuffer(
        session->device(), NriDeviceHeap, {32, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE}, buffer
    ) == nri::Result::SUCCESS);
    const auto render = [&]()
    {
        const std::array instances{&*instance};
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(!frames->submitting());
        CHECK(session->drain());
    };
    // A viewport can change sampled resources several times, then be detached and recreated
    // at the same allocator address. Local ++ used to collide with the next global identity.
    for (std::uint64_t update = 0; update < 2 * creation_revisions; ++update)
    {
        auto& draws = instance->rasterDraws(*instance->compiledPass(pass.pass), 0);
        draws.reads = {{buffer, GraphBufferRange{16 * (update % 2), 16}, EGraphUse::STORAGE_READ, 1}};
        render();
    }
    const auto cached_revision = revision(*instance);
    const auto cached_rebuilds = rebuilds();
    const auto* address = &*instance;
    instance.reset();
    CHECK(session->drain());
    instance.emplace(*session, 1, plan);
    CHECK(&*instance == address);
    CHECK(instance->initialize());
    render();
    CHECK(rebuilds() == cached_rebuilds + 1);
    CHECK(revision(*instance) > cached_revision);
    const auto adopted_rebuilds = rebuilds();
    render();
    CHECK(rebuilds() == adopted_rebuilds); // Stable inputs still reuse the dependency cache.
    instance.reset();
    CHECK(session->drain());
    session->nriCore().DestroyBuffer(buffer);
    std::puts("IMPORTED IDENTITY PASS: same address rebuilt; unchanged inputs reused dependencies");
}

static void deviceLostDelivery(Validation& validation, bool enable_validation = nativeValidationEnabled())
{
    TestDevice session({}, diagnostics(validation), enable_validation);
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    nri::Buffer* buffer = nullptr;
    const nri::BufferDesc description{16, 0, nri::BufferUsageBits::NONE};
    CHECK(core.CreatePlacedBuffer(session->device(), NriHostReadbackHeap, description, buffer) == nri::Result::SUCCESS);
    auto& queue = session->queue();
    const auto record = [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void> { return {}; };
    CHECK(submitFixtureCommands(*session, queue, record));
    CHECK(session->drain()); // No live GPU access is exposed to a simulated loss on healthy hardware.
    RenderError delivered;
    SubmissionCallback callback;
    queue.arm(queue.submitted(), callback, [](void* context, RenderResult<void> result) noexcept
    {
        CHECK(!result);
        *static_cast<RenderError*>(context) = result.error();
    }, &delivered);
    const auto submit = core.QueueSubmit;
    core.QueueSubmit = [](nri::Queue&, const nri::QueueSubmitDesc&) noexcept -> nri::Result
    { return nri::Result::DEVICE_LOST; };
    const auto rejected = submitFixtureCommands(*session, queue, record);
    core.QueueSubmit = submit;
    CHECK(!rejected && session->deviceLost());
    CHECK(rejected.error().args[0] == static_cast<std::uint32_t>(nri::Result::DEVICE_LOST));
    bool released = false;
    session->deferRelease(queue, [&]() noexcept
    {
        core.DestroyBuffer(buffer);
        released = true;
    });
    const auto progressed = session->progressCompletions();
    CHECK(!progressed && progressed.error() == rejected.error());
    CHECK(delivered == rejected.error() && released && !callback.armed());
    session.reset();
    std::puts(
        "DEVICE LOST contract PASS: terminal delivered, cleanup settled; injected only after real GPU completion"
    );
}

static void importedExtentContracts(Validation& validation)
{
    TestDevice session({}, diagnostics(validation), nativeValidationEnabled());
    const auto& core = session->nriCore();
    for (const bool resolve : {false, true})
    {
        RenderGraphBuilder builder;
        GraphImageDesc image{0, 0, 1, 1, lux::rdesc::ETextureFormat::RGBA8_UNORM};
        image.extent.source = EImageExtentSource::IMPORTED;
        image.samples = resolve ? 4 : 1;
        const auto source = take(builder.declareResource({
            "source", image, EGraphOrigin::IMPORTED, {{}},
            resolve ? EGraphUse::COLOR_ATTACHMENT : EGraphUse::TRANSFER_SOURCE
        }));
        image.samples = 1;
        const auto destination = take(builder.declareResource({"destination", image, EGraphOrigin::IMPORTED}));
        if (resolve)
        {
            RasterWork work;
            work.program = RasterPassTemplate{"resolve extent"};
            RasterAttachment color{{{source, 0}}};
            color.resolve = ResourceAccess{{destination, 0}};
            work.colors.push_back(color);
            const auto pass = take(builder.addRasterPass("resolve", std::move(work)));
            CHECK(builder.exportOutput({pass.written.back()}));
        }
        else
        {
            const auto pass = take(builder.addTransferPass(
                "copy", TransferWork{{lux::render::CopyImage{{{source, 0}}, {{destination, 0}}}}}
            ));
            CHECK(builder.exportOutput({pass.written.front()}));
        }
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyzeRenderGraph(graph));
        auto ready_result = stdexec::sync_wait(GraphCompiler::compile(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis, nullptr
        ));
        CHECK(ready_result);
        auto plan = take(std::move(std::get<0>(*ready_result)));
        std::array<nri::Texture*, 3> images{};
        for (std::size_t i = 0; i < images.size(); ++i)
        {
            nri::TextureDesc description{};
            description.type = nri::TextureType::TEXTURE_2D;
            description.format = nri::Format::RGBA8_UNORM;
            description.width = description.height = i == 1 ? 8 : 16;
            description.depth = description.mipNum = description.layerNum = 1;
            description.sampleNum = resolve && i == 0 ? 4 : 1;
            description.usage = nri::TextureUsageBits::COLOR_ATTACHMENT;
            CHECK(core.CreatePlacedTexture(session->device(), NriDeviceHeap, description, images[i]) ==
                  nri::Result::SUCCESS);
        }
        {
            RenderGraphInstance invalid{*session, 1, plan};
            invalid.bindImage(source, 0, *images[0]);
            invalid.bindImage(destination, 0, *images[1]);
            const auto rejected = invalid.initialize();
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
        }
        {
            RenderGraphInstance instance{*session, 1, plan};
            instance.bindImage(source, 0, *images[0]);
            instance.bindImage(destination, 0, *images[2]);
            CHECK(instance.initialize());
            const std::array<TargetFrame, 1> smaller_frames{{{images[1]}}};
            const RenderGraphInstance::ImageReplacement replacement{destination, smaller_frames};
            const auto rejected = instance.prepareResize({}, {&replacement, 1});
            CHECK(!rejected && rejected.error().type == kInvalidTargetExtent);
            // A graph with only imported resources legitimately accepts an empty output.
            const std::array<TargetFrame, 1> original{{{images[2]}}};
            const RenderGraphInstance::ImageReplacement valid{destination, original};
            auto unchanged = take(instance.prepareResize({}, {&valid, 1}));
            CHECK(instance.prepareResizeAdoption(unchanged));
            instance.commitResize(std::move(unchanged));
        }
        CHECK(session->drain());
        for (auto* texture : images) core.DestroyTexture(texture);
    }
}

static void verifyTonemapOutput(
    ipc::IpcRequester& client,
    const CommandList& commands,
    DeviceSessionId session,
    SceneId scene,
    FeatureId clear,
    FeatureId tonemap,
    FeatureId capture,
    EffectOutputDesc output,
    EColorEncoding encoding
)
{
    CapturedPixels pixels;
    CreateEffectRequest request{output};
    request.parameters.exposure = 2;
    auto& recipe = request.definition;
    recipe.features.push_back({
        clear, "hdr", take(ConfigValue::make(WindowClear::Config{rdesc::ETextureFormat::RGBA16_SFLOAT}))
    });
    recipe.features.push_back({
        tonemap, "tone", take(ConfigValue::make(TonemapFeatureConfig{output, encoding})), 0, EParameterSource::VIEW
    });
    recipe.features.push_back({capture, "readback", take(ConfigValue::make(CaptureConfig{&pixels, output.format}))});
    recipe.connections = {{{0, "color"}, {1, "hdr_color"}}, {{1, "color"}, {2, "color"}}};
    recipe.outputs = {{{1, "color"}, GraphOutputRequirements{output.format, 1}}, {{2, "pixels"}, {}}};
    const auto effect = call<CreateEffect>(client, commands, std::move(request)).effect;
    const auto view = call<CreateView>(client, commands, {scene}).view;
    const auto target = call<CreateTarget>(client, commands, {session, {64, 64, output.format}}).target;
    const auto attached = call<CreateRenderJob>(client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    StartRenderJob::Request invalid_start{session, attached, target};
    invalid_start.binding.inputs.parameters.resize(16);
    CHECK(call<StartRenderJob>(client, commands, std::move(invalid_start), false).error.type == kDeviceArgument);
    call<StartRenderJob>(client, commands, {session, attached, target});
    const auto other_scene = call<CreateScene>(client, commands, {}).scene;
    const auto other_view = call<CreateView>(client, commands, {other_scene}).view;
    const auto other_target = call<CreateTarget>(client, commands, {session, {64, 64, output.format}}).target;
    const auto other_attachment =
        startRenderJob(client, commands, session, other_scene, other_view, other_target, effect);
    const auto capture_name = CaptureDescriptor::descriptor.type;
    const auto resource = call<QueryResource>(client, commands, {session, ViewRequest{scene, view}, capture_name}).resource_id;
    const auto other_resource =
        call<QueryResource>(client, commands, {session, ViewRequest{other_scene, other_view}, capture_name}).resource_id;
    CHECK(resource != other_resource);
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kRuntimeState);
    const auto compiled = call<QueryRenderStats>(client, commands, {}).compiled_graphs;
    const auto pipeline_layouts = call_capture_counts[1].load();
    const auto pipelines = call_capture_counts[2].load();
    CHECK(!call_capture_core || pipelines != 0);
    renderFrame(client, commands, attached);
    renderFrame(client, commands, other_attachment);
    call<SetEffectParameters>(client, commands, {effect, {-1.F}});
    call<SetEffectParameters>(client, commands, {effect, {-1.F}});
    const auto invalid = std::numeric_limits<float>::quiet_NaN();
    CHECK(call<SetEffectParameters>(client, commands, {effect, {invalid}}, false).error.type == kFrameContract);
    renderFrame(client, commands, other_attachment);
    renderFrame(client, commands, attached);
    CHECK(call<QueryRenderStats>(client, commands, {}).compiled_graphs == compiled);
    CHECK(call_capture_counts[1].load() == pipeline_layouts && call_capture_counts[2].load() == pipelines);
    stopAndRemoveJob(client, commands, attached);
    CHECK(call<RemoveEffect>(client, commands, {effect}, false).error.type == kRuntimeState);
    stopAndRemoveJob(client, commands, other_attachment);
    call<RemoveEffect>(client, commands, {effect});
    CHECK(call<QueryResource>(client, commands, {session, ViewRequest{scene, view}, capture_name}).resource_id == resource);
    CHECK(call<QueryResource>(client, commands, {session, ViewRequest{other_scene, other_view}, capture_name}).resource_id ==
        other_resource);
    const auto deadline = Clock::now() + 10s;
    const auto retire_view = [&](ViewRequest location, unsigned count)
    {
        call<RemoveView>(client, commands, location);
        while (pixels.ready.load(std::memory_order_acquire) != count)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    retire_view({scene, view}, 1);
    retire_view({other_scene, other_view}, 2);
    call<RemoveTarget>(client, commands, target);
    call<RemoveTarget>(client, commands, other_target);
    call<RemoveScene>(client, commands, {other_scene});
    CHECK(pixels.frames.size() == 4);
    constexpr std::array hdr{.25, .5, .75};
    const auto matches = [&](const auto& frame, double gain)
    {
        if (frame[3] != std::byte{255}) return false;
        for (std::size_t channel = 0; channel < hdr.size(); ++channel)
        {
            const auto x = hdr[channel] * gain;
            auto value = (x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14);
            if (encoding == EColorEncoding::SRGB)
                value = 1.055 * std::pow(value, 1 / 2.4) - .055;
            const auto expected = std::lround(value * 255);
            if (std::abs(std::to_integer<long>(frame[channel]) - expected) > 2) return false;
        }
        return true;
    };
    for (std::size_t index : {0, 2})
    {
        const auto frames = std::span(pixels.frames).subspan(index, 2);
        CHECK(std::ranges::count_if(frames, [&](const auto& frame) { return matches(frame, 4); }) == 1);
        CHECK(std::ranges::count_if(frames, [&](const auto& frame) { return matches(frame, .5); }) == 1);
    }
    std::printf("SHARED EFFECT PASS two scenes rendered +2 EV and -1 EV, pipelines=%llu unchanged, resources retained\n",
        static_cast<unsigned long long>(pipelines));
}

static void verifyTargetInput(
    ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId session, SceneId scene,
    FeatureId clear, FeatureId tone, FeatureId capture, DeviceSessionId foreign_device = {}
)
{
    const auto hdr_format = rdesc::ETextureFormat::RGBA16_SFLOAT;
    const auto ldr_format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto producer_view = call<CreateView>(client, commands, {scene}).view;
    const auto consumer_view = call<CreateView>(client, commands, {scene}).view;
    const auto source = call<CreateTarget>(
        client, commands, {session, {64, 64, hdr_format, 1, ETargetKind::OFFSCREEN, true}}
    ).target;
    const auto target = call<CreateTarget>(client, commands, {session, {64, 64, ldr_format}}).target;
    CreateEffectRequest produce{{hdr_format, 1, ETargetKind::OFFSCREEN, true}};
    produce.definition.features.push_back({clear, "hdr", take(ConfigValue::make(WindowClear::Config{hdr_format}))});
    produce.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{hdr_format, 1}});
    const auto producer = call<CreateEffect>(client, commands, std::move(produce)).effect;

    CapturedPixels pixels;
    CreateEffectRequest consume{{ldr_format}};
    consume.definition.features.push_back({
        tone, "tone", take(ConfigValue::make(TonemapFeatureConfig{{ldr_format}, EColorEncoding::LINEAR}))
    });
    consume.definition.features.push_back({
        capture, "readback", take(ConfigValue::make(CaptureConfig{&pixels, ldr_format}))
    });
    consume.definition.connections.push_back({{0, "color"}, {1, "color"}});
    consume.definition.outputs = {{{0, "color"}, GraphOutputRequirements{ldr_format, 1}}, {{1, "pixels"}, {}}};
    const FeaturePortRef input{0, "hdr_color"};
    GraphImageDesc hdr{0, 0, 1, 1, hdr_format};
    hdr.extent.source = EImageExtentSource::TARGET;
    consume.definition.inputs.push_back({
        input, {"author.hdr", hdr, EGraphOrigin::IMPORTED, {GraphImageRange{}}, EGraphUse::SAMPLED},
        "lux.color.linear-hdr.v1"
    });
    const auto consumer = call<CreateEffect>(client, commands, std::move(consume)).effect;
    const auto writer = startRenderJob(client, commands, session, scene, producer_view, source, producer);
    const auto reader = call<CreateRenderJob>(client, commands, {
        scene, consumer_view, consumer, EJobCadence::REQUESTED
    }).job;
    StartRenderJob::Request start{session, reader, target};
    start.binding.target_images.push_back({{0, "author.hdr"}, source});
    CHECK(call<StartRenderJob>(client, commands, start, false).error.type == kRuntimeIdentity);
    start.binding.target_images.front().destination = input;
    if (!foreign_device.isNull())
    {
        const auto foreign = call<CreateTarget>(client, commands, {
            foreign_device, {64, 64, hdr_format, 1, ETargetKind::OFFSCREEN, true}
        }).target;
        start.binding.target_images.front().target = foreign;
        CHECK(call<StartRenderJob>(client, commands, start, false).error.type == kJobState);
        call<RemoveTarget>(client, commands, foreign);
        start.binding.target_images.front().target = source;
        std::puts("DEVICE IMPORT PASS: rejected a sampled Target from the peer physical adapter");
    }
    call<StartRenderJob>(client, commands, std::move(start));
    CHECK(call<RequestRenderJob>(client, commands, {reader}, false).error.type == kRuntimeState);
    const auto published = call<RequestRenderJob>(client, commands, {writer});
    const auto submitted = call<RequestRenderJob>(client, commands, {reader});
    CHECK(published.slot_index != submitted.slot_index);
    stopAndRemoveJob(client, commands, reader);
    call<RemoveView>(client, commands, {scene, consumer_view});
    const auto deadline = Clock::now() + 10s;
    while (!pixels.ready.load(std::memory_order_acquire)) CHECK(Clock::now() < deadline);
    CHECK(submitted.job == reader && submitted.slot_index < pixels.frames.size());
    const auto& color = pixels.frames[submitted.slot_index];
    const bool matches = color[0] > std::byte{0} && color[1] > color[0] && color[2] > color[1] &&
                         color[3] == std::byte{255};
    if (!matches)
    {
        std::fprintf(stderr, "Sampled Target slot %u pixel %u %u %u %u\n", submitted.slot_index,
            std::to_integer<unsigned>(color[0]), std::to_integer<unsigned>(color[1]),
            std::to_integer<unsigned>(color[2]), std::to_integer<unsigned>(color[3]));
    }
    CHECK(matches);
    stopAndRemoveJob(client, commands, writer);
    call<RemoveView>(client, commands, {scene, producer_view});
    call<RemoveTarget>(client, commands, target);
    call<RemoveTarget>(client, commands, source);
    call<RemoveEffect>(client, commands, {consumer});
    call<RemoveEffect>(client, commands, {producer});
    std::puts("TARGET INPUT PASS: authored Feature port resolved to a sampled producer output");
}

static void basicFeatureOutputs(bool d3d12)
{
    RenderServerConfig config;
    config.channel_name = "lux.render.test.hzb-feature";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    DeviceOptions options;
    options.texture_descriptors = true;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto& execution = lux::execution::ExecutionRuntime::instance();
    auto server = take(RenderServer::create(config, execution));
    const auto clear = take(server->registerFeature<WindowClear>());
    const auto hzb = take(server->featureId("lux.render.feature.Hzb"));
    const auto tone = take(server->featureId("lux.render.feature.TonemapFeature"));
    take(server->registerResource<CaptureBuffer>());
    const auto capture = take(server->registerFeature<CaptureFeature>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const TargetDesc target_desc{64, 64, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto target = call<CreateTarget>(*client, commands, {session, target_desc}).target;
    GraphImageDesc depth{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
    depth.extent.source = EImageExtentSource::TARGET;
    CapturedPixels current, previous;
    CaptureConfig current_copy{&current, rdesc::ETextureFormat::R32_SFLOAT};
    current_copy.mips = 4;
    current_copy.mip = 3;
    auto previous_copy = current_copy;
    previous_copy.result = &previous;
    CreateEffectRequest request{{target_desc.format}};
    auto& recipe = request.definition;
    recipe.features.push_back({clear, "depth", take(ConfigValue::make(WindowClear::Config{depth.format}))});
    recipe.features.push_back({hzb, "hzb", take(ConfigValue::make(HzbFeatureConfig{depth, 4}))});
    recipe.features.push_back({capture, "current", take(ConfigValue::make(current_copy))});
    recipe.features.push_back({capture, "previous", take(ConfigValue::make(previous_copy))});
    recipe.connections = {{{0, "color"}, {1, "depth"}}, {{1, "current"}, {2, "color"}}, {{1, "previous"}, {3, "color"}}};
    recipe.outputs = {{{2, "pixels"}, {}}, {{3, "pixels"}, {}}};
    const auto effect = call<CreateEffect>(*client, commands, std::move(request)).effect;
    const auto attachment = startRenderJob(*client, commands, session, scene, view, target, effect);
    renderFrame(*client, commands, attachment);
    renderFrame(*client, commands, attachment);
    stopAndRemoveJob(*client, commands, attachment);
    call<RemoveView>(*client, commands, {scene, view});
    verifyTonemapOutput(*client, commands, session, scene, clear, tone, capture,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EColorEncoding::LINEAR);
    verifyTonemapOutput(*client, commands, session, scene, clear, tone, capture,
        {rdesc::ETextureFormat::RGBA8_UNORM}, EColorEncoding::SRGB);
    verifyTonemapOutput(*client, commands, session, scene, clear, tone, capture,
        {rdesc::ETextureFormat::RGBA8_SRGB}, EColorEncoding::SRGB);
    verifyTargetInput(*client, commands, session, scene, clear, tone, capture);
    const auto secondary = call<CreateDeviceSession>(*client, commands, options).device;
    verifyTargetInput(*client, commands, secondary, scene, clear, tone, capture);
    std::puts("DEVICE HISTORY PASS: sampled and read back published pixels on both sessions");
    std::printf("TONEMAP FEATURE PASS backend=%s linear, shader sRGB, target sRGB and Effect exposure\n",
        d3d12 ? "D3D12" : "Vulkan");
    server->stop();
    render.join();
    const auto values = [](const CapturedPixels& pixels)
    {
        CHECK(pixels.ready.load() == 1 && pixels.frames.size() == 2);
        std::array<float, 2> result{};
        for (std::size_t i = 0; i < result.size(); ++i)
            std::memcpy(&result[i], pixels.frames[i].data(), sizeof(float));
        std::ranges::sort(result);
        return result;
    };
    CHECK((values(current) == std::array{.25f, .25f}));
    CHECK((values(previous) == std::array{.25f, 1.f}));
    CHECK(server->stats().validation_errors == 0);
    std::printf("HZB FEATURE PASS backend=%s D32 to R32, mip 3 and previous sample\n", d3d12 ? "D3D12" : "Vulkan");
}

#ifdef _WIN32
struct WsiFailureProbe final
{
    struct Descriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{ResourceNameView{"test.wsi.failure"}, 0, 0, {}};
    };

    static inline std::atomic<bool> fail_view = false;
    static inline decltype(nri::CoreInterface::CreateTextureView) create_texture_view = nullptr;

    static RenderResult<std::unique_ptr<WsiFailureProbe>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        auto& core = const_cast<nri::CoreInterface&>(context.device.nriCore());
        create_texture_view = core.CreateTextureView;
        core.CreateTextureView = [](const nri::TextureViewDesc& description, nri::Descriptor*& view) noexcept
        {
            if (description.type == nri::TextureView::COLOR_ATTACHMENT && fail_view.exchange(false))
                return nri::Result::OUT_OF_MEMORY;
            return create_texture_view(description, view);
        };
        return std::make_unique<WsiFailureProbe>();
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, WsiFailureProbe, &create>(nullptr);
    }
};

static HWND createProbeWindow()
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpszClassName = L"LuxRenderOutputProbe";
    window_class.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
    {
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    };
    CHECK(RegisterClassW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    return CreateWindowExW(
        0, window_class.lpszClassName, L"LUX Render WSI probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, nullptr, nullptr, instance, nullptr
    );
}

static void windowOutput(bool d3d12)
{
    std::promise<HWND> opened;
    auto window_result = opened.get_future();
    std::thread ui([&]
    {
        const auto window = createProbeWindow();
        CHECK(window);
        ShowWindow(window, SW_SHOW);
        opened.set_value(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    const auto window = window_result.get();
    const auto dimensions = [&]
    {
        RECT client;
        CHECK(GetClientRect(window, &client));
        return std::pair{std::uint32_t(client.right), std::uint32_t(client.bottom)};
    };
    RenderServerConfig config;
    config.channel_name = "lux.render.test.window";
    config.frames_in_flight = 2;
    config.validation = nativeValidationEnabled();
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        std::fprintf(stderr, "WSI diagnostic %u: %.*s\n", unsigned(diagnostic.severity),
            int(diagnostic.message.size()), diagnostic.message.data());
    };
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    auto& execution = lux::execution::ExecutionRuntime::instance();
    auto server = take(RenderServer::create(config, execution));
    auto feature = take(server->registerFeature<WindowClear>());
    take(server->registerResource<WsiFailureProbe>());
    auto client = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(client);
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*client);
    const auto session = call<CreateDeviceSession>(*client, commands, options).device;
    call<GetOrCreateResource>(*client, commands, {session, DeviceScope{}, WsiFailureProbe::Descriptor::descriptor.type});
    const auto [width, height] = dimensions();
    const TargetDesc description{width, height, rdesc::ETextureFormat::BGRA8_UNORM, 1, ETargetKind::SURFACE};
    auto target = call<CreateTarget>(*client, commands, {session, description, SurfaceDescription{Win32Surface{window}}});
    CHECK(target.output_available && target.description.width == width && target.description.height == height);
    CreateEffectRequest recipe;
    recipe.output = {target.description.format, 1, ETargetKind::SURFACE};
    recipe.definition.features.push_back({
        feature, "clear", take(ConfigValue::make(WindowClear::Config{target.description.format}))
    });
    recipe.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.description.format, 1}});
    const auto effect = call<CreateEffect>(*client, commands, std::move(recipe)).effect;
    const auto scene = call<CreateScene>(*client, commands, {}).scene;
    const auto view = call<CreateView>(*client, commands, {scene}).view;
    const auto first = startRenderJob(*client, commands, session, scene, view, target.target, effect);
    const auto second = call<CreateRenderJob>(*client, commands, {scene, view, effect, EJobCadence::REQUESTED}).job;
    CHECK(call<StartRenderJob>(*client, commands, {session, second, target.target}, false).error.type == kTargetInUse);
    const auto layout = call<QueryRenderJobInputLayout>(*client, commands, first).layout;
    auto foreign = take(FrameLayout::create(layout.channels(), layout.revision()));
    CHECK(layout == foreign && !layout.hasSameIdentity(foreign));
    auto stale = take(FrameData::create<std::tuple<>>(foreign));
    CHECK(stale.seal());
    // The packet is qualified after successful Acquire and before the first GPU submission.
    CHECK(call<RequestRenderJob>(*client, commands, {first, &stale}, false).error.type == kFrameContract);
    CHECK(server->stats().submissions == 0);
    stopAndRemoveJob(*client, commands, first);
    call<StartRenderJob>(*client, commands, {session, second, target.target});
    const auto waitPresented = [&](std::uint64_t count)
    {
        const auto deadline = Clock::now() + 15s;
        while (server->stats().presentations < count)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
    };
    for (int i = 0; i < 6; ++i) renderFrame(*client, commands, second);
    waitPresented(6);

    CHECK(SetWindowPos(window, nullptr, 0, 0, 400, 300, SWP_NOMOVE | SWP_NOZORDER));
    const auto [changed_width, changed_height] = dimensions();
    auto requested = target.description;
    requested.width = changed_width;
    requested.height = changed_height;
    // The first color view of the replacement fails after the previous swapchain is closed.
    WsiFailureProbe::fail_view = true;
    const auto failed = call<ResizeTarget>(*client, commands, {target.target, requested}, false);
    CHECK(!WsiFailureProbe::fail_view && failed.error.type == kDeviceFailure);
    CHECK(!failed.output_available && failed.target == target.target && failed.epoch == target.epoch);
    CHECK(call<SetTargetActive>(*client, commands, {target.target, true, failed.epoch}, false).error.type ==
        kRuntimeState);
    const auto resized = call<ResizeTarget>(*client, commands, {target.target, requested});
    CHECK(resized.output_available && resized.epoch == target.epoch + 1);
    CHECK(resized.description.width == changed_width && resized.description.height == changed_height);
    CHECK(call<SetTargetActive>(*client, commands, {target.target, true, target.epoch}, false).error.type ==
          kRuntimeState);
    call<SetTargetActive>(*client, commands, {target.target, true, resized.epoch});
    call<RebindRenderJobEffect>(*client, commands, {session, second, effect});
    CHECK(call<StopRenderJob>(*client, commands, first, false).error.type == kJobIdentity);
    for (int i = 0; i < 6; ++i) renderFrame(*client, commands, second);
    waitPresented(12);
    const auto submitted = server->stats().submissions;
    CHECK(call<RequestRenderJob>(*client, commands, {second, &stale}, false).error.type == kFrameContract);
    CHECK(server->stats().submissions == submitted);
    stale.release();
    call<RemoveTarget>(*client, commands, target.target);
    // Reusing the same HWND proves Remove returned the native window association.
    target = call<CreateTarget>(*client, commands, {session, requested, SurfaceDescription{Win32Surface{window}}});
    CHECK(target.output_available);
    call<RemoveRenderJob>(*client, commands, second);
    const auto final_job = startRenderJob(*client, commands, session, scene, view, target.target, effect);
    renderFrame(*client, commands, final_job);
    server->stop(); // The live Target is retired before the window's thread is closed.
    render.join();
    CHECK(server->stats().validation_errors == 0);
    CHECK(PostMessageW(window, WM_CLOSE, 0, 0));
    ui.join();
    std::puts("WSI RETIREMENT PASS: READY Acquire reused after Stop and released by Remove; live Target settled at join");
    std::puts("WSI RETRY PASS: failed replacement reports unavailable; same TargetId recovered");
    std::printf("WSI PASS backend=%s real presents=%llu resize/writer replacement/remove/shutdown\n",
        d3d12 ? "D3D12" : "Vulkan", static_cast<unsigned long long>(server->stats().presentations));
}

// A real timeline wait on the retiring GPU. The submitted work cannot retire until the host signals it.
struct RetirementFence final
{
    explicit RetirementFence(DeviceSession& session) noexcept : core(session.nriCore())
    {
        auto& device = session.device();
        auto* native = core.GetDeviceNativeObject(&device);
        if (session.description().graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            auto& dx_device = *static_cast<ID3D12Device*>(native);
            CHECK(SUCCEEDED(dx_device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx_fence))));
            nri::WrapperD3D12Interface wrapper{};
            CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperD3D12Interface), &wrapper) ==
                nri::Result::SUCCESS);
            CHECK(wrapper.CreateFenceD3D12(device, {dx_fence}, fence) == nri::Result::SUCCESS);
            return;
        }
        vk_device = static_cast<VkDevice>(native);
        nri::WrapperVKInterface wrapper{};
        CHECK(nri::nriGetInterface(device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS);
        const auto proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(wrapper.GetDeviceProcAddrVK(device));
        const auto create = reinterpret_cast<PFN_vkCreateSemaphore>(proc(vk_device, "vkCreateSemaphore"));
        destroy_vk = reinterpret_cast<PFN_vkDestroySemaphore>(proc(vk_device, "vkDestroySemaphore"));
        signal_vk = reinterpret_cast<PFN_vkSignalSemaphore>(proc(vk_device, "vkSignalSemaphore"));
        CHECK(create && destroy_vk && signal_vk);
        const VkSemaphoreTypeCreateInfo timeline{
            VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0
        };
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline, 0};
        CHECK(create(vk_device, &info, nullptr, &vk_fence) == VK_SUCCESS);
        const nri::FenceVKDesc native_fence{reinterpret_cast<std::uintptr_t>(vk_fence)};
        CHECK(wrapper.CreateFenceVK(device, native_fence, fence) == nri::Result::SUCCESS);
    }

    ~RetirementFence() noexcept
    {
        core.DestroyFence(fence);
        if (dx_fence) dx_fence->Release();
        else destroy_vk(vk_device, vk_fence, nullptr);
    }

    void signal() const noexcept
    {
        if (dx_fence)
        {
            CHECK(SUCCEEDED(dx_fence->Signal(1)));
            return;
        }
        const VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, vk_fence, 1};
        CHECK(signal_vk(vk_device, &info) == VK_SUCCESS);
    }

    const nri::CoreInterface& core;
    nri::Fence* fence = nullptr;
    ID3D12Fence* dx_fence = nullptr;
    VkDevice vk_device = nullptr;
    VkSemaphore vk_fence = VK_NULL_HANDLE;
    PFN_vkDestroySemaphore destroy_vk = nullptr;
    PFN_vkSignalSemaphore signal_vk = nullptr;
};

struct SignalRetirementFence final
{
    static constexpr std::string_view name{"test.device.signal_retirement"};
    using Request = EmptyRequest;
    using Response = AckResponse;
};

struct DeviceRetirementProbe final
{
    struct Descriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const ResourceDefinition descriptor{ResourceNameView{"test.device.retirement"}, 0, 0, {}};
    };

    static inline std::array<nri::AdapterDesc, 3> adapters;
    static inline std::array<bool, 3> present_retirement{};
    static inline std::array<std::atomic<bool>, 3> destroyed{};
    static inline std::size_t created = 0;
    static inline decltype(nri::CoreInterface::QueueSubmit) submit = nullptr;
    static inline decltype(nri::CoreInterface::QueueWaitIdle) wait_idle = nullptr;
    static inline RetirementFence* gate = nullptr;
    static inline std::atomic<bool> arm = false, held = false;
    static inline std::atomic<bool> hold_queue_idle = false, queue_idle_entered = false;
    std::size_t index;
    std::optional<RetirementFence> fence;

    explicit DeviceRetirementProbe(DeviceSession& session) noexcept : index(created++)
    {
        CHECK(index < adapters.size());
        adapters[index] = session.description().adapterDesc;
        present_retirement[index] = session.supportsPresentRetirement();
        if (index != 1) return;
        fence.emplace(session);
        gate = &*fence;
        auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
        submit = core.QueueSubmit;
        wait_idle = core.QueueWaitIdle;
        core.QueueWaitIdle = [](nri::Queue* queue) noexcept
        {
            if (hold_queue_idle.load())
            {
                queue_idle_entered.store(true);
                hold_queue_idle.wait(true);
            }
            return wait_idle(queue);
        };
        core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& input) noexcept
        {
            if (!arm.exchange(false)) return submit(queue, input);
            CHECK(input.waitFenceNum == 0);
            const nri::FenceSubmitDesc wait{gate->fence, 1, nri::StageBits::ALL};
            auto waiting = input;
            waiting.waitFences = &wait;
            waiting.waitFenceNum = 1;
            const auto result = submit(queue, waiting);
            held.store(result == nri::Result::SUCCESS);
            return result;
        };
    }

    ~DeviceRetirementProbe() noexcept
    {
        if (fence)
        {
            const_cast<nri::CoreInterface&>(fence->core).QueueSubmit = submit;
            const_cast<nri::CoreInterface&>(fence->core).QueueWaitIdle = wait_idle;
            gate = nullptr;
            fence.reset();
        }
        destroyed[index].store(true);
    }

    static void signal(const EmptyRequest&, AckResponse& response) noexcept
    {
        CHECK(gate && held.exchange(false));
        gate->signal();
        response = {};
    }

    static RenderResult<std::unique_ptr<DeviceRetirementProbe>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        return std::make_unique<DeviceRetirementProbe>(context.device);
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, DeviceRetirementProbe, &create>(nullptr);
    }
};

static bool physicalDeviceLifecycle(bool d3d12)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto backend = d3d12 ? "D3D12" : "Vulkan";
    RenderServerConfig config{"lux.render.test.device_lifecycle", nativeValidationEnabled()};
    config.diagnostics.report = [](void*, const RenderDiagnostic& diagnostic) noexcept
    {
        if (diagnostic.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "Device lifecycle diagnostic: %.*s\n",
                int(diagnostic.message.size()), diagnostic.message.data());
    };
    auto server = take(RenderServer::create(config, lux::execution::ExecutionRuntime::instance()));
    const auto clear = take(server->registerFeature<WindowClear>());
    const auto tone = take(server->featureId("lux.render.feature.TonemapFeature"));
    const auto capture = take(server->registerFeature<CaptureFeature>());
    take(server->registerResource<CaptureBuffer>());
    take(server->registerResource<DeviceRetirementProbe>());
    const std::array gate_commands{CommandBinding::declare<SignalRetirementFence, &DeviceRetirementProbe::signal>()};
    CHECK(server->registerCommands(gate_commands));
    auto requester = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(requester);
    auto& client = *requester;
    std::thread render([&] { CHECK(server->run()); });
    const auto commands = queryCommands(client);
    DeviceOptions options;
    options.backend = d3d12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    options.texture_descriptors = true;
    options.physical_device_index = 0;
    const auto a = call<CreateDeviceSession>(client, commands, options).device;
    options.physical_device_index = 1;
    const auto second = call<CreateDeviceSession>(client, commands, options, false);
    if (second.error.type)
    {
        std::printf("PHYSICAL LIFECYCLE UNKNOWN backend=%s adapter 1 error=%llu arg=%llu\n",
            backend, second.error.type, second.error.args[0]);
        server->stop();
        render.join();
        return false;
    }
    const auto b = second.device;
    const auto probe_type = DeviceRetirementProbe::Descriptor::descriptor.type;
    const auto probe_a = call<GetOrCreateResource>(client, commands, {a, DeviceScope{}, probe_type}).resource_id;
    const auto probe_b = call<GetOrCreateResource>(client, commands, {b, DeviceScope{}, probe_type}).resource_id;
    const auto& adapters = DeviceRetirementProbe::adapters;
    const bool distinct = adapters[0].uid.low != adapters[1].uid.low || adapters[0].uid.high != adapters[1].uid.high;
    if (!distinct)
    {
        std::printf("PHYSICAL LIFECYCLE UNKNOWN backend=%s duplicate adapter UID\n", backend);
        server->stop();
        render.join();
        return false;
    }
    std::printf("DEVICE LIFECYCLE ADAPTERS backend=%s A=%s uid=%llx/%llx B=%s uid=%llx/%llx\n",
        backend, adapters[0].name, adapters[0].uid.high, adapters[0].uid.low,
        adapters[1].name, adapters[1].uid.high, adapters[1].uid.low);

    const auto scene = call<CreateScene>(client, commands, {}).scene;
    const auto view = call<CreateView>(client, commands, {scene}).view;
    verifyTargetInput(client, commands, a, scene, clear, tone, capture, b);
    verifyTargetInput(client, commands, b, scene, clear, tone, capture, a);

    std::promise<std::array<HWND, 2>> opened;
    auto ready = opened.get_future();
    std::thread ui([&]
    {
        std::array windows{createProbeWindow(), createProbeWindow()};
        CHECK(windows[0] && windows[1]);
        CHECK(SetWindowPos(windows[0], nullptr, 40, 40, 320, 240, SWP_NOZORDER));
        CHECK(SetWindowPos(windows[1], nullptr, 420, 40, 320, 240, SWP_NOZORDER));
        for (auto window : windows) ShowWindow(window, SW_SHOW);
        opened.set_value(windows);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    });
    const auto windows = ready.get();
    const auto surface_desc = [](HWND window)
    {
        RECT client;
        CHECK(GetClientRect(window, &client));
        return TargetDesc{std::uint32_t(client.right), std::uint32_t(client.bottom),
            rdesc::ETextureFormat::BGRA8_UNORM, 1, ETargetKind::SURFACE};
    };
    const auto surface_a = call<CreateTarget>(
        client, commands, {a, surface_desc(windows[0]), SurfaceDescription{Win32Surface{windows[0]}}}
    );
    const auto surface_b = call<CreateTarget>(
        client, commands, {b, surface_desc(windows[1]), SurfaceDescription{Win32Surface{windows[1]}}}
    );
    CHECK(surface_a.output_available && surface_b.output_available);
    if (!d3d12)
        std::printf("VULKAN WSI RETIREMENT A=%s B=%s\n",
            DeviceRetirementProbe::present_retirement[0] ? "present-fence" : "queue-idle",
            DeviceRetirementProbe::present_retirement[1] ? "present-fence" : "queue-idle");
    const auto effect_for = [&](TargetDesc description)
    {
        CreateEffectRequest recipe{{description.format, description.samples, description.kind, description.sampled}};
        auto config = take(ConfigValue::make(WindowClear::Config{description.format}));
        recipe.definition.features.push_back({clear, "clear", std::move(config)});
        recipe.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{description.format, 1}});
        return call<CreateEffect>(client, commands, std::move(recipe)).effect;
    };
    const auto surface_effect = effect_for(surface_a.description);
    const auto surface_effect_b = surface_a.description.format != surface_b.description.format
        ? effect_for(surface_b.description) : surface_effect;
    const auto job_a = startRenderJob(client, commands, a, scene, view, surface_a.target, surface_effect);
    const auto present = [&](RenderJobId job)
    {
        const auto before = server->stats().presentations;
        renderFrame(client, commands, job);
        const auto deadline = Clock::now() + 10s;
        while (server->stats().presentations == before)
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        CHECK(server->stats().presentations == before + 1);
    };
    const auto resized = [&](const TargetCreated& target, TargetDesc description, const TargetCreated& result)
    {
        CHECK(!result.error.type && result.output_available);
        CHECK(result.description == description && result.epoch == target.epoch + 1);
        call<SetTargetActive>(client, commands, {target.target, true, result.epoch});
    };
    const auto resize = [&](const TargetCreated& target, TargetDesc description)
    {
        resized(target, description, call<ResizeTarget>(client, commands, {target.target, description}));
    };
    const auto new_window_extent = [&](const TargetCreated& target, HWND window)
    {
        auto description = target.description;
        const auto extent = surface_desc(window);
        description.width = extent.width;
        description.height = extent.height;
        return description;
    };
    present(job_a);
    CHECK(SetWindowPos(windows[0], nullptr, 0, 0, 360, 260, SWP_NOMOVE | SWP_NOZORDER));
    resize(surface_a, new_window_extent(surface_a, windows[0]));
    const auto job_b = startRenderJob(client, commands, b, scene, view, surface_b.target, surface_effect_b);
    present(job_b);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    CHECK(SetWindowPos(windows[1], nullptr, 0, 0, 370, 270, SWP_NOMOVE | SWP_NOZORDER));
    const auto new_extent = new_window_extent(surface_b, windows[1]);
    const bool can_hold_native_wait = d3d12 || !DeviceRetirementProbe::present_retirement[1];
    if (can_hold_native_wait)
    {
        auto resizing = ipc::IpcCore::instance().createRequester(config.channel_name);
        CHECK(resizing);
        DeviceRetirementProbe::hold_queue_idle.store(true);
        auto pending = std::async(std::launch::async, [&]
        {
            return call<ResizeTarget>(*resizing, commands, {surface_b.target, new_extent});
        });
        const auto deadline = Clock::now() + 10s;
        while (!DeviceRetirementProbe::queue_idle_entered.load())
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::sleep_for(1ms);
        }
        present(job_a);
        CHECK(pending.wait_for(0s) == std::future_status::timeout);
        DeviceRetirementProbe::hold_queue_idle.store(false);
        DeviceRetirementProbe::hold_queue_idle.notify_all();
        resized(surface_b, new_extent, pending.get());
        std::puts("WSI QUEUE ISOLATION PASS: peer presented while the resizing device held its own queue wait");
    }
    else
    {
        resize(surface_b, new_extent);
        present(job_a);
    }
    present(job_b);
    stopAndRemoveJob(client, commands, job_b);
    call<RemoveTarget>(client, commands, surface_b.target);

    const TargetDesc offscreen{32, 32, rdesc::ETextureFormat::RGBA8_UNORM};
    const auto offscreen_effect = effect_for(offscreen);
    const auto off_a = call<CreateTarget>(client, commands, {a, offscreen});
    const auto off_b = call<CreateTarget>(client, commands, {b, offscreen});
    const auto off_job_a = startRenderJob(client, commands, a, scene, view, off_a.target, offscreen_effect);
    const auto off_job_b = startRenderJob(client, commands, b, scene, view, off_b.target, offscreen_effect);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    auto larger_offscreen = offscreen;
    larger_offscreen.width = 48;
    resize(off_a, larger_offscreen);
    resize(off_b, larger_offscreen);
    renderFrame(client, commands, off_job_a);
    DeviceRetirementProbe::arm.store(true);
    renderFrame(client, commands, off_job_b);
    CHECK(DeviceRetirementProbe::held.load());
    call<StopRenderJob>(client, commands, off_job_b);
    CHECK(call<RequestRenderJob>(client, commands, {off_job_b}, false).error.type == kJobStopped);
    stopAndRemoveJob(client, commands, off_job_b);
    CHECK(call<RequestRenderJob>(client, commands, {off_job_b}, false).error.type == kJobIdentity);
    call<RemoveTarget>(client, commands, off_b.target);

    auto deleting = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(deleting);
    auto removal = std::async(std::launch::async, [&]
    {
        return call<RemoveDeviceSession>(*deleting, commands, b, false);
    });
    const auto deadline = Clock::now() + 10s;
    while (!call<GetOrCreateResource>(client, commands, {b, DeviceScope{}, probe_type}, false).error.type)
    {
        CHECK(Clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
    CHECK(call<QueryResource>(client, commands, {b, DeviceScope{}, probe_type}, false).error.type == kDeviceBusy);
    CHECK(call<RemoveDeviceSession>(client, commands, b, false).error.type == kDeviceBusy);
    CHECK(call<CreateTarget>(client, commands, {b, offscreen}, false).error.type == kDeviceBusy);
    CHECK(removal.wait_for(0s) == std::future_status::timeout && !DeviceRetirementProbe::destroyed[1].load());
    present(job_a);
    renderFrame(client, commands, off_job_a);
    CHECK(DeviceRetirementProbe::held.load());
    call<SignalRetirementFence>(client, commands, {});
    CHECK(!removal.get().error.type && DeviceRetirementProbe::destroyed[1].load());
    CHECK(call<QueryRenderDevice>(client, commands, b, false).error.type == kRuntimeIdentity);
    CHECK(call<QueryResource>(client, commands, {a, DeviceScope{}, probe_type}).resource_id == probe_a);
    CHECK(probe_b != probe_a && !DeviceRetirementProbe::destroyed[0].load());
    present(job_a);

    const auto replacement = call<CreateDeviceSession>(client, commands, options).device;
    CHECK(replacement != b && replacement.index == b.index);
    call<GetOrCreateResource>(client, commands, {replacement, DeviceScope{}, probe_type});
    CHECK(adapters[2].uid.low == adapters[1].uid.low && adapters[2].uid.high == adapters[1].uid.high);
    const auto reopen_b = [&]
    {
        return call<CreateTarget>(
            client, commands, {replacement, surface_desc(windows[1]), SurfaceDescription{Win32Surface{windows[1]}}}
        );
    };
    const auto unused_target = reopen_b();
    const auto unused_job = startRenderJob(
        client, commands, replacement, scene, view, unused_target.target, surface_effect_b
    );
    const auto layout = call<QueryRenderJobInputLayout>(client, commands, unused_job).layout;
    auto other_layout = take(FrameLayout::create(layout.channels(), layout.revision()));
    auto stale = take(FrameData::create<std::tuple<>>(other_layout));
    CHECK(stale.seal());
    // The frame rejects the stale layout after Acquire and before submitting its acquire wait.
    CHECK(call<RequestRenderJob>(client, commands, {unused_job, &stale}, false).error.type == kFrameContract);
    stale.release();
    call<RemoveTarget>(client, commands, unused_target.target);
    call<RemoveRenderJob>(client, commands, unused_job);
    std::puts("WSI ACQUIRE PASS: removed and reopened the window after an unconsumed Acquire");

    const auto recreated = reopen_b();
    const auto final_job = startRenderJob(client, commands, replacement, scene, view, recreated.target, surface_effect_b);
    present(final_job);
    call<Stop>(client, commands, {});
    render.join();
    CHECK(std::ranges::all_of(DeviceRetirementProbe::destroyed, [](const auto& value) { return value.load(); }));
    const auto presents = server->stats().presentations;
    CHECK(server->stats().validation_errors == 0 && presents == 7);
    CHECK(PostMessageW(windows[0], WM_CLOSE, 0, 0));
    ui.join();
    std::printf("DEVICE LIFECYCLE PASS backend=%s presents=%llu dual_wsi=1 Offscreen resize/Stop/Shutdown\n",
        backend, presents);
    std::puts("DEVICE RETIREMENT PASS: peer submitted while GPU fence held, removal revoked only B, all borrows settled");
    return true;
}
#endif

int main(int argc, char** argv)
{
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    if (argc > 1 && std::string_view{argv[1]} == "--basic-features")
    {
        basicFeatureOutputs(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
#ifdef _WIN32
    if (argc > 1 && std::string_view{argv[1]} == "--wsi")
    {
        windowOutput(argc > 2 && std::string_view{argv[2]} == "d3d12");
        return 0;
    }
    if (argc > 1 && std::string_view{argv[1]} == "--dual-device-lifecycle")
        return physicalDeviceLifecycle(argc > 2 && std::string_view{argv[2]} == "d3d12") ? 0 : 77;
#endif
    Validation validation;
    if (argc > 1 && std::string_view{argv[1]} == "--device-lost")
    {
        deviceLostDelivery(validation, false);
        CHECK(validation.errors == 0);
        return 0;
    }
    {
        graphOutput(validation);
        importedExtentContracts(validation);
        importedIdentityReuse(validation);
        deviceLostDelivery(validation);
    }
    CHECK(validation.errors == 0);
}
