#include "NativeDevice.hpp"
#ifdef LUX_RENDER_GRAPH_ALLOCATION_PROBE
#include "GraphMeasurements.hpp"
#endif
#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include "FrameHarness.hpp"
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <string>
#include <thread>

#include "GraphCount.hpp"
#include "GraphDepthField.hpp"
#include "GraphFill.hpp"
#include "GraphHistory.hpp"
#include "GraphMrt.hpp"
#include "GraphPresent.hpp"
#include "GraphRed.hpp"
#include "GraphTriangle.hpp"
#include "HzbDownsample.hpp"

using namespace foundation_test;
namespace rdesc = lux::rdesc;

struct RecordTextureQueries final
{
    inline static decltype(nri::CoreInterface::GetTextureDesc) original = nullptr;
    inline static std::uint32_t count = 0;

    static const nri::TextureDesc& NRI_CALL get(const nri::Texture& texture)
    {
        ++count;
        return original(texture);
    }
};

static rdesc::ShaderProgramArtifact program(const rdesc::EmbeddedShader& shader, EGraphicsBackend backend)
{
    const auto target = backend == EGraphicsBackend::D3D12 ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto result = shader.load(target);
    CHECK(!result.stages.empty());
    return result;
}

static rdesc::ShaderProgramArtifact rasterProgram(const rdesc::EmbeddedShader& fragment, EGraphicsBackend backend)
{
    auto result = program(fragment, backend);
    auto vertex = program(kGraphTriangle, backend);
    result.stages.insert(result.stages.begin(), std::move(vertex.stages.front()));
    result.interface.entry_points.insert(
        result.interface.entry_points.begin(),
        vertex.interface.entry_points.begin(),
        vertex.interface.entry_points.end()
    );
    return result;
}

static PreparedRenderGraph compile(
    DeviceSession& session,
    const RenderGraph& graph,
    const RenderGraphAnalysis& analysis,
    const TargetDesc* target,
    ETransientAllocation allocation = ETransientAllocation::ALIAS_MEMORY,
    bool exercise_async = true
)
{
    std::optional<EffectOutputDesc> output;
    if (target) output.emplace(target->format, target->samples, target->kind, target->sampled);
    auto ready = stdexec::sync_wait(GraphCompiler::compile(
        session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis,
        output ? &*output : nullptr, allocation, {}, exercise_async
    ));
    CHECK(ready);
    return take(std::move(std::get<0>(*ready)));
}

static ResourceVersion written(const PassResult& pass, GraphResourceId resource)
{
    const auto found = std::ranges::find(pass.written, resource, &ResourceVersion::resource);
    CHECK(found != pass.written.end());
    return *found;
}

static std::uint32_t finishSubmissions(DeviceSession& session, FrameHarness& frames)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint32_t submitted = 0;
    // Poll counts depend on CPU speed and do not bound actual GPU progress.
    while (frames.submitting() && std::chrono::steady_clock::now() < deadline)
    {
        CHECK(session.progressCompletions());
        submitted += take(runTask(frames.render({}, {}))); // Continue the frozen window, not a fresh preparation.
        if (frames.submitting())
        {
            take(frames.waitForProgress(1'000'000));
        }
    }
    CHECK(!frames.submitting());
    return submitted;
}

static void checkPixels(
    DeviceSession& session,
    nri::Buffer& input,
    std::uint32_t width,
    std::uint32_t offset,
    std::array<unsigned, 4> expected,
    std::uint32_t row_pitch = 0
)
{
    const auto row = row_pitch ? row_pitch
                               : (width * 4 + session.description().memoryAlignment.uploadBufferTextureRow - 1) /
                                     session.description().memoryAlignment.uploadBufferTextureRow *
                                     session.description().memoryAlignment.uploadBufferTextureRow;
    const auto bytes = std::uint64_t(row) * width + offset;
    nri::Buffer* mapped_buffer = nullptr;
    CHECK(
        session.nriCore().CreatePlacedBuffer(
            session.device(),
            NriHostReadbackHeap,
            {bytes, 0, nri::BufferUsageBits::NONE},
            mapped_buffer
        ) == nri::Result::SUCCESS
    );
    CHECK(submitFixtureCommands(session, session.queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            const nri::BufferBarrierDesc barrier{
                &input,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
            };
            session.nriCore().CmdBarrier(command, {nullptr, 0, &barrier, 1});
            session.nriCore().CmdCopyBuffer(command, *mapped_buffer, 0, input, 0, bytes);
            return {};
        }
    ));
    CHECK(session.drain());
    const auto* data = static_cast<const unsigned char*>(session.nriCore().MapBuffer(*mapped_buffer, 0, bytes));
    CHECK(data);
    CHECK(session.invalidateMappedRange(*mapped_buffer, 0, bytes) == nri::Result::SUCCESS);
    for (std::uint32_t y = 1; y < width - 1; ++y)
    {
        for (std::uint32_t x = 1; x < width - 1; ++x)
        {
            for (unsigned channel = 0; channel < 4; ++channel)
            {
                const auto actual = data[offset + y * row + x * 4 + channel];
                if (std::abs(int(actual) - int(expected[channel])) > 1)
                {
                    std::fprintf(
                        stderr,
                        "pixel (%u,%u) channel=%u actual=%u expected=%u\n",
                        x,
                        y,
                        channel,
                        actual,
                        expected[channel]
                    );
                    CHECK(false);
                }
            }
        }
    }
    session.nriCore().UnmapBuffer(*mapped_buffer);
    session.nriCore().DestroyBuffer(mapped_buffer);
}

static void exportRows(
    RenderGraphBuilder& builder,
    ResourceVersion version,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    std::uint32_t offset = 0
)
{
    for (std::uint32_t row = 0; row < height; ++row)
    {
        CHECK(builder.exportOutput({version, GraphBufferRange{offset + std::uint64_t(row) * pitch, width * 4}}));
    }
}

static void imageChain(Validation& validation, EGraphicsBackend backend, std::uint32_t fif, bool async_compute = false)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = async_compute;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    GraphImageDesc image{64, 64};
    image.extent.source = EImageExtentSource::TARGET;
    auto seed_description = image;
    seed_description.mips = 2;
    const auto seed = take(builder.declareResource({"seed", seed_description}));
    const auto first = take(builder.declareResource({"first", image}));
    const auto second = take(builder.declareResource({"second", image}));
    const auto final = take(builder.declareResource({"final", image}));
    auto depth_description = image;
    depth_description.format = rdesc::ETextureFormat::D32_SFLOAT;
    const auto depth = take(builder.declareResource({"depth", depth_description}));
    const auto tint = take(builder.declareResource({"tint", GraphBufferDesc{16}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{128 * 512 + 256}}));
    const auto tint_pass = take(builder.addTransferPass("tint", TransferWork{{FillBuffer{{{tint, 0}}, 0x3f800000u}}}));
    ComputeWork compute;
    compute.allow_async_compute = async_compute;
    compute.interface.program =
        take(builder.addProgram(program(kGraphFill, backend)));
    compute.interface.resources = {{"generated", {{seed, 0}, GraphImageRange{1, 1, 1}, EGraphAccess::WRITE}}};
    compute.dispatch = DispatchArguments{{8, 8, 1}};
    const auto generated = take(builder.addComputePass("generate mip 1", std::move(compute)));
    RasterWork mrt;
    auto& fixed_mrt = mrt.fixed();
    fixed_mrt.interface.program = take(builder.addProgram(rasterProgram(kGraphMrt, backend)));
    fixed_mrt.interface.resources = {
        {"source_image", {generated.written.front(), GraphImageRange{1, 1, 1}}},
        {"tint", {tint_pass.written.front()}}
    };
    fixed_mrt.interface.samplers = {{"source_sampler", {0}}};
    mrt.colors = {{{{first, 0}}}, {{{second, 0}}}};
    mrt.depth = RasterAttachment{{{depth, 0}, GraphImageRange{2}}};
    fixed_mrt.graphics.depth_test = fixed_mrt.graphics.depth_write = true;
    const auto raster = take(builder.addRasterPass("MRT and depth", std::move(mrt)));
    RasterWork present;
    auto& fixed_present = present.fixed();
    fixed_present.interface.program = take(builder.addProgram(rasterProgram(kGraphPresent, backend)));
    fixed_present.interface.resources = {
        {"colors", {written(raster, first)}, {{written(raster, second)}}},
        {"depth_image", {written(raster, depth), GraphImageRange{2}}}
    };
    fixed_present.interface.samplers = {{"source_sampler", {0}}};
    present.colors = {{{{final, 0}}}};
    const auto resolved = take(builder.addRasterPass("postprocess", std::move(present)));
    const auto initialized =
        take(builder.addTransferPass("initialize resize copy capacity", TransferWork{{FillBuffer{{{pixels, 0}}}}}));
    const auto copied = take(builder.addTransferPass(
        "copy result",
        TransferWork{{CopyImageBuffer{
            {resolved.written.front()},
            {initialized.written.front(), GraphBufferRange{256, 128 * 512}, EGraphAccess::READ_WRITE}
        }}}
    ));
    const TargetDesc target{64, 64, image.format, 1, ETargetKind::OFFSCREEN};
    CHECK(builder.exportOutput({resolved.written.front(), {}, GraphOutputRequirements{target.format, 1}}));
    CHECK(builder.exportOutput({copied.written.front(), GraphBufferRange{256, 128 * 512}}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, &target);
    std::string dump;
    const DumpSink sink{
        &dump,
        [](void* pointer, std::string_view data) noexcept
        {
            static_cast<std::string*>(pointer)->append(data);
            return true;
        }
    };
    CHECK(dumpGraph(graph, analysis, EGraphDumpFormat::JSON, sink));
    CHECK(dumpPlan(plan, EGraphDumpFormat::JSON, sink));
    CHECK(dump.find("\"mip\":1") != std::string::npos);
    CHECK(dump.find("\"color_count\":2") != std::string::npos);
    CHECK(dump.find("\"lifetimes\":[") != std::string::npos);
    CHECK(dump.find("\"final_image_transitions\":[") != std::string::npos);
    nri::Descriptor* sampler = nullptr;
    nri::SamplerDesc sampler_description{};
    sampler_description.addressModes =
        {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};
    CHECK(session->nriCore().CreateSampler(session->device(), sampler_description, sampler) == nri::Result::SUCCESS);
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, fif))};
    auto instance = std::make_unique<RenderGraphInstance>(
        *session, fif, plan.executable, plan.programs, physical->output()
    );
    CHECK(instance->bindSampler(0, *sampler));
    CHECK(instance->initialize());
    CHECK(instance->dumpResources(sink));
    CHECK(dump.find("\"allocation_requirement_bytes\":") != std::string::npos);
    CHECK(dump.find("\"owned_command_buffers\":") != std::string::npos);
    if (const auto* path = std::getenv("LUX_GRAPH_DUMP"))
    {
        std::ofstream output(path, std::ios::app);
        output << dump << '\n';
        CHECK(output.good());
    }
    auto larger_target = target;
    larger_target.width = larger_target.height = 128;
    CHECK(plan.executable.compatible(larger_target));
    auto larger = std::optional{take(OffscreenTarget::create(*session, larger_target, fif))};
    auto second_instance = std::make_unique<RenderGraphInstance>(
        *session, fif, plan.executable, plan.programs, larger->output()
    );
    CHECK(second_instance->bindSampler(0, *sampler));
    CHECK(second_instance->initialize());
    const std::array instances{instance.get(), second_instance.get()};
    const auto submit = [&](std::span<RenderGraphInstance* const> selected)
    {
        auto submitted = take(runTask(frames->render(selected, {})));
        submitted += finishSubmissions(*session, *frames);
        return submitted;
    };
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        if (async_compute)
        {
            CHECK(submit(instances) > 0);
            CHECK(session->drain());
        }
        else
        {
            CHECK(submit(instances) == 1);
        }
    }
    if (!async_compute)
    {
        CHECK(take(runTask(frames->render(instances, {}))) == 0);
    }
    CHECK(session->drain());
    const auto submission_dump_offset = dump.size();
    CHECK(frames->dumpSubmissions(sink));
    CHECK(dump.find("\"instances\":2") != std::string::npos);
    if (const auto* path = std::getenv("LUX_GRAPH_DUMP"))
    {
        std::ofstream output(path, std::ios::app);
        output << std::string_view(dump).substr(submission_dump_offset);
        CHECK(output.good());
    }
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        checkPixels(*session, *instance->nativeBuffer(pixels.index, slot), 64, 256, {64, 64, 64, 255});
        checkPixels(*session, *second_instance->nativeBuffer(pixels.index, slot), 128, 256, {64, 64, 64, 255});
    }
    CHECK(submit(instances) > 0);
    instance.reset();
    physical.reset();
    CHECK(session->drain());
    const std::array remaining{second_instance.get()};
    CHECK(submit(remaining) > 0);
    second_instance.reset();
    larger.reset();
    CHECK(session->drain());
    session->nriCore().DestroyDescriptor(sampler);
}

static void indirectCounts(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 3);
    RenderGraphBuilder builder;
    const auto commands = take(builder.declareResource({"commands", GraphBufferDesc{128}}));
    const auto count = take(builder.declareResource({"count", GraphBufferDesc{128}}));
    const auto indices = take(builder.declareResource({"indices", GraphBufferDesc{12}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{32, 32}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{8192}}));
    const auto parameter =
        take(builder.declareResource({"requested count", GraphBufferDesc{4}, EGraphOrigin::IMPORTED}));
    const auto dispatch_arguments = take(builder.declareResource({"dispatch arguments", GraphBufferDesc{64}}));
    const auto dispatch_ready = take(builder.addTransferPass(
        "dispatch dimensions",
        TransferWork{{FillBuffer{{{dispatch_arguments, 0}, GraphBufferRange{16, 12}}, 1}}}
    ));
    ComputeWork compute;
    compute.interface.program =
        take(builder.addProgram(program(kGraphCount, backend)));
    compute.interface.resources = {
        {"commands", {{commands, 0}, GraphBufferRange{32, 60}, EGraphAccess::WRITE}},
        {"count", {{count, 0}, GraphBufferRange{64, 4}, EGraphAccess::WRITE}},
        {"indices", {{indices, 0}, {}, EGraphAccess::WRITE}}
    };
    compute.interface.parameters.values.resize(4);
    compute.interface.parameters.fields.push_back({"requested", parameter, "requested"});
    compute.dispatch = IndirectDispatchArguments{dispatch_ready.written.front(), 16};
    const auto generated = take(builder.addComputePass("GPU commands and count", std::move(compute)));
    RasterWork draw;
    auto& fixed_draw = draw.fixed();
    fixed_draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, backend)));
    draw.colors = {{{{color, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 0, 1}}};
    fixed_draw.draw = DrawIndexedIndirect{
        written(generated, indices),
        0,
        EIndexType::UINT32,
        written(generated, commands),
        32,
        3,
        20,
        written(generated, count),
        64
    };
    const auto drawn = take(builder.addRasterPass("indirect count", std::move(draw)));
    const auto copied = take(builder.addTransferPass(
        "copy result",
        TransferWork{{CopyImageBuffer{{drawn.written.front()}, {{pixels, 0}}, 256, 8192}}}
    ));
    exportRows(builder, copied.written.front(), 32, 32, 256);
    CHECK(builder.exportOutput({drawn.written.front(), {}, {}, EGraphUse::SAMPLED}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 3, plan.executable, plan.programs);
    std::uint32_t requested_value = 0;
    rdesc::ShaderBlock
        parameter_layout{4, 4, rdesc::EDataLayout::STD430, {{"requested", rdesc::EDataScalar::UINT, 0, 1, 1, 0, 0}}};
    instance->setResource(
        parameter.index,
        UINT32_MAX,
        {nullptr, 0, 0, std::as_bytes(std::span(&requested_value, 1)), &parameter_layout}
    );
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    std::uint64_t parameter_revision = 0;
    CHECK(!instance->recordBatch(0, 0)); // An initialized owner is not a prepared recording input.
    requested_value = 1;
    CHECK(take(instance->prepareFrame(0)));
    requested_value = 0; // Change the unversioned author borrow after its bytes have been prepared.
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    RecordTextureQueries::original = core.GetTextureDesc;
    RecordTextureQueries::count = 0;
    core.GetTextureDesc = &RecordTextureQueries::get;
    for (std::uint32_t batch = 0; batch < plan.executable.submissions().size(); ++batch)
    {
        const auto commands = take(instance->recordBatch(0, batch));
        CHECK(runTask(session->queue(plan.executable.submissions()[batch].queue).submit(commands)));
        CHECK(session->drain()); // This fixture serializes batches; normal queue dependencies are tested below.
    }
    CHECK(RecordTextureQueries::count == 0);
    requested_value = 1;
    CHECK(take(instance->prepareFrame(0)));
    CHECK(RecordTextureQueries::count == 0); // An unchanged fixed slice reuses its frozen native bindings.
    core.GetTextureDesc = RecordTextureQueries::original;
    checkPixels(*session, *instance->nativeBuffer(pixels.index, 0), 32, 0, {255, 0, 0, 255}, 256);
    CHECK(!instance->prepareFrame(UINT32_MAX));
    std::puts("FROZEN INPUT PASS author changed after Prepare, GPU uses prepared bytes, Record texture queries=0");
    for (std::uint32_t requested : {0u, 1u, 3u, 0u, 1u, 3u, 0u, 1u, 3u})
    {
        requested_value = requested;
        if (++parameter_revision > 3)
        {
            // The first three frames exercise an unversioned live borrow. Later frames wrap
            // every FIF slice with explicit parameter revisions and verify the actual GPU count.
            instance->setResource(
                parameter.index,
                UINT32_MAX,
                {nullptr, 0, 0, std::as_bytes(std::span(&requested_value, 1)), &parameter_layout, parameter_revision}
            );
        }
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        checkPixels(
            *session,
            *instance->nativeBuffer(pixels.index, instance->progress().last_submitted_slot),
            32,
            0,
            {requested ? 255u : 0u, 0, 0, 255},
            256
        );
    }
    instance.reset();
    CHECK(session->drain());
}

static void resolveAndCopy(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 2);
    RenderGraphBuilder builder;
    GraphImageDesc multisample{32, 32};
    multisample.samples = 4;
    const auto source = take(builder.declareResource({"MSAA", multisample}));
    const auto resolved = take(builder.declareResource({"resolve", GraphImageDesc{32, 32}}));
    const auto rectangle = take(builder.declareResource({"rectangle", GraphImageDesc{16, 16}}));
    const auto roundtrip = take(builder.declareResource({"roundtrip", GraphImageDesc{16, 16}}));
    const auto packed = take(builder.declareResource({"packed", GraphBufferDesc{4096}}));
    const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{4352}}));
    RasterWork draw;
    auto& fixed_draw = draw.fixed();
    fixed_draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, backend)));
    RasterAttachment attachment{{{source, 0}}};
    attachment.resolve = ResourceAccess{{resolved, 0}};
    draw.colors.push_back(attachment);
    const auto raster = take(builder.addRasterPass("multisample resolve", std::move(draw)));
    const auto clear =
        take(builder.addTransferPass("initialize rectangle", TransferWork{{ClearImage{{{rectangle, 0}}}}}));
    const auto copied = take(builder.addTransferPass(
        "copy rectangle",
        TransferWork{{CopyImage{
            {written(raster, resolved)},
            {clear.written.front()},
            ImageCopyRegion{8, 8, 16, 16},
            ImageCopyRegion{0, 0, 16, 16}
        }}}
    ));
    const auto stored = take(builder.addTransferPass(
        "texture to buffer",
        TransferWork{{CopyImageBuffer{{copied.written.front()}, {{packed, 0}}, 256, 4096}}}
    ));
    const auto uploaded = take(builder.addTransferPass(
        "buffer to texture",
        TransferWork{{CopyImageBuffer{{stored.written.front()}, {{roundtrip, 0}}, 256, 4096}}}
    ));
    const auto observed = take(builder.addTransferPass(
        "result",
        TransferWork{{CopyImageBuffer{{uploaded.written.front()}, {{pixels, 0}, GraphBufferRange{256, 4096}}, 256, 4096}
        }}
    ));
    exportRows(builder, observed.written.front(), 16, 16, 256, 256);
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    CHECK(take(runTask(frames->render(instances, {}))) == 1);
    CHECK(session->drain());
    checkPixels(*session, *instance->nativeBuffer(pixels.index, 0), 16, 256, {255, 0, 0, 255}, 256);
    instance.reset();
    CHECK(session->drain());
}

template <std::size_t Count = 9>
static std::array<std::uint32_t, Count> readHistoryResult(DeviceSession& session, nri::Buffer& input)
{
    constexpr auto size = sizeof(std::array<std::uint32_t, Count>);
    nri::Buffer* read = nullptr;
    const auto& core = session.nriCore();
    CHECK(
        core.CreatePlacedBuffer(session.device(), NriHostReadbackHeap, {size, 0, nri::BufferUsageBits::NONE}, read) ==
        nri::Result::SUCCESS
    );
    CHECK(submitFixtureCommands(session, session.queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            const nri::BufferBarrierDesc transition{
                &input,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY}
            };
            core.CmdBarrier(command, {nullptr, 0, &transition, 1});
            core.CmdCopyBuffer(command, *read, 0, input, 0, size);
            return {};
        }
    ));
    CHECK(session.drain());
    const void* bytes = core.MapBuffer(*read, 0, size);
    CHECK(bytes);
    CHECK(session.invalidateMappedRange(*read, 0, size) == nri::Result::SUCCESS);
    std::array<std::uint32_t, Count> result{};
    std::memcpy(result.data(), bytes, size);
    core.UnmapBuffer(*read);
    core.DestroyBuffer(read);
    return result;
}

struct HeldCompletion final
{
    inline static decltype(nri::CoreInterface::GetFenceValue) original = nullptr;
    inline static std::uint64_t completed = 0;

    static std::uint64_t NRI_CALL get(nri::Fence& fence)
    {
        return std::min(original(fence), completed);
    }
};

struct RecordingGate final
{
    inline static decltype(nri::CoreInterface::CmdZeroBuffer) original = nullptr;
    inline static std::atomic<unsigned> entered{0};
    inline static std::atomic<bool> released{false};

    static void NRI_CALL zero(nri::CommandBuffer& command, nri::Buffer& buffer, std::uint64_t offset, std::uint64_t size)
    {
        entered.fetch_add(1);
        released.wait(false);
        original(command, buffer, offset, size);
    }
};

struct EndFailure final
{
    inline static decltype(nri::CoreInterface::EndCommandBuffer) original = nullptr;
    inline static bool fail = false;
    inline static unsigned calls = 0;

    static nri::Result NRI_CALL end(nri::CommandBuffer& command)
    {
        ++calls;
        const auto result = original(command);
        if (std::exchange(fail, false) && result == nri::Result::SUCCESS)
        {
            return nri::Result::INVALID_ARGUMENT;
        }
        return result;
    }
};

static void recordingOwnership(Validation& validation, bool d3d12)
{
    DeviceOptions options;
    options.multiple_queues = true;
    if (d3d12)
    {
        options.backend = EGraphicsBackend::D3D12;
    }
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    RecordingGate::original = core.CmdZeroBuffer;
    RecordingGate::entered = 0;
    RecordingGate::released = false;
    core.CmdZeroBuffer = &RecordingGate::zero;
    RenderGraphBuilder builder;
    const auto buffer = take(builder.declareResource({"recording", GraphBufferDesc{16}}));
    CHECK(builder.addTransferPass("recording", TransferWork{{FillBuffer{{{buffer, 0}}, 0}}}, {true}));
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
    CHECK(instance->initialize());
    CHECK(take(instance->prepareFrame(0)));
    CHECK(take(instance->prepareFrame(1)));
    RenderResult<std::span<nri::CommandBuffer* const>> first, second;
    const auto waitEntered = [&](unsigned count)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (RecordingGate::entered.load() < count)
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    };
    std::thread first_worker([&] { first = instance->recordBatch(0, 0); });
    waitEntered(1);
    const auto duplicate = instance->recordBatch(0, 0);
    CHECK(!duplicate && duplicate.error().type == kDeviceBusy);
    CHECK(RecordingGate::entered == 1);
    std::thread second_worker([&] { second = instance->recordBatch(1, 0); });
    waitEntered(2); // Distinct FIF allocators can record concurrently.
    RecordingGate::released.store(true);
    RecordingGate::released.notify_all();
    first_worker.join();
    second_worker.join();
    CHECK(first && second && first->front() != second->front());

    auto frames = std::make_unique<FrameHarness>(*session, 2);
    const std::array instances{instance.get()};
    HeldCompletion::original = core.GetFenceValue;
    HeldCompletion::completed = 0;
    core.GetFenceValue = &HeldCompletion::get;
    CHECK(take(runTask(frames->render(instances, {}))) != 0);
    CHECK(take(runTask(frames->render(instances, {}))) != 0);
    const auto recorded = RecordingGate::entered.load();
    std::uint32_t slot = UINT32_MAX;
    CHECK(!frames->beginFrame(slot) && slot == UINT32_MAX);
    CHECK(take(runTask(frames->render(instances, {}))) == 0);
    CHECK(RecordingGate::entered == recorded); // Accepted use without observed completion cannot reset/re-record a FIF slot.
    core.GetFenceValue = HeldCompletion::original;
    CHECK(session->drain());
    CHECK(take(runTask(frames->render(instances, {}))) != 0);
    CHECK(RecordingGate::entered > recorded);
    CHECK(session->drain());

    // All real work is complete before injecting a terminal result on a healthy device.
    const auto submit = core.QueueSubmit;
    core.QueueSubmit = [](nri::Queue&, const nri::QueueSubmitDesc&) noexcept -> nri::Result
    { return nri::Result::DEVICE_LOST; };
    const auto lost = runTask(frames->render(instances, {}));
    core.QueueSubmit = submit;
    CHECK(!lost && session->deviceLost());
    const auto after_loss = RecordingGate::entered.load();
    CHECK(!frames->beginFrame(slot));
    const auto rejected = runTask(frames->render(instances, {}));
    CHECK(!rejected && rejected.error() == lost.error());
    CHECK(!instance->prepareFrame(0));
    CHECK(!instance->recordBatch(0, 0));
    CHECK(RecordingGate::entered == after_loss);
    core.CmdZeroBuffer = RecordingGate::original;
    (void)session->progressCompletions();
    frames.reset();
    instance.reset();
    std::printf(
        "RECORD OWNER PASS backend=%s: parallel batches/slices, same-batch rejection, FIF completion gate, terminal "
        "rejection\n",
        d3d12 ? "D3D12" : "Vulkan"
    );
}

static GraphProgram dualTargetRasterProgram()
{
    GraphProgram result;
    for (auto target : {rdesc::EShaderTarget::SPIRV, rdesc::EShaderTarget::DXIL})
    {
        auto fragment = kGraphRed.load(target);
        auto vertex = kGraphTriangle.load(target);
        fragment.stages.insert(fragment.stages.begin(), std::move(vertex.stages.front()));
        fragment.interface.entry_points.insert(fragment.interface.entry_points.begin(),
            vertex.interface.entry_points.begin(), vertex.interface.entry_points.end());
        result.targets.push_back(std::move(fragment));
    }
    return result;
}

static void typedRasterScopes(Validation& validation, bool d3d12)
{
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    {
        RenderGraphBuilder builder;
        const auto color = take(builder.declareResource({"scoped color", GraphImageDesc{64, 64, 2, 2}}));
        const auto depth = take(builder.declareResource({"scoped depth",
            GraphImageDesc{64, 64, 2, 2, rdesc::ETextureFormat::D32_SFLOAT}}));
        const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{8192}}));
        const auto shader = take(builder.addProgram(dualTargetRasterProgram()));
        RasterWork clear;
        clear.fixed().interface.program = shader;
        CHECK(!builder.addRasterPass("empty scope", clear));
        clear.colors = {{{{color, 0}, GraphImageRange{1, 1, 1, 1, 1}},
            EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 1, 0, 1}}};
        clear.depth = RasterAttachment{
            {{depth, 0}, GraphImageRange{2, 1, 1, 1, 1}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE
        };
        clear.fixed().graphics.depth_test = clear.fixed().graphics.depth_write = true;
        clear.fixed().graphics.depth_compare = EDepthCompare::ALWAYS;
        const auto drawn = take(builder.addRasterPass("clear and draw", std::move(clear)));
        RasterWork load;
        load.viewport = RasterViewport{0, 0, 0.25f, 1};
        load.fixed().interface.program = shader;
        load.fixed().graphics.depth_write = true;
        load.colors = {{{written(drawn, color), GraphImageRange{1, 1, 1, 1, 1}},
            EAttachmentLoad::LOAD, EAttachmentStore::STORE}};
        load.depth = RasterAttachment{
            {written(drawn, depth), GraphImageRange{2, 1, 1, 1, 1}}, EAttachmentLoad::LOAD, EAttachmentStore::STORE
        };
        const auto ordinary = take(builder.addRasterPass("load and draw", std::move(load)));
        const auto copied = take(builder.addTransferPass("scoped pixels", TransferWork{
            {CopyImageBuffer{{written(ordinary, color), GraphImageRange{1, 1, 1, 1, 1}}, {{pixels, 0}}, 256, 8192}}
        }));
        exportRows(builder, copied.written.front(), 32, 32, 256);
        const auto graph = take(std::move(builder).finish());
        const auto analysis = take(analyzeRenderGraph(graph));
        auto plan = compile(*session, graph, analysis, nullptr);
        CHECK(plan.executable.passes()[0].rendering_group == 0);
        CHECK(plan.executable.passes()[1].rendering_group == plan.executable.passes()[0].rendering_group);
        CHECK(plan.programs[0].pipeline != plan.programs[1].pipeline);
        const auto duplicate = compile(*session, graph, analysis, nullptr);
        CHECK(duplicate.programs[0].pipeline == plan.programs[0].pipeline);
        CHECK(duplicate.programs[1].pipeline == plan.programs[1].pipeline);
        auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
        CHECK(instance->initialize());
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1 && session->drain());
        checkPixels(*session, *instance->nativeBuffer(pixels.index, 0), 32, 0, {255, 0, 0, 255}, 256);
        instance.reset();
        CHECK(session->drain());
    }
    {
        RenderGraphBuilder builder;
        const auto color = take(builder.declareResource({"MSAA color",
            GraphImageDesc{32, 32, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM, 4}}));
        const auto resolved = take(builder.declareResource({"resolved color", GraphImageDesc{32, 32}}));
        const auto pixels = take(builder.declareResource({"resolve pixels", GraphBufferDesc{8192}}));
        RasterWork raster;
        raster.fixed().interface.program = take(builder.addProgram(dualTargetRasterProgram()));
        RasterAttachment attachment{{{color, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE};
        attachment.resolve = ResourceAccess{{resolved, 0}};
        raster.colors.push_back(attachment);
        const auto drawn = take(builder.addRasterPass("resolve", std::move(raster)));
        const auto copied = take(builder.addTransferPass("resolve readback", TransferWork{
            {CopyImageBuffer{{written(drawn, resolved)}, {{pixels, 0}}, 256, 8192}}
        }));
        exportRows(builder, copied.written.front(), 32, 32, 256);
        const auto graph = take(std::move(builder).finish());
        auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
        auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
        CHECK(instance->initialize());
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1 && session->drain());
        checkPixels(*session, *instance->nativeBuffer(pixels.index, 0), 32, 0, {255, 0, 0, 255}, 256);
        instance.reset();
        CHECK(session->drain());
    }
    std::puts("TYPED RASTER PASS: target shaders, color/depth mip/layer, merged LOAD scope, MSAA resolve");
}

static void typedRecording(Validation& validation, bool d3d12)
{
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    {
        // Dispatch extents must refer to a materialized graph image, as before the compiler unification.
        RenderGraphBuilder builder;
        const auto unused = take(builder.declareResource({"unused extent", GraphImageDesc{8, 8}}));
        const auto output = take(builder.declareResource({"output", GraphImageDesc{8, 8}}));
        ComputeWork work;
        work.interface.program = take(builder.addProgram(GraphProgram{kGraphFill}));
        work.interface.resources = {{"generated", {{output, 0}, {}, EGraphAccess::WRITE}}};
        work.dispatch = ImageDispatchArguments{unused};
        const auto pass = take(builder.addComputePass("invalid extent dependency", std::move(work)));
        CHECK(builder.exportOutput({pass.written.front()}));
        const auto graph = take(std::move(builder).finish());
        const auto analysis = take(analyzeRenderGraph(graph));
        auto rejected = stdexec::sync_wait(GraphCompiler::compile(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), graph, analysis
        ));
        CHECK(rejected && !std::get<0>(*rejected) && std::get<0>(*rejected).error().type == kDeviceArgument);
    }
    {
        RenderGraphBuilder builder;
        const auto source = take(builder.declareResource({"source", GraphBufferDesc{16}}));
        const auto output = take(builder.declareResource({"output", GraphBufferDesc{16}}));
        const auto fill = take(builder.addTransferPass("fill", TransferWork{{FillBuffer{{{source, 0}}, 7}}}));
        const auto copy = take(builder.addTransferPass(
            "copy", TransferWork{{CopyBuffer{{fill.written.front()}, {{output, 0}}}}}
        ));
        CHECK(builder.exportOutput({copy.written.front()}));
        auto plan = [&]
        {
            const auto graph = take(std::move(builder).finish());
            return compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
        }();
        auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
        CHECK(instance->initialize());
        CHECK(take(instance->prepareFrame(0)));
        auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
        EndFailure::original = core.EndCommandBuffer;
        EndFailure::fail = true;
        EndFailure::calls = 0;
        core.EndCommandBuffer = &EndFailure::end;
        const auto rejected = instance->recordBatch(0, 0);
        CHECK(!rejected && session->queue().submitted() == 0 && EndFailure::calls == 1);
        CHECK(instance->recordBatch(0, 0) && EndFailure::calls == 2);
        CHECK(instance->recordBatch(0, 0) && EndFailure::calls == 2);
        core.EndCommandBuffer = EndFailure::original;
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto values = readHistoryResult<4>(*session, *instance->nativeBuffer(output.index, 0));
        CHECK(std::ranges::all_of(values, [](auto value) { return value == 7; }));
        instance.reset();
        CHECK(session->drain());
    }
    std::printf("TYPED RECORDING PASS backend=%s: detached source, End failure and retry, GPU buffer results\n",
        d3d12 ? "D3D12" : "Vulkan");
}

struct ComputeSequenceFixture final
{
    RenderGraph graph;
    GraphResourceId intermediate;
    GraphResourceId readback;
};

static ComputeSequenceFixture computeSequenceGraph()
{
    RenderGraphBuilder builder;
    const auto seed = take(builder.declareResource({"seed", GraphBufferDesc{16}}));
    const auto intermediate = take(builder.declareResource({"intermediate", GraphBufferDesc{16}}));
    const auto result = take(builder.declareResource({"result", GraphBufferDesc{16}}));
    const auto readback = take(builder.declareResource({"readback", GraphBufferDesc{16}}));
    const auto initialized = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{seed, 0}}, 11}}}));
    const auto program = take(builder.addProgram(GraphProgram{kGraphHistory}));
    const auto dispatch = [program](ResourceVersion input, GraphResourceId output)
    {
        ComputeWork work;
        work.interface.program = program;
        work.interface.resources = {
            {"previous_sample", {input}},
            {"current_sample", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        return work;
    };
    const auto unused = take(builder.declareResource({"unused", GraphBufferDesc{16}}));
    CHECK(builder.addComputePass("culled", dispatch(initialized.written.front(), unused)));
    const auto next = take(builder.nextWriteVersion(intermediate));
    auto invalid = dispatch(next, result);
    invalid.interface.program = kGraphInvalidIndex;
    CHECK(!builder.addComputeSequence("invalid", {dispatch(initialized.written.front(), intermediate), invalid}));
    CHECK(builder.passCount() == 2 && take(builder.nextWriteVersion(intermediate)) == next);
    const auto sequence = take(builder.addComputeSequence(
        "two increments", {dispatch(initialized.written.front(), intermediate), dispatch(next, result)}
    ));
    const auto copied = take(builder.addTransferPass(
        "readback", TransferWork{{CopyBuffer{{written(sequence, result)}, {{readback, 0}}}}}
    ));
    CHECK(builder.exportOutput({copied.written.front()}));
    return {take(std::move(builder).finish()), intermediate, readback};
}

static void checkComputeSequence(const RenderGraphExecutable& executable, GraphResourceId intermediate)
{
    CHECK(executable.recordingUnits().size() == 3);
    CHECK(executable.recordingUnits()[1].pass_count == 2);
    const auto passes = executable.passes();
    CHECK(passes.size() == 4);
    CHECK(passes[1].submission == passes[2].submission);
    CHECK(std::ranges::any_of(passes[2].before.buffers, [&](const auto& barrier)
    {
        return barrier.resource == intermediate.index;
    }));
}

static void multiCommandCompute(Validation& validation, bool d3d12)
{
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    GraphResourceId intermediate, readback;
    auto executable = [&]
    {
        const auto fixture = computeSequenceGraph();
        intermediate = fixture.intermediate;
        readback = fixture.readback;
        const auto analysis = take(analyzeRenderGraph(fixture.graph));
        return compile(*session, fixture.graph, analysis, nullptr);
    }();
    checkComputeSequence(executable.executable, intermediate);
    auto instance = std::make_unique<RenderGraphInstance>(*session, 2, executable.executable, executable.programs);
    CHECK(instance->initialize());
    auto frames = std::make_unique<FrameHarness>(*session, 2);
    const std::array instances{instance.get()};
    CHECK(take(runTask(frames->render(instances, {}))) == 1);
    CHECK(session->drain());
    const auto values = readHistoryResult<4>(*session, *instance->nativeBuffer(readback.index, 0));
    CHECK(std::ranges::all_of(values, [](auto value) { return value == 13; }));
    instance.reset();
    CHECK(session->drain());
}

// This recorder implements the fixture's one known kernel and Buffer operations.
class ComputeSequenceRecorder final
{
public:
    explicit ComputeSequenceRecorder(const RenderGraphExecutable& executable)
        : executable_(executable), buffers_(executable.resources().size())
    {
        for (std::size_t i = 0; i < buffers_.size(); ++i)
        {
            const auto& resource = executable.resources()[i];
            if (!resource.memory_used) continue;
            const auto& buffer = std::get<GraphBufferDesc>(resource.description);
            CHECK(buffer.size % sizeof(Word) == 0);
            buffers_[i].resize(buffer.size / sizeof(Word));
        }
    }

    void execute(std::span<const std::uint8_t> conditions = {})
    {
        const auto& program = executable_.recordingProgram();
        for (const auto& batch : executable_.submissions())
        {
            for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
            {
                const auto& pass = program.passes[p];
                if (pass.condition_region != kGraphInvalidIndex)
                {
                    const auto& predicate = program.conditions[pass.condition_region];
                    CHECK(predicate.source == EConditionSource::INPUT && predicate.input_slot < conditions.size());
                    if (static_cast<bool>(conditions[predicate.input_slot]) != predicate.expected) continue;
                }
                CHECK(recordBlock(program, pass.commands, [&](const auto& packet) noexcept
                {
                    return replay(packet, pass);
                }));
            }
        }
    }

    void verify(GraphResourceId readback, std::span<const std::uint32_t> expected) const
    {
        CHECK(std::ranges::equal(buffers_[readback.index], expected));
    }

private:
    using Word = std::uint32_t;

    auto words(std::uint32_t resource, const VGraphRange& range) noexcept
    {
        std::span result(buffers_[resource]);
        if (std::holds_alternative<std::monostate>(range)) return result;
        const auto& bytes = std::get<GraphBufferRange>(range);
        CHECK(bytes.offset % sizeof(Word) == 0 && bytes.size % sizeof(Word) == 0);
        const auto offset = bytes.offset / sizeof(Word);
        const auto count = bytes.size / sizeof(Word);
        CHECK(offset <= result.size() && count <= result.size() - offset);
        return result.subspan(offset, count);
    }

    RenderResult<void> replay(const RasterCommands&, const CompiledPass&) noexcept
    {
        return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
    }

    RenderResult<void> replay(const TransferCommands& packet, const CompiledPass&) noexcept
    {
        for (const auto& operation : packet.operations)
        {
            if (const auto* fill = std::get_if<FillBuffer>(&operation))
            {
                std::ranges::fill(words(fill->destination.version.resource.index, fill->destination.range), fill->value);
                continue;
            }
            if (const auto* copy = std::get_if<CopyBuffer>(&operation))
            {
                auto source = words(copy->source.version.resource.index, copy->source.range);
                auto destination = words(copy->destination.version.resource.index, copy->destination.range);
                CHECK(source.size() == destination.size());
                CHECK(std::ranges::all_of(source, [](const auto& word) { return word.has_value(); }));
                std::ranges::copy(source, destination.begin());
                continue;
            }
            return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
        }
        return {};
    }

    RenderResult<void> replay(const ComputeCommands& packet, const CompiledPass& pass) noexcept
    {
        const auto* dispatch = std::get_if<DispatchArguments>(&packet.dispatch);
        CHECK(dispatch && dispatch->groups == (std::array{1u, 1u, 1u}));
        const auto uses = executable_.descriptorUses(pass.binding_slot);
        CHECK(uses.size() == 2);
        // The production layout packs private fields by name, placing current_sample before previous_sample.
        const auto output = std::ranges::find(uses, 0u, &DescriptorUse::binding);
        const auto input = std::ranges::find(uses, 1u, &DescriptorUse::binding);
        CHECK(input != uses.end() && output != uses.end());
        auto previous = words(input->resource, input->whole_resource ? VGraphRange{} : input->range);
        auto current = words(output->resource, output->whole_resource ? VGraphRange{} : output->range);
        CHECK(previous.size() == 4 && current.size() == 4);
        for (std::size_t i = 0; i < current.size(); ++i)
        {
            CHECK(previous[i]);
            current[i] = *previous[i] + 1;
        }
        return {};
    }

    const RenderGraphExecutable& executable_;
    std::vector<std::vector<std::optional<Word>>> buffers_;
};

static RenderGraphExecutable
compileCpuGraph(const RenderGraph& graph, GraphQueuePolicy policy = {}, bool alias_queues = false)
{
    nri::DeviceDesc capabilities{};
    capabilities.graphicsAPI = nri::GraphicsAPI::VK;
    capabilities.dimensions.texture2DMaxDim = 4096;
    capabilities.dimensions.textureLayerMaxNum = 256;
    capabilities.shaderStage.compute.dispatchMaxDim[0] = 1;
    capabilities.shaderStage.compute.dispatchMaxDim[1] = 1;
    capabilities.shaderStage.compute.dispatchMaxDim[2] = 1;
    const QueueProperties graphics;
    const QueueProperties compute{nri::QueueType::COMPUTE, 1};
    const QueueProperties copy{nri::QueueType::COPY, 2};
    const std::array queues{&graphics, alias_queues ? &graphics : &compute, &copy};
    return take(GraphCompiler::compileForRecordingTest(
        capabilities, queues, graph, take(analyzeRenderGraph(graph)), ETransientAllocation::INDEPENDENT, policy
    ));
}

static void cpuRecordingTables()
{
    GraphResourceId intermediate, readback;
    const auto executable = [&]
    {
        const auto fixture = computeSequenceGraph();
        intermediate = fixture.intermediate;
        readback = fixture.readback;
        return compileCpuGraph(fixture.graph);
    }();
    checkComputeSequence(executable, intermediate);
    ComputeSequenceRecorder recorder(executable);
    recorder.execute();
    recorder.verify(readback, std::array{13u, 13u, 13u, 13u});
    std::puts("CPU recording: expired source, culled dispatch, in-pass barrier and four values of 13");
}

static void cpuConditionalRanges()
{
    RenderGraphBuilder builder;
    const auto condition = take(builder.declareCondition("overwrite middle"));
    const auto data = take(builder.declareResource({"data", GraphBufferDesc{16}}));
    const auto readback = take(builder.declareResource({"observed", GraphBufferDesc{16}}));
    const auto seed = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{data, 0}}, 3}}})).written.front();
    const auto updated = take(builder.addTransferPass(
        "update", TransferWork{{FillBuffer{{seed, GraphBufferRange{4, 8}, EGraphAccess::WRITE, 0, seed}, 9}}},
        {false, PassCondition{EConditionSource::INPUT, condition}}
    )).written.front();
    const auto copied =
        take(builder.addTransferPass("observe", TransferWork{{CopyBuffer{{updated}, {{readback, 0}}}}}));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto executable = compileCpuGraph(take(std::move(builder).finish()));
    CHECK(executable.recordingProgram().conditions.size() == 1);
    for (const std::uint8_t enabled : {0, 1})
    {
        ComputeSequenceRecorder recorder(executable);
        recorder.execute({&enabled, 1});
        const auto middle = enabled ? 9u : 3u;
        recorder.verify(readback, std::array{3u, middle, middle, 3u});
    }
}

static void cpuImportedBoundaries()
{
    {
        RenderGraphBuilder builder;
        const auto input = take(builder.declareResource(
            {"unchanged", GraphBufferDesc{16}, EGraphOrigin::IMPORTED, {GraphBufferRange{4, 8}}}
        ));
        CHECK(builder.exportOutput({{input, 0}, GraphBufferRange{4, 8}}));
        const auto executable = compileCpuGraph(take(std::move(builder).finish()));
        CHECK(executable.passes().empty());
        CHECK(executable.recordingUnits().empty());
        CHECK(std::ranges::all_of(executable.submissions(), [](const auto& batch) { return batch.pass_count == 0; }));
    }
    {
        RenderGraphBuilder builder;
        const GraphImageRange range{1, 1, 1};
        const GraphImageDesc image{64, 64, 2, 1, rdesc::ETextureFormat::R32_SFLOAT};
        const auto source = take(builder.declareResource(
            {"partially imported", image, EGraphOrigin::IMPORTED, {range}, EGraphUse::SAMPLED}
        ));
        const auto destination = take(builder.declareResource({"copied", image}));
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = take(builder.addProgram(GraphProgram{kHzbDownsample}));
        work.interface.resources = {
            {"uSrc", {{source, 0}, range}},
            {"uDst", {{destination, 0}, range, EGraphAccess::WRITE}}
        };
        const auto written = take(builder.addComputePass("read imported subresource", std::move(work)));
        CHECK(builder.exportOutput({written.written.front(), range}));
        const auto executable = compileCpuGraph(take(std::move(builder).finish()), {.exercise_async = true});
        CHECK(executable.submissions().size() == 1);
        CHECK(executable.submissions().front().queue == EQueueRole::GRAPHICS);
        const auto& after = executable.recordingProgram().after.images;
        const auto restored = std::ranges::find(after, source.index, &ImageTransition::resource);
        CHECK(restored != after.end());
        CHECK(restored->boundary == ETransitionBoundary::RESTORE_IMPORTED);
        CHECK(restored->native.mipOffset == 1 && restored->native.mipNum == 1);
        CHECK(restored->native.layerOffset == 0 && restored->native.layerNum == 1);
    }
}

static void cpuQueueBoundaries()
{
    RenderGraphBuilder builder;
    const auto input = take(builder.declareResource({"input", GraphBufferDesc{16}, EGraphOrigin::IMPORTED, {{}}}));
    const auto output = take(builder.declareResource({"output", GraphBufferDesc{16}}));
    ComputeWork work;
    work.allow_async_compute = true;
    work.interface.program = take(builder.addProgram(GraphProgram{kGraphHistory}));
    work.interface.resources = {
        {"previous_sample", {{input, 0}}},
        {"current_sample", {{output, 0}, {}, EGraphAccess::WRITE}}
    };
    const auto written = take(builder.addComputePass("compute only", std::move(work)));
    CHECK(builder.exportOutput({written.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto baseline = compileCpuGraph(graph);
    CHECK(baseline.submissions().size() == 1 && baseline.submissions().front().queue == EQueueRole::GRAPHICS);
    const auto executable = compileCpuGraph(graph, {.exercise_async = true});
    CHECK(executable.submissions().size() == 1);
    CHECK(executable.submissions().front().queue == EQueueRole::COMPUTE);
    CHECK(executable.submissions().front().dependencies.empty());
}

struct QueueScheduleFixture final
{
    RenderGraph graph;
    GraphResourceId compute_result, graphics_result;
};

static QueueScheduleFixture queueScheduleFixture(
    EGraphicsBackend backend, std::uint64_t branch_ns, bool serialize
)
{
    RenderGraphBuilder builder;
    const auto seed = take(builder.declareResource({"seed", GraphBufferDesc{16}}));
    const auto compute_result = take(builder.declareResource({"compute result", GraphBufferDesc{16}}));
    const auto graphics_result = take(builder.declareResource({"graphics result", GraphBufferDesc{16}}));
    const auto initialized = take(builder.addTransferPass(
        "seed", TransferWork{{FillBuffer{{{seed, 0}}, 3}}}, {.estimated_gpu_ns = 100}
    ));
    ComputeWork work;
    work.allow_async_compute = true;
    work.interface.program = take(builder.addProgram(program(kGraphHistory, backend)));
    work.interface.resources = {
        {"previous_sample", {initialized.written.front()}},
        {"current_sample", {{compute_result, 0}, {}, EGraphAccess::WRITE}}
    };
    const auto compute = take(builder.addComputePass("compute", std::move(work), {.estimated_gpu_ns = branch_ns}));
    const auto graphics = take(builder.addTransferPass(
        "independent graphics work", TransferWork{{FillBuffer{{{graphics_result, 0}}, 7}}},
        {.estimated_gpu_ns = branch_ns}
    ));
    if (serialize)
    {
        CHECK(builder.dependsOn(compute.pass, graphics.pass));
    }
    CHECK(builder.exportOutput({compute.written.front()}));
    CHECK(builder.exportOutput({graphics.written.front()}));
    return {take(std::move(builder).finish()), compute_result, graphics_result};
}

static void cpuQueueCandidates()
{
    constexpr auto backend = EGraphicsBackend::VULKAN;
    constexpr GraphQueuePolicy calibrated{{1'000, 1'000, 1'000}};
    const auto heavy = queueScheduleFixture(backend, 80'000, false);
    const auto serial = queueScheduleFixture(backend, 80'000, true);
    const auto light = queueScheduleFixture(backend, 500, false);
    const auto unknown = queueScheduleFixture(backend, 0, false);
    CHECK(compileCpuGraph(heavy.graph).submissions().size() == 1);
    CHECK(compileCpuGraph(serial.graph, calibrated).submissions().size() == 1);
    CHECK(compileCpuGraph(light.graph, calibrated).submissions().size() == 1);
    CHECK(compileCpuGraph(unknown.graph, calibrated).submissions().size() == 1);
    CHECK(compileCpuGraph(heavy.graph, calibrated, true).submissions().size() == 1);
    CHECK(compileCpuGraph(heavy.graph, {{1'000, 1'000, 100'000}}).submissions().size() == 1);

    const auto scheduled = compileCpuGraph(heavy.graph, calibrated);
    const auto batches = scheduled.submissions();
    CHECK(batches.size() == 3);
    CHECK(batches[0].queue == EQueueRole::GRAPHICS && batches[1].queue == EQueueRole::COMPUTE);
    CHECK(batches[2].queue == EQueueRole::GRAPHICS);
    CHECK(batches[1].dependencies.size() == 1 && batches[1].dependencies.front().producer == 0);
    CHECK(batches[2].dependencies.size() == 1 && batches[2].dependencies.front().producer == 0);
}

static void nativeQueueCandidates(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    options.graph_queue_costs = {1'000, 1'000, 1'000}; // Deterministic scheduling input for the fixture.
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    FrameHarness frames(*session, 1);
    const auto fixture = queueScheduleFixture(backend, 80'000, false);
    const auto analysis = take(analyzeRenderGraph(fixture.graph));
    const auto plan = compile(*session, fixture.graph, analysis, nullptr, ETransientAllocation::ALIAS_MEMORY, false);
    const auto bounds = plan.executable.submissions();
    const bool separate = &session->queue() != &session->queue(EQueueRole::COMPUTE);
    CHECK(bounds.size() == (separate ? 3 : 1));
    if (separate)
    {
        CHECK(bounds[1].queue == EQueueRole::COMPUTE);
    }
    RenderGraphInstance instance(*session, 1, plan.executable, plan.programs);
    CHECK(instance.initialize());
    const std::array instances{&instance};
    CHECK(take(runTask(frames.render(instances, {}))) > 0);
    finishSubmissions(*session, frames);
    CHECK(session->drain());
    const auto computed = readHistoryResult<4>(*session, *instance.nativeBuffer(fixture.compute_result.index, 0));
    const auto graphics = readHistoryResult<4>(*session, *instance.nativeBuffer(fixture.graphics_result.index, 0));
    CHECK(computed[0] == 4 && graphics[0] == 7);
    std::printf("Calibrated queue schedule: split=%u, both branches read back correctly\n", separate);
}

static void cpuRasterPackets()
{
    RenderGraphBuilder builder;
    const GraphImageDesc color_desc{64, 64, 1, 2, rdesc::ETextureFormat::RGBA8_UNORM, 4};
    const GraphImageDesc depth_desc{64, 64, 1, 2, rdesc::ETextureFormat::D32_SFLOAT, 4};
    const auto color = take(builder.declareResource({"color", color_desc}));
    const auto other = take(builder.declareResource({"other color", color_desc}));
    const auto depth = take(builder.declareResource({"depth", depth_desc}));
    const auto resolved = take(builder.declareResource({"resolved", GraphImageDesc{64, 64, 1, 2}}));
    const GraphImageRange color_range{1, 0, 1, 1, 1};
    const GraphImageRange depth_range{2, 0, 1, 1, 1};
    RasterWork work;
    work.fixed().interface.program = take(builder.addProgram(dualTargetRasterProgram()));
    work.fixed().graphics.depth_test = work.fixed().graphics.depth_write = true;
    work.colors = {
        {{{color, 0}, color_range}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE},
        {{{other, 0}, color_range}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE}
    };
    work.colors.front().resolve = ResourceAccess{{resolved, 0}, color_range};
    work.depth = RasterAttachment{{{depth, 0}, depth_range}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE};
    const auto drawn = take(builder.addRasterPass("MRT depth and resolve", std::move(work)));
    CHECK(builder.exportOutput({written(drawn, resolved), color_range}));
    const auto executable = compileCpuGraph(take(std::move(builder).finish()));
    CHECK(executable.passes().size() == 1);
    const auto& pass = executable.passes().front();
    const auto& recording = executable.recordingProgram();
    CHECK(pass.commands.kernel == ERecordKernel::RASTER && recording.raster.size() == 1);
    CHECK(recording.groups.size() == 1);
    const auto& scope = recording.groups.front();
    CHECK(scope.colors.size() == 2 && scope.depth);
    CHECK(std::get<GraphImageRange>(scope.depth->access.range) == depth_range);
    CHECK(scope.colors.front().resolve);
    CHECK(scope.colors.front().resolve->version.resource == resolved);
    CHECK(std::get<GraphImageRange>(scope.colors.front().resolve->range) == color_range);
    for (const auto& attachment : scope.colors)
    {
        CHECK(std::get<GraphImageRange>(attachment.access.range) == color_range);
    }
}

static void historySequence(
    Validation& validation, EGraphicsBackend backend, std::uint32_t fif, bool use_fallback, std::uint32_t distance = 1
)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto history = take(builder.declareHistory("samples", GraphBufferDesc{16}, {}, EGraphUse::STORAGE_READ,
                                                     distance));
    const auto enabled = take(builder.declareCondition("update"));
    const auto independent = take(builder.declareHistory("independent", GraphBufferDesc{16}));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{52}}));
    const auto valid = take(builder.declareResource({"valid", GraphBufferDesc{4}}));
    std::optional<ResourceVersion> shader_fallback, copy_fallback;
    if (use_fallback)
    {
        const auto shader_seed = take(builder.declareResource({"shader seed", GraphBufferDesc{16}}));
        const auto copy_seed = take(builder.declareResource({"copy seed", GraphBufferDesc{16}}));
        shader_fallback =
            take(builder.addTransferPass("initialize shader seed", TransferWork{{FillBuffer{{{shader_seed, 0}}, 17}}})
            ).written.front();
        copy_fallback =
            take(builder.addTransferPass("initialize copy seed", TransferWork{{FillBuffer{{{copy_seed, 0}}, 29}}})
            ).written.front();
    }
    const auto cleared =
        take(builder.addTransferPass("clear current", TransferWork{{FillBuffer{{{history.current, 0}}}}}));
    ComputeWork work;
    work.interface.program =
        take(builder.addProgram(program(kGraphHistory, backend)));
    work.interface.resources = {
        {"previous_sample", {{history.previous, 0}, {}, EGraphAccess::READ, 0, shader_fallback}},
        {"current_sample", {cleared.written.front(), {}, EGraphAccess::WRITE, 0, cleared.written.front()}}
    };
    const auto incremented = take(
        builder.addComputePass("increment", std::move(work), {false, PassCondition{EConditionSource::INPUT, enabled}})
    );
    CHECK(builder.publishHistory(incremented.written.front()));
    ComputeWork advancing;
    advancing.interface.program =
        take(builder.addProgram(program(kGraphHistory, backend)));
    advancing.interface.resources = {
        {"previous_sample", {{independent.previous, 0}}},
        {"current_sample", {{independent.current, 0}, {}, EGraphAccess::WRITE}}
    };
    const auto advanced = take(builder.addComputePass("advance independent", std::move(advancing)));
    CHECK(builder.publishHistory(advanced.written.front()));
    const auto marker = take(builder.addTransferPass("invalid marker", TransferWork{{FillBuffer{{{valid, 0}}}}}));
    const auto valid_marker = take(builder.addTransferPass(
        "valid marker",
        TransferWork{{FillBuffer{{marker.written.front(), {}, EGraphAccess::WRITE, 0, marker.written.front()}, 1}}},
        {false, PassCondition{EConditionSource::HISTORY_VALID, history.previous.index}}
    ));
    const auto observed = take(builder.addTransferPass(
        "observe",
        TransferWork{
            {CopyBuffer{{incremented.written.front()}, {{output, 0}, GraphBufferRange{0, 16}}},
             CopyBuffer{
                 {{history.previous, 0}, {}, EGraphAccess::READ, 0, copy_fallback},
                 {{output, 0}, GraphBufferRange{16, 16}}
             },
             CopyBuffer{{valid_marker.written.front()}, {{output, 0}, GraphBufferRange{32, 4}}},
             CopyBuffer{{advanced.written.front()}, {{output, 0}, GraphBufferRange{36, 16}}}}
        }
    ));
    CHECK(builder.exportOutput({observed.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    std::vector<std::uint32_t> published;
    const auto newestInput = [&]()
    {
        return published.size() >= distance ? published[published.size() - distance] : 0u;
    };
    const auto evaluate = [&](std::uint32_t input)
    {
        return (input == 0 && use_fallback ? 17 : input) + 1;
    };
    CHECK(plan.diagnostics.resource_reuse[history.previous.index] == EResourceReuse::HISTORY);
    CHECK(plan.executable.physicalResources().resource_slots[history.current.index] == kGraphInvalidIndex);
    const std::uint8_t enabled_initial = 1;
    CHECK(instance->setConditions({&enabled_initial, 1}));
    CHECK(take(instance->prepareFrame(0)));
    const auto rejected = submitFixtureCommands(*session, session->queue(),
        [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void>
        { return lux::cxx::unexpected(RenderError{kDeviceArgument}); }
    );
    CHECK(!rejected && instance->progress().count(EExecutionOutcome::EXECUTED) == 0);
    const GraphResourceId invalid_id{UINT32_MAX};
    CHECK(!instance->invalidateHistory({&invalid_id, 1}));
    for (std::uint32_t execution = 0; execution < 13; ++execution)
    {
        const bool invalidate = execution == 8;
        if (invalidate)
        {
            CHECK(instance->invalidateHistory());
            published.clear();
        }
        const std::uint8_t enabled_value = execution != 3 && execution != 4 && execution != 10;
        CHECK(instance->setConditions({&enabled_value, 1}));
        const auto previous = newestInput();
        if (enabled_value)
        {
            published.push_back(evaluate(previous));
        }
        const auto submitted = take(runTask(frames->render(instances, {})));
        CHECK(submitted == 1);
        const auto slot = instance->progress().last_submitted_slot;
        CHECK(session->drain());
        const auto values = readHistoryResult<13>(*session, *instance->nativeBuffer(output.index, slot));
        for (unsigned i = 0; i < 4; ++i)
        {
            CHECK(values[i] == (enabled_value ? published.back() : 0));
            CHECK(values[i + 4] == (!previous && use_fallback ? 29 : previous));
            // A paused writer in the first history must not suppress this history's publication.
            CHECK(values[i + 9] == (execution < 8 ? execution + 1 : execution - 7));
        }
        CHECK(values[8] == unsigned(previous != 0));
    }
    // Several accepted frames coexist; history selection follows accepted samples, not slot parity.
    const std::uint8_t enabled_value = 1;
    CHECK(instance->setConditions({&enabled_value, 1}));
    for (std::uint32_t i = 0; i < fif; ++i)
    {
        if (!take(runTask(frames->render(instances, {}))))
        {
            CHECK(session->drain());
            CHECK(take(runTask(frames->render(instances, {}))) == 1);
        }
        published.push_back(evaluate(newestInput()));
    }
    CHECK(session->drain());
    const auto values =
        readHistoryResult(*session, *instance->nativeBuffer(output.index, instance->progress().last_submitted_slot));
    CHECK(values[0] == published.back());
    instance.reset();
    CHECK(session->drain());
}

static void hzbHistory(Validation& validation, EGraphicsBackend backend, std::uint32_t fif)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    GraphImageDesc image{9, 7, 1, 1, rdesc::ETextureFormat::R32_SFLOAT};
    image.extent.source = EImageExtentSource::TARGET;
    const auto source = take(builder.declareResource({"depth field", image}));
    image.mips = 4;
    const auto history = take(builder.declareHistory("hzb", image, {{1, 1, 1, 1}}, EGraphUse::SAMPLED));
    const auto seed_image = take(builder.declareResource({"initial pyramid", image}));
    const auto seeded = take(builder.addTransferPass(
        "initialize pyramid",
        TransferWork{{ClearImage{{{seed_image, 0}, GraphImageRange{1, 3, 1}}, {0.35f, 0.35f, 0.35f, 0.35f}}}}
    ));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{512}}));
    auto color_description = image;
    color_description.mips = 1;
    color_description.format = rdesc::ETextureFormat::RGBA8_UNORM;
    const auto color = take(builder.declareResource({"target", color_description}));
    ComputeWork seed;
    seed.interface.program =
        take(builder.addProgram(program(kGraphDepthField, backend)));
    seed.interface.resources = {{"generated", {{source, 0}, {}, EGraphAccess::WRITE}}};
    seed.dispatch = ImageDispatchArguments{source};
    const auto generated = take(builder.addComputePass("generate depth field", std::move(seed)));
    const auto reduce =
        take(builder.addProgram(program(kHzbDownsample, backend)));
    ResourceVersion current{history.current, 0};
    for (std::uint32_t mip = 0; mip < 4; ++mip)
    {
        ComputeWork work;
        work.interface.program = reduce;
        work.allow_async_compute = true;
        work.interface.resources = {
            {"uSrc", {mip ? current : generated.written.front(), GraphImageRange{1, mip ? mip - 1 : 0, 1}}},
            {"uDst", {current, GraphImageRange{1, mip, 1}, EGraphAccess::WRITE}}
        };
        work.dispatch = ImageDispatchArguments{history.current, mip};
        current = take(builder.addComputePass("reduce " + std::to_string(mip), std::move(work))).written.front();
    }
    CHECK(builder.publishHistory(current));
    const auto initialized =
        take(builder.addTransferPass("initialize copy rows", TransferWork{{FillBuffer{{{output, 0}}}}}));
    const auto copied = take(builder.addTransferPass(
        "observe pyramid",
        TransferWork{
            {CopyImageBuffer{
                 {current, GraphImageRange{1, 3, 1}},
                 {initialized.written.front(), GraphBufferRange{0, 256}, EGraphAccess::READ_WRITE},
                 256,
                 256
             },
             CopyImageBuffer{
                 {{history.previous, 0}, GraphImageRange{1, 3, 1}, EGraphAccess::READ, 0, seeded.written.front()},
                 {initialized.written.front(), GraphBufferRange{256, 256}, EGraphAccess::READ_WRITE},
                 256,
                 256
             }}
        }
    ));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto cleared = take(builder.addTransferPass("clear target", TransferWork{{ClearImage{{{color, 0}}}}}));
    CHECK(builder.exportOutput({cleared.written.front(), {}, GraphOutputRequirements{color_description.format, 1}}));
    const auto graph = take(std::move(builder).finish());
    const auto analysis = take(analyzeRenderGraph(graph));
    TargetDesc target{9, 7, color_description.format, 1, ETargetKind::OFFSCREEN};
    auto plan = compile(*session, graph, analysis, &target);
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, fif))};
    auto instance = std::make_unique<RenderGraphInstance>(
        *session, fif, plan.executable, plan.programs, physical->output()
    );
    CHECK(instance->initialize());
    for (std::uint32_t execution = 0; execution < 4; ++execution)
    {
        if (execution == 2)
        {
            // Retire an in-flight old instance while preparing a larger candidate from the same immutable plan.
            const std::array old{instance.get()};
            CHECK(take(runTask(frames->render(old, {}))) > 0 && !frames->submitting());
            target.width = 17;
            target.height = 11;
            auto replacement = std::optional{take(OffscreenTarget::create(*session, target, fif))};
            auto candidate = std::make_unique<RenderGraphInstance>(
                *session, fif, plan.executable, plan.programs, replacement->output()
            );
            CHECK(candidate->initialize());
            instance = std::move(candidate);
            physical.emplace(std::move(*replacement));
            CHECK(session->drain());
        }
        const std::array instances{instance.get()};
        CHECK(take(runTask(frames->render(instances, {}))) > 0 && !frames->submitting());
        CHECK(session->drain());
        const auto values = readHistoryResult<128>(
            *session,
            *instance->nativeBuffer(output.index, instance->progress().last_submitted_slot)
        );
        const auto width = std::max(1u, target.width >> 3);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const auto expected = x + 1 == width ? 0.9f : 0.2f;
            CHECK(std::abs(std::bit_cast<float>(values[x]) - expected) < 0.00001f);
            const auto previous = execution % 2 == 0 ? 0.35f : expected;
            CHECK(std::abs(std::bit_cast<float>(values[64 + x]) - previous) < 0.00001f);
        }
    }
    instance.reset();
    physical.reset();
    CHECK(session->drain());
}

static void conditionalRegion(Validation& validation, EGraphicsBackend backend, std::uint32_t fif)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto input = take(builder.declareCondition("choose update"));
    const auto data = take(builder.declareResource({"data", GraphBufferDesc{16}}));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{16}}));
    auto current = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{data, 0}}, 3}}})).written.front();
    for (auto value : {6u, 9u})
    {
        current = take(builder.addTransferPass(
                           "conditional " + std::to_string(value),
                           TransferWork{{FillBuffer{{current, {}, EGraphAccess::WRITE, 0, current}, value}}},
                           {false, PassCondition{EConditionSource::INPUT, input}}
                       )
        ).written.front();
    }
    const auto copied = take(builder.addTransferPass("observe", TransferWork{{CopyBuffer{{current}, {{output, 0}}}}}));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
    std::string dump;
    CHECK(plan.diagnostics.resource_reuse[data.index] == EResourceReuse::CONDITIONAL);
    CHECK(dumpPlan(
        plan,
        EGraphDumpFormat::JSON,
        {&dump,
         [](void* value, std::string_view text) noexcept
         {
             static_cast<std::string*>(value)->append(text);
             return true;
         }}
    ));
    CHECK(dump.find("\"conditional_regions\":[{\"first_pass\":1,\"pass_count\":2") != std::string::npos);
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    for (const std::uint8_t enabled : {0, 1, 0, 1})
    {
        CHECK(instance->setConditions({&enabled, 1}));
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto values = readHistoryResult<4>(
            *session,
            *instance->nativeBuffer(output.index, instance->progress().last_submitted_slot)
        );
        CHECK(std::ranges::all_of(values, [&](auto value) { return value == (enabled ? 9 : 3); }));
    }
    instance.reset();
    CHECK(session->drain());
}

static void sharedImportedBuffer(Validation& validation, EGraphicsBackend backend, bool write_back, bool separate_ranges = false)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, 2);
    const auto& core = session->nriCore();
    nri::Buffer* shared = nullptr;
    CHECK(
        core.CreatePlacedBuffer(
            session->device(),
            NriDeviceHeap,
            {32, 4, nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE},
            shared
        ) == nri::Result::SUCCESS
    );
    CHECK(submitFixtureCommands(*session, session->queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            core.CmdZeroBuffer(command, *shared, 0, 32);
            const nri::BufferBarrierDesc barrier{
                shared,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY},
                {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::ALL}
            };
            core.CmdBarrier(command, {nullptr, 0, &barrier, 1});
            return {};
        }
    ));
    CHECK(session->drain());
    RenderGraphBuilder builder;
    const auto input = take(builder.declareResource(
        {"shared input", GraphBufferDesc{16}, EGraphOrigin::IMPORTED, {{}}, EGraphUse::STORAGE_READ}
    ));
    const auto current = take(builder.declareResource({"increment", GraphBufferDesc{16}}));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{16}}));
    ComputeWork work;
    work.allow_async_compute = true;
    work.interface.program =
        take(builder.addProgram(program(kGraphHistory, backend)));
    work.interface.resources = {
        {"previous_sample", {{input, 0}}},
        {"current_sample", {{current, 0}, {}, EGraphAccess::WRITE}}
    };
    const auto incremented = take(builder.addComputePass("increment", std::move(work)));
    TransferWork copies{{CopyBuffer{{incremented.written.front()}, {{output, 0}}}}};
    if (write_back)
    {
        copies.operations.push_back(CopyBuffer{{incremented.written.front()}, {{input, 0}}});
    }
    const auto copied = take(builder.addTransferPass("copy result", std::move(copies)));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
    auto first = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
    auto second = std::make_unique<RenderGraphInstance>(*session, 2, plan.executable, plan.programs);
    for (auto* instance : {first.get(), second.get()})
    {
        const std::uint64_t offset = separate_ranges && instance == second.get() ? 16 : 0;
        instance->setResource(input.index, UINT32_MAX, {shared, offset, 16});
        CHECK(instance->initialize());
    }
    const std::array instances{first.get(), second.get()};
    const auto before = session->queue(EQueueRole::COMPUTE).submitted();
    CHECK(take(runTask(frames->render(instances, {}))) > 0);
    finishSubmissions(*session, *frames);
    if (&session->queue() != &session->queue(EQueueRole::COMPUTE))
    {
        const bool has_shared_writes = write_back && !separate_ranges;
        CHECK(session->queue(EQueueRole::COMPUTE).submitted() - before == (has_shared_writes ? 2 : 1));
    }
    CHECK(session->drain());
    const auto a =
        readHistoryResult<4>(*session, *first->nativeBuffer(output.index, first->progress().last_submitted_slot));
    const auto b =
        readHistoryResult<4>(*session, *second->nativeBuffer(output.index, second->progress().last_submitted_slot));
    const auto second_value = write_back && !separate_ranges ? 2u : 1u;
    CHECK(a[0] == 1 && b[0] == second_value);
    first.reset();
    second.reset();
    CHECK(session->drain());
    core.DestroyBuffer(shared);
}

static void multiQueueHistory(Validation& validation, DeviceOptions options, std::uint32_t fif)
{
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto history = take(builder.declareHistory("samples", GraphBufferDesc{16}));
    const auto temporary = take(builder.declareResource({"temporary", GraphBufferDesc{16}}));
    const auto initial = take(builder.declareResource({"initial", GraphBufferDesc{16}}));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{16}}));
    const auto seeded = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{initial, 0}}, 1}}}));
    const auto shader =
        take(builder.addProgram(program(kGraphHistory, options.backend)));
    ResourceVersion current{history.current, 0};
    ResourceVersion scratch{temporary, 0};
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = shader;
        work.interface.resources = {
            {"previous_sample",
             i == 0 ? ResourceAccess{{history.previous, 0}, {}, EGraphAccess::READ, 0, seeded.written.front()}
                    : ResourceAccess{current}},
            {"current_sample", {scratch, {}, EGraphAccess::WRITE}}
        };
        scratch = take(builder.addComputePass("increment " + std::to_string(i), std::move(work))).written.front();
        current =
            take(builder.addTransferPass("copy " + std::to_string(i), TransferWork{{CopyBuffer{{scratch}, {current}}}})
            ).written.front();
    }
    CHECK(builder.publishHistory(current));
    const auto observed =
        take(builder.addTransferPass("observe", TransferWork{{CopyBuffer{{current}, {{output, 0}}}}}));
    CHECK(builder.exportOutput({observed.written.front()}));
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
    const bool separate = &session->queue() != &session->queue(EQueueRole::COMPUTE);
    CHECK(plan.executable.submissions().size() == (separate ? 7 : 1));
    if (separate)
    {
        const auto& copy_dependencies = plan.executable.submissions()[2].dependencies;
        const auto copy = std::ranges::find(copy_dependencies, 1u, &SubmissionDependency::producer);
        CHECK(copy != copy_dependencies.end() && copy->stages == nri::StageBits::COPY);
        const auto& compute_dependencies = plan.executable.submissions()[3].dependencies;
        const auto compute = std::ranges::find(compute_dependencies, 2u, &SubmissionDependency::producer);
        // The latest Graphics producer also covers the shared preparation edge (ALL).
        CHECK(compute != compute_dependencies.end() && compute->stages == nri::StageBits::ALL);
        CHECK(
            std::ranges::find(compute_dependencies, 0u, &SubmissionDependency::producer) == compute_dependencies.end()
        );
    }

    auto first = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    auto second = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(first->initialize() && second->initialize());
    const std::array instances{first.get(), second.get()};
    for (unsigned frame = 0; frame < 2; ++frame)
    {
        const auto before_graphics = session->queue().submitted();
        const auto before_compute = session->queue(EQueueRole::COMPUTE).submitted();
        CHECK(take(runTask(frames->render(instances, {}))) != 0);
        if (separate)
        {
            // Every graph batch owns a FIF command buffer: seven batches must no longer
            // stop at the temporary pool's three slots.
            CHECK(!frames->submitting(*first) && !frames->submitting(*second));
            CHECK(first->progress().count(EExecutionOutcome::EXECUTED) == frame + 1);
            if (frame == 0)
            {
                CHECK(first->invalidateHistory());
            }
            finishSubmissions(*session, *frames);
            CHECK(session->queue().submitted() - before_graphics == 4);
            CHECK(session->queue(EQueueRole::COMPUTE).submitted() - before_compute == 3);
        }
        else
        {
            CHECK(session->queue().submitted() - before_graphics == 1);
        }
        CHECK(session->drain());
        const auto a =
            readHistoryResult<4>(*session, *first->nativeBuffer(output.index, first->progress().last_submitted_slot));
        const auto b =
            readHistoryResult<4>(*session, *second->nativeBuffer(output.index, second->progress().last_submitted_slot));
        CHECK(a[0] == (separate ? 4 : 4 + frame * 3));
        CHECK(b[0] == 4 + frame * 3);
        if (const auto* path = std::getenv("LUX_SYNC_PLAN_DUMP"))
        {
            std::ofstream output(path, std::ios::app);
            const DumpSink sink{&output, [](void* context, std::string_view text) noexcept
            {
                auto& stream = *static_cast<std::ofstream*>(context);
                stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                return stream.good();
            }};
            CHECK(dumpPlan(plan, EGraphDumpFormat::JSON, sink));
            CHECK(frames->dumpSubmissions(sink));
        }
    }
    if (separate)
    {
        // Reject Compute before native acceptance after Graphics has accepted the frame prefix.
        auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
        static decltype(core.QueueSubmit) original_submit;
        static nri::Queue* rejected_queue;
        original_submit = core.QueueSubmit;
        rejected_queue = &session->queue(EQueueRole::COMPUTE).native();
        core.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& input) noexcept -> nri::Result
        {
            return &queue == rejected_queue ? nri::Result::INVALID_ARGUMENT : original_submit(queue, input);
        };
        const auto before = session->queue().submitted();
        const auto rejected = runTask(frames->render(instances, {}));
        core.QueueSubmit = original_submit;
        CHECK(!rejected && rejected.error().type == kDeviceFailure);
        CHECK(rejected.error().args[0] == static_cast<std::uint64_t>(nri::Result::INVALID_ARGUMENT));
        CHECK(session->queue().submitted() > before);
        CHECK(first->progress().count(EExecutionOutcome::EXECUTED) == 2);
        CHECK(!frames->submitting() && !first->prepareFrame(0));
    }
    first.reset();
    second.reset();
    CHECK(session->drain());
    const auto old_serial = session->queue().submitted();
    CHECK(submitFixtureCommands(*session, session->queue(),
        [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void> { return {}; }
    ));
    const auto dependency = take(session->queue().dependency(old_serial, nri::StageBits::ALL));
    CHECK(dependency.value == old_serial && dependency.value != session->queue().submitted());
    CHECK(!session->queue().dependency(session->queue().submitted() + 1, nri::StageBits::ALL));
    CHECK(session->drain());
}

static void physicalImageReuse(Validation& validation, EGraphicsBackend backend, std::uint32_t fif, bool alias_memory = false)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto a = take(builder.declareResource({"red temporary", GraphImageDesc{16, 16}}));
    const auto extent = alias_memory ? 32u : 16u;
    const auto b = take(builder.declareResource({"green temporary", GraphImageDesc{extent, extent}}));
    const auto red = take(builder.declareResource({"red pixels", GraphBufferDesc{4096}}));
    const auto green = take(builder.declareResource({"green pixels", GraphBufferDesc{extent * 256}}));
    const auto first = take(builder.addTransferPass("red", TransferWork{{ClearImage{{{a, 0}}, {1, 0, 0, 1}}}}));
    const auto first_copy = take(builder.addTransferPass(
        "preserve red",
        TransferWork{{CopyImageBuffer{{first.written.front()}, {{red, 0}}, 256, 4096}}}
    ));
    const auto second = take(builder.addTransferPass("green", TransferWork{{ClearImage{{{b, 0}}, {0, 1, 0, 1}}}}));
    CHECK(builder.dependsOn(first_copy.pass, second.pass));
    const auto second_copy = take(builder.addTransferPass(
        "preserve green",
        TransferWork{{CopyImageBuffer{{second.written.front()}, {{green, 0}}, 256, extent * 256}}}
    ));
    exportRows(builder, first_copy.written.front(), 16, 16, 256);
    exportRows(builder, second_copy.written.front(), extent, extent, 256);
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(
        *session,
        graph,
        take(analyzeRenderGraph(graph)),
        nullptr,
        alias_memory ? ETransientAllocation::ALIAS_MEMORY : ETransientAllocation::REUSE_OBJECTS
    );
    CHECK(
        (plan.executable.physicalResources().resource_slots[a.index] == plan.executable.physicalResources().resource_slots[b.index]) ==
        !alias_memory
    );
    CHECK(plan.executable.physicalResources().slots.size() == (alias_memory ? 4 : 3));
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(instance->initialize());
    if (alias_memory)
    {
        std::string dump;
        CHECK(instance->dumpResources(
            {&dump,
             [](void* p, std::string_view text) noexcept
             {
                 static_cast<std::string*>(p)->append(text);
                 return true;
             }}
        ));
        CHECK(dump.find("\"explicit_memory_count\":3") != std::string::npos);
    }
    const std::array instances{instance.get()};
    for (std::uint32_t i = 0; i < fif + 1; ++i)
    {
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto slot = instance->progress().last_submitted_slot;
        checkPixels(*session, *instance->nativeBuffer(red.index, slot), 16, 0, {255, 0, 0, 255}, 256);
        checkPixels(*session, *instance->nativeBuffer(green.index, slot), extent, 0, {0, 255, 0, 255}, 256);
    }
    instance.reset();
    CHECK(session->drain());
}

static void
physicalReuse(Validation& validation, DeviceOptions options, std::uint32_t fif, bool ordered, bool alias_memory = false)
{
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto a = take(builder.declareResource({"first temporary", GraphBufferDesc{16}}));
    const auto b = take(builder.declareResource({"second temporary", GraphBufferDesc{alias_memory ? 32u : 16u}}));
    const auto output_a = take(builder.declareResource({"first result", GraphBufferDesc{16}}));
    const auto output_b = take(builder.declareResource({"second result", GraphBufferDesc{16}}));
    const auto shader =
        take(builder.addProgram(program(kGraphHistory, options.backend)));
    const auto seed_a = take(builder.addTransferPass("seed first", TransferWork{{FillBuffer{{{a, 0}}, 3}}}));
    const auto increment = [&](std::string name, ResourceVersion input, GraphResourceId output)
    {
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = shader;
        work.interface.resources = {
            {"previous_sample", {input}},
            {"current_sample", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        return take(builder.addComputePass(std::move(name), std::move(work)));
    };
    const auto first = increment("observe first", seed_a.written.front(), output_a);
    const auto seed_b = take(builder.addTransferPass("seed second", TransferWork{{FillBuffer{{{b, 0}}, 7}}}));
    if (ordered)
    {
        CHECK(builder.dependsOn(first.pass, seed_b.pass));
    }
    const auto second = increment("observe second", seed_b.written.front(), output_b);
    CHECK(builder.exportOutput({first.written.front(), {}, {}, EGraphUse::TRANSFER_DESTINATION}));
    CHECK(builder.exportOutput({second.written.front(), {}, {}, EGraphUse::TRANSFER_DESTINATION}));
    const auto graph = take(std::move(builder).finish());
    const auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(
        *session,
        graph,
        analysis,
        nullptr,
        alias_memory ? ETransientAllocation::ALIAS_MEMORY : ETransientAllocation::REUSE_OBJECTS
    );
    auto control = compile(*session, graph, analysis, nullptr, ETransientAllocation::INDEPENDENT);
    CHECK(plan.executable.submissions().size() == control.executable.submissions().size());
    for (std::size_t i = 0; i < plan.executable.submissions().size(); ++i)
    {
        const auto& compiled = plan.executable.submissions()[i];
        const auto& independent = control.executable.submissions()[i];
        CHECK(compiled.queue == independent.queue && compiled.dependencies.size() == independent.dependencies.size());
        for (std::size_t d = 0; d < compiled.dependencies.size(); ++d)
        {
            CHECK(compiled.dependencies[d].producer == independent.dependencies[d].producer);
        }
    }
    const bool reuse = ordered || &session->queue() == &session->queue(EQueueRole::COMPUTE);
    CHECK(
        (plan.executable.physicalResources().resource_slots[a.index] == plan.executable.physicalResources().resource_slots[b.index]) ==
        (reuse && !alias_memory)
    );
    CHECK(plan.diagnostics.resource_reuse[output_a.index] == EResourceReuse::EXPORTED);
    CHECK(control.executable.physicalResources().slots.size() == 4);
    CHECK(plan.executable.physicalResources().slots.size() == (reuse && !alias_memory ? 3 : 4));
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(instance->initialize());
    if (alias_memory)
    {
        std::string dump;
        CHECK(instance->dumpResources(
            {&dump,
             [](void* p, std::string_view text) noexcept
             {
                 static_cast<std::string*>(p)->append(text);
                 return true;
             }}
        ));
        CHECK(dump.find("\"explicit_memory_count\":" + std::to_string(reuse ? 3 : 4)) != std::string::npos);
        CHECK(dump.find(reuse ? "\"reason\":\"aliased\"" : "\"reason\":\"unordered\"") != std::string::npos);
    }
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        CHECK(
            (instance->nativeBuffer(a.index, slot) == instance->nativeBuffer(b.index, slot)) == (reuse && !alias_memory)
        );
        for (std::uint32_t previous = 0; previous < slot; ++previous)
        {
            CHECK(instance->nativeBuffer(a.index, slot) != instance->nativeBuffer(a.index, previous));
        }
    }
    const std::array instances{instance.get()};
    for (std::uint32_t frame = 0; frame < fif + 2; ++frame)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (instance->progress().count(EExecutionOutcome::EXECUTED) <= frame &&
               std::chrono::steady_clock::now() < deadline)
        {
            take(runTask(frames->render(instances, {})));
            CHECK(session->progressCompletions());
            if (instance->progress().count(EExecutionOutcome::EXECUTED) <= frame)
            {
                take(frames->waitForProgress(1'000'000));
            }
        }
        CHECK(instance->progress().count(EExecutionOutcome::EXECUTED) == frame + 1);
    }
    CHECK(session->drain());
    const auto slot = instance->progress().last_submitted_slot;
    const auto left = readHistoryResult<4>(*session, *instance->nativeBuffer(output_a.index, slot));
    const auto right = readHistoryResult<4>(*session, *instance->nativeBuffer(output_b.index, slot));
    CHECK(std::ranges::all_of(left, [](auto value) { return value == 4; }));
    CHECK(std::ranges::all_of(right, [](auto value) { return value == 8; }));
    take(runTask(frames->render(instances, {})));
    finishSubmissions(*session, *frames);
    instance.reset(); // Accepted GPU work retains the unique physical collection through cleanup_scope_.
    CHECK(session->drain());
}

static void coalescedImageRanges(Validation& validation, EGraphicsBackend backend, std::uint32_t fif)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    GraphImageDesc description{32, 32};
    description.mips = 4;
    description.layers = 3;
    const auto image = take(builder.declareResource({"array", description}));
    const auto output = take(builder.declareResource({"readback", GraphBufferDesc{8192}}));
    TransferWork clear;
    for (std::uint32_t mip = 0; mip < description.mips; ++mip)
    {
        clear.operations.push_back(ClearImage{
            {{image, 0}, GraphImageRange{1, mip, 1, 0, description.layers}}, {.25f, .5f, .75f, 1}
        });
    }
    const auto cleared = take(builder.addTransferPass("clear every mip and layer", std::move(clear)));
    const auto version = written(cleared, image);
    const auto copied = take(builder.addTransferPass(
        "read last layer", TransferWork{{CopyImageBuffer{
            {version, GraphImageRange{1, 0, 1, 2, 1}}, {{output, 0}}, 256, 8192
        }}}
    ));
    exportRows(builder, copied.written.front(), 32, 32, 256);
    CHECK(builder.exportOutput({version, GraphImageRange{1, 0, 4, 0, 3}, {}, EGraphUse::SAMPLED}));
    auto graph = take(std::move(builder).finish());
    auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    const auto& transitions = plan.executable.passes().front().before.images;
    CHECK(transitions.size() == 1);
    const auto& transition = transitions.front();
    CHECK(transition.resource == image.index && transition.previous_slot == kGraphInvalidIndex);
    CHECK(transition.native.mipOffset == 0 && transition.native.mipNum == 4);
    CHECK(transition.native.layerOffset == 0 && transition.native.layerNum == 3);
    CHECK(transition.native.before.layout == nri::Layout::UNDEFINED);
    CHECK(transition.native.after.layout == nri::Layout::COLOR_ATTACHMENT);
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan.executable, plan.programs);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    for (std::uint32_t frame = 0; frame < fif; ++frame)
    {
        CHECK(take(runTask(frames->render(instances, {}))) != 0);
        finishSubmissions(*session, *frames);
        CHECK(session->drain());
        checkPixels(*session, *instance->nativeBuffer(output.index, frame), 32, 0, {64, 128, 191, 255}, 256);
    }
    instance.reset();
    CHECK(session->drain());
    std::printf("COALESCED RANGE PASS cells=12 descriptors=1 FIF=%u last_layer_readback=correct\n", fif);
}

static void cpuGraphAnalysis()
{
    {
        RenderGraphBuilder builder;
        const auto buffer = take(builder.declareResource({"buffer", GraphBufferDesc{16}}));
        const auto written = take(builder.addTransferPass(
            "write first half", TransferWork{{FillBuffer{{{buffer, 0}, GraphBufferRange{0, 8}}, 0}}}
        ));
        CHECK(builder.exportOutput({written.written.front()}));
        const auto graph = take(std::move(builder).finish());
        const auto analysis = analyzeRenderGraph(graph);
        CHECK(!analysis && analysis.error().type == kGraphUndefinedRead);
    }
    {
        RenderGraphBuilder builder;
        const auto dead = take(builder.declareResource({"dead", GraphBufferDesc{16}}));
        const auto external = take(builder.declareResource({
            "external", GraphBufferDesc{16}, EGraphOrigin::IMPORTED
        }));
        CHECK(builder.addTransferPass("dead write", TransferWork{{FillBuffer{{{dead, 0}}, 0}}}));
        const auto live = take(builder.addTransferPass(
            "external write", TransferWork{{FillBuffer{{{external, 0}}, 0}}}
        ));
        const auto graph = take(std::move(builder).finish());
        const auto analysis = take(analyzeRenderGraph(graph));
        CHECK(analysis.execution_order == std::vector{live.pass});
    }
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "analysis")
    {
        cpuGraphAnalysis();
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "cpu-recording")
    {
        cpuRecordingTables();
        cpuConditionalRanges();
        cpuImportedBoundaries();
        cpuRasterPackets();
        cpuQueueBoundaries();
        cpuQueueCandidates();
        std::puts("CPU matrix: conditional range, zero-unit export, imported boundary, MRT/depth/resolve packets");
        return 0;
    }
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    Validation validation;
#ifdef LUX_RENDER_GRAPH_ALLOCATION_PROBE
    if (argc > 1 && std::string_view(argv[1]) == "measure")
    {
        DeviceOptions options;
        if (argc > 2 && std::string_view(argv[2]) == "d3d12") options.backend = EGraphicsBackend::D3D12;
        TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
        const auto fixture = computeSequenceGraph();
        const auto analysis = take(analyzeRenderGraph(fixture.graph));
        const auto candidate = compile(*session, fixture.graph, analysis, nullptr);
        const auto status = measureGraph(*session, candidate);
        CHECK(validation.errors == 0);
        return status;
    }
#endif
    if (argc > 1 && std::string_view(argv[1]) == "recording")
    {
        recordingOwnership(validation, argc > 2 && std::string_view(argv[2]) == "d3d12");
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "raster")
    {
        typedRasterScopes(validation, argc > 2 && std::string_view(argv[2]) == "d3d12");
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "encoding")
    {
        const bool d3d12 = argc > 2 && std::string_view(argv[2]) == "d3d12";
        typedRecording(validation, d3d12);
        multiCommandCompute(validation, d3d12);
        CHECK(validation.errors == 0);
        return 0;
    }
    const auto backend = argc > 2 && std::string_view(argv[2]) == "d3d12"
        ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    if (argc > 1 && std::string_view(argv[1]) == "copies")
    {
        resolveAndCopy(validation, backend);
        coalescedImageRanges(validation, backend, 2);
        CHECK(validation.errors == 0);
        std::puts("Graph copy focus: image/buffer/offset roundtrip and layered ranges passed");
        return 0;
    }
    for (std::uint32_t fif : {1u, 2u, 3u})
    {
        coalescedImageRanges(validation, backend, fif);
        imageChain(validation, backend, fif);
        imageChain(validation, backend, fif, true);
        historySequence(validation, backend, fif, false);
        historySequence(validation, backend, fif, true);
        historySequence(validation, backend, fif, true, 2);
        conditionalRegion(validation, backend, fif);
        hzbHistory(validation, backend, fif);
        physicalImageReuse(validation, backend, fif);
        physicalImageReuse(validation, backend, fif, true);
        for (unsigned topology = 0; topology < 3; ++topology)
        {
            DeviceOptions options;
            options.backend = backend;
            options.multiple_queues = topology != 0;
            options.prefer_dedicated_queues = topology == 2;
            multiQueueHistory(validation, options, fif);
            physicalReuse(validation, options, fif, true);
            physicalReuse(validation, options, fif, false);
            physicalReuse(validation, options, fif, true, true);
            physicalReuse(validation, options, fif, false, true);
        }
    }
    nativeQueueCandidates(validation, backend);
    indirectCounts(validation, backend);
    resolveAndCopy(validation, backend);
    sharedImportedBuffer(validation, backend, false);
    sharedImportedBuffer(validation, backend, true);
    sharedImportedBuffer(validation, backend, true, true);
    CHECK(validation.errors == 0);
    std::printf(
        "Graph %s: Compute -> mip -> MRT/Depth -> sampled postprocess; FIF=1/2/3; GPU IndirectCount=0/1/3; errors=%u "
        "warnings=%u\n",
        backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan",
        validation.errors.load(),
        validation.warnings.load()
    );
}
