#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/graph/execute/FrameRecordInput.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/device/Error.hpp>
#include "NativeDevice.hpp"
#include "BufferReadback.hpp"
#include "AliasHandoffProbe.hpp"
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#ifdef LUX_RENDER_GRAPH_ALLOCATION_PROBE
#include "GraphMeasurements.hpp"
#endif
#include <lux/engine/render/extensions/device/CopyEncoding.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <lux/engine/render/graph/execute/RasterDraws.hpp>
#include "FrameHarness.hpp"
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <string>
#include <thread>

#include "GraphConstantLayout.hpp"
#include "GraphCount.hpp"
#include "GraphDepthField.hpp"
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include "GraphFill.hpp"
#include "GraphHistory.hpp"
#include "GraphTableSize.hpp"
#include "GraphMrt.hpp"
#include "GraphPresent.hpp"
#include "GraphRed.hpp"
#include "GraphTriangle.hpp"
#include "HzbDownsample.hpp"

using namespace foundation_test;
using lux::render::runtime_detail::JobExecution;
namespace rdesc = lux::rdesc;

struct alignas(16) NestedUniformBlock final
{
    std::array<std::array<std::array<std::uint32_t, 4>, 2>, 2> entries;
};

template <> struct lux::render::ConstantBlockTraits<NestedUniformBlock> final
{
    static ConstantBlockLayout layout(rdesc::EShaderTarget) noexcept
    {
        static constexpr std::array<rdesc::ShaderArrayDimension, 2> dimensions{{{2, 32}, {2, 16}}};
        static constexpr std::array<ConstantMemberLayout, 4> members{{
            {"entries[0].groups[0]", 0, rdesc::EDataScalar::UINT, 4, 1, 0,
             rdesc::EShaderMatrixOrder::NOT_MATRIX, dimensions},
            {"entries[0].groups[1]", 16, rdesc::EDataScalar::UINT, 4, 1, 0,
             rdesc::EShaderMatrixOrder::NOT_MATRIX, dimensions},
            {"entries[1].groups[0]", 32, rdesc::EDataScalar::UINT, 4, 1, 0,
             rdesc::EShaderMatrixOrder::NOT_MATRIX, dimensions},
            {"entries[1].groups[1]", 48, rdesc::EDataScalar::UINT, 4, 1, 0,
             rdesc::EShaderMatrixOrder::NOT_MATRIX, dimensions}
        }};
        return {sizeof(NestedUniformBlock), alignof(NestedUniformBlock), members};
    }
};

struct IndirectCountConstants final
{
    std::uint32_t requested;
};

template <> struct lux::render::ConstantBlockTraits<IndirectCountConstants> final
{
    static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
    {
        static constexpr std::array members{
            ConstantMemberLayout{"requested", offsetof(IndirectCountConstants, requested), rdesc::EDataScalar::UINT}
        };
        const unsigned alignment = target == rdesc::EShaderTarget::DXIL ? 16u : 4u;
        return {sizeof(IndirectCountConstants), alignment, members};
    }
};

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
        session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), {graph, analysis},
        {output, allocation, exercise_async}
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
    const auto nearest = builder.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_EDGE});
    fixed_mrt.interface.samplers = {{"source_sampler", {nearest}}};
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
    fixed_present.interface.samplers = {{"source_sampler", {nearest}}};
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
    auto physical = std::optional{take(OffscreenTarget::create(*session, target, fif))};
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), physical->output()
    );
    CHECK(instance_job->initialize());
    CHECK(instance_job->dumpResources(sink));
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
    CHECK(plan.graph.compatible(larger_target));
    auto larger = std::optional{take(OffscreenTarget::create(*session, larger_target, fif))};
    auto second_instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto second_instance_job = std::make_unique<JobExecution>(
        *second_instance_gpu, plan.graph, testInstallation(), larger->output()
    );
    CHECK(second_instance_job->initialize());
    CHECK(&instance_job->graph() == &plan.graph && &second_instance_job->graph() == &plan.graph);
    {
        auto candidate = take(ResizeResources::prepare(*instance_job, larger->output()));
        const auto foreign = candidate.prepareAdoption(*second_instance_job);
        CHECK(!foreign && foreign.error().type == kFrameContract);
    }
    const std::array instances{instance_job.get(), second_instance_job.get()};
    const auto submit = [&](std::span<JobExecution* const> selected)
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
        checkPixels(*session, *instance_job->resourceInput(pixels, slot).buffer, 64, 256, {64, 64, 64, 255});
        checkPixels(*session, *second_instance_job->resourceInput(pixels, slot).buffer, 128, 256, {64, 64, 64, 255});
    }
    CHECK(submit(instances) > 0);
    instance_job.reset();
    instance_gpu.reset();
    physical.reset();
    CHECK(session->drain());
    const std::array remaining{second_instance_job.get()};
    CHECK(submit(remaining) > 0);
    second_instance_job.reset();
    second_instance_gpu.reset();
    larger.reset();
    CHECK(session->drain());
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
    compute.interface.parameters.setInlineConstant(IndirectCountConstants{});
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
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, 3);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    const auto count_pass = plan.diagnostics.passSlot(generated.pass);
    CHECK(count_pass != kGraphInvalidIndex);
    CHECK(instance_job->initialize());
    plan.diagnostics = {};
    IndirectCountConstants count_input{1};
    const auto bytes = std::as_bytes(std::span{&count_input, 1});
    const std::array instances{instance_job.get()};
    CHECK(!recordFrame(*instance_job, *session, 0, 0)); // An initialized owner is not a prepared recording input.
    const auto pixel_binding = instance_job->resourceInput(pixels, 0);
    auto truncated_pixels = pixel_binding;
    CHECK(truncated_pixels.size == 8192);
    --truncated_pixels.size;
    instance_job->bindResource(pixels, 0, truncated_pixels);
    const auto rejected_extent = instance_job->freezeFrame(0);
    CHECK(!rejected_extent && rejected_extent.error().type == kDeviceArgument);
    CHECK(!recordFrame(*instance_job, *session, 0, 0) && session->queue().submitted() == 0);
    instance_job->bindResource(pixels, 0, pixel_binding);
    CHECK(instance_job->frame(0).writeInlineConstant(instance_job->graph(), count_pass, bytes, 1));
    CHECK(take(instance_job->freezeFrame(0)));
    CHECK(instance_job->frame(0).last_operands.compute == 1);
    count_input.requested = 0;
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    RecordTextureQueries::original = core.GetTextureDesc;
    RecordTextureQueries::count = 0;
    core.GetTextureDesc = &RecordTextureQueries::get;
    for (std::uint32_t batch = 0; batch < plan.graph.submissions().size(); ++batch)
    {
        const auto commands = take(recordFrame(*instance_job, *session, 0, batch));
        CHECK(runTask(session->queue(plan.graph.submissions()[batch].queue).submit(commands)));
        CHECK(session->drain()); // This fixture serializes batches; normal queue dependencies are tested below.
    }
    CHECK(RecordTextureQueries::count == 0);
    count_input.requested = 1;
    CHECK(instance_job->frame(0).writeInlineConstant(instance_job->graph(), count_pass, bytes, 2));
    CHECK(instance_job->frame(0).writeInlineConstant(instance_job->graph(), count_pass, bytes, 2));
    CHECK(take(instance_job->freezeFrame(0)));
    CHECK(instance_job->frame(0).last_operands.compute == 0);
    std::puts("DISPATCH FIELDS PASS: first freeze patched one indirect dispatch; stable freeze patched none");
    CHECK(RecordTextureQueries::count == 0); // An unchanged fixed slice reuses its frozen native bindings.
    core.GetTextureDesc = RecordTextureQueries::original;
    checkPixels(*session, *instance_job->resourceInput(pixels, 0).buffer, 32, 0, {255, 0, 0, 255}, 256);
    count_input.requested = 2;
    CHECK(instance_job->frame(0).writeInlineConstant(instance_job->graph(), count_pass, bytes, 3));
    CHECK(take(instance_job->freezeFrame(0)));
    CHECK(instance_job->frame(0).last_patches.passes == 1 && instance_job->frame(0).last_operands.compute == 0);
    std::puts("INLINE UPDATE PASS: one dirty Compute pass, zero indirect dispatch fields rewritten");
    CHECK(!instance_job->freezeFrame(UINT32_MAX));
    std::puts("FROZEN INPUT PASS author changed after Prepare, GPU uses prepared bytes, Record texture queries=0");
    for (std::uint32_t requested : {0u, 1u, 3u, 0u, 1u, 3u, 0u, 1u, 3u})
    {
        count_input.requested = requested;
        for (std::uint32_t slot = 0; slot < instance_job->frameCount(); ++slot)
        {
            CHECK(instance_job->frame(slot).writeInlineConstant(instance_job->graph(), count_pass, bytes, 0));
        }
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        checkPixels(
            *session,
            *instance_job->resourceInput(pixels, instance_job->progress.last_submitted_slot).buffer,
            32,
            0,
            {requested ? 255u : 0u, 0, 0, 255},
            256
        );
    }
    instance_job.reset();
    instance_gpu.reset();
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
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    const std::array instances{instance_job.get()};
    CHECK(take(runTask(frames->render(instances, {}))) == 1);
    CHECK(session->drain());
    checkPixels(*session, *instance_job->resourceInput(pixels, 0).buffer, 16, 256, {255, 0, 0, 255}, 256);
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
}

static void compressedCopies(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    FrameHarness frames(*session, 1);
    constexpr std::uint32_t pattern = 0x12345678;
    for (bool aligned : {false, true})
    {
        RenderGraphBuilder builder;
        const auto packed = take(builder.declareResource({"packed BC1 blocks", GraphBufferDesc{512}}));
        const auto pixels = take(builder.declareResource({"BC1 readback", GraphBufferDesc{512}}));
        const auto image = take(builder.declareResource({
            "compressed", GraphImageDesc{8, 8, 1, 1, rdesc::ETextureFormat::BC1_RGBA_SRGB}
        }));
        const auto init = take(builder.addTransferPass("seed rows", TransferWork{{FillBuffer{{{packed, 0}}, 0}}}));
        const auto seed = take(builder.addTransferPass("last BC1 block", TransferWork{{
            FillBuffer{{init.written.front(), GraphBufferRange{264, 8}}, pattern}
        }}));
        const auto upload = take(builder.addTransferPass("upload BC1", TransferWork{{
            CopyImageBuffer{{seed.written.front()}, {{image, 0}}, 256, 512}
        }}));
        const ImageCopyRegion rect{aligned ? 4u : 2u, 4, 4, 4};
        const auto copy = take(builder.addTransferPass("read one BC1 block", TransferWork{{
            CopyImageBuffer{{upload.written.front()}, {{pixels, 0}}, 0, 0, rect}
        }}));
        CHECK(builder.exportOutput({copy.written.front(), GraphBufferRange{0, 8}}));
        const auto graph = take(std::move(builder).finish());
        auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
        JobGpuResources instance_gpu(*session, 1);
        JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
        auto initialized = instance_job.initialize();
        if (!aligned)
        {
            CHECK(!initialized && initialized.error().type == kDeviceArgument);
            continue;
        }
        CHECK(initialized);
        const std::array instances{&instance_job};
        CHECK(take(runTask(frames.render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto words = readBufferWords<2>(*session, *instance_job.resourceInput(pixels, 0).buffer);
        CHECK((words == std::array{pattern, pattern}));
    }
    std::puts("BC1 COPY PASS: misaligned block rejected; automatic pitch selected the last block of the second row");
}

// This pass has three copies. Changing one source view must patch only that source address.
static void transferFieldPatches(DeviceSession& device, nri::Buffer& input)
{
    RenderGraphBuilder builder;
    const auto source = take(builder.declareResource({
        "changing source", GraphBufferDesc{4}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, 4}}, EGraphUse::TRANSFER_SOURCE
    }));
    const auto fixed = take(builder.declareResource({"fixed source", GraphBufferDesc{4}}));
    const auto seed = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{fixed, 0}}, 7}}}));
    std::array<GraphResourceId, 3> outputs;
    TransferWork work;
    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        outputs[i] = take(builder.declareResource({"output " + std::to_string(i), GraphBufferDesc{4}}));
        const auto from = i == 1 ? ResourceVersion{source, 0} : seed.written.front();
        work.operations.emplace_back(CopyBuffer{{from}, {{outputs[i], 0}}});
    }
    const auto copied = take(builder.addTransferPass("three copies", std::move(work)));
    for (auto version : copied.written) CHECK(builder.exportOutput({version}));
    const auto graph = take(std::move(builder).finish());
    auto compiled = compile(device, graph, take(analyzeRenderGraph(graph)), nullptr);
    JobGpuResources gpu(device, 1);
    JobExecution instance_job{gpu, compiled.graph, testInstallation(), TargetOutput{}};
    instance_job.bindResource(source, 0, {&input, 0, 4});
    CHECK(instance_job.initialize());
    FrameHarness frames(device, 1);
    const std::array jobs{&instance_job};
    const auto submit = [&]
    {
        CHECK(take(runTask(frames.render(jobs, {}))) > 0);
        finishSubmissions(device, frames);
        CHECK(device.drain());
    };
    CHECK(take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_operands.transfer == 7);
    submit();
    CHECK(instance_job.frame(0).last_operands.transfer == 0);
    CHECK(readBufferWords<1>(device, *instance_job.resourceInput(outputs[1], 0).buffer)[0] == 11);
    instance_job.bindResource(source, 0, {&input, 4, 2});
    CHECK(!instance_job.freezeFrame(0) && !recordFrame(instance_job, device, 0, 0));
    instance_job.bindResource(source, 0, {&input, 4, 4});
    CHECK(take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_operands.transfer == 1);
    submit();
    CHECK(instance_job.frame(0).last_operands.transfer == 0);
    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        const auto words = readBufferWords<1>(device, *instance_job.resourceInput(outputs[i], 0).buffer);
        CHECK(words[0] == (i == 1 ? 12u : 7u));
    }
    std::puts("TRANSFER FIELDS PASS: initial 7 fields, stable 0, short source rejected, "
              "rebind patched 1 source, stable 0; readback 11 -> 12, neighbors 7/7");
}

static void frameUploadSnapshot(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto uploads = take(ResourceUploader::create(
        *session, TransferOptions{4096, 2}, lux::execution::ExecutionRuntime::instance().cpuScheduler()
    ));
    const auto before = session->queue().submitted();
    CHECK(!take(runTask(uploads->submitPreparationCopies())));
    CHECK(session->queue().submitted() == before);
    std::array<std::uint32_t, 4> source{11, 12, 13, 14};
    const auto bytes = std::as_bytes(std::span(source));
    nri::Buffer* target = nullptr;
    const auto& core = session->nriCore();
    const nri::BufferDesc description{bytes.size(), 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE};
    CHECK(core.CreatePlacedBuffer(session->device(), NriDeviceHeap, description, target) == nri::Result::SUCCESS);
    CHECK(take(uploads->stageBuffer({target, 0, bytes})) == bytes.size());
    source.fill(99);
    CHECK(uploads->flushFrameCopies());
    const auto receipt = take(runTask(uploads->submitPreparationCopies()));
    CHECK(receipt && receipt->ranges == 1 && receipt->point.serial == session->queue().submitted());
    uploads->acceptFrameCopies(*receipt);
    CHECK(session->drain());
    const auto result = readBufferWords<4>(
        *session, *target, {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::ALL}
    );
    const std::array<std::uint32_t, 4> expected{11, 12, 13, 14};
    CHECK(result == expected);
    transferFieldPatches(*session, *target);
    core.DestroyBuffer(target);
    std::puts("FRAME UPLOAD SNAPSHOT PASS: caller mutation after staging leaves submitted bytes unchanged");

}

static void detachedDirtyRanges()
{
    std::array<DirtyRanges, 3> replicas;
    std::vector<DirtyWordMask> selected, pending;
    for (auto& replica : replicas)
    {
        replica.grow(130);
        replica.markRange(63, 3);
    }
    auto& first = replicas[0];
    first.detachRange(selected, 63, 3);
    CHECK(first.empty() && selected.size() == 2);
    for (auto& replica : replicas)
    {
        replica.mark(64);
        replica.mark(129);
    }
    first.finishSelection(selected, true);
    CHECK(selected.empty() && first.recordCount() == 2);
    first.select(3, pending);
    CHECK(pending.size() == 2 && pending[0].word_index == 1 && pending[0].bits == 1);
    CHECK(pending[1].word_index == 2 && pending[1].bits == 2);
    CHECK(replicas[1].recordCount() == 4 && replicas[2].recordCount() == 4);

    first.mark(63);
    first.detachRange(selected, 63, 2);
    first.mark(64);
    first.finishSelection(selected, false);
    CHECK(selected.empty() && first.recordCount() == 3);
    first.select(3, pending);
    CHECK(pending.size() == 3 && pending[0].bits == (std::uint64_t{1} << 63));
    CHECK(pending[1].bits == 1 && pending[2].bits == 2);
}

static void constantLayoutEvidence(Validation& validation, EGraphicsBackend backend)
{
    auto shader = program(kGraphConstantLayout, backend);
    CHECK(shader.interface.push_constants.size() == 1 && shader.interface.sets.size() == 1);
    const auto& push = shader.interface.push_constants.front().block;
    const auto& bindings = shader.interface.sets.front().bindings;
    const auto uniforms = std::ranges::find(bindings, "uniforms", &rdesc::EDescriptorBindingInfo::name);
    CHECK(uniforms != bindings.end());
    const auto& uniform = uniforms->block;
    CHECK(push.hasConstantLayoutEvidence() && uniform.hasConstantLayoutEvidence());
    const auto find_field = [](const rdesc::ShaderBlock& block, std::string_view path) -> const rdesc::ShaderMember&
    {
        const auto member = std::ranges::find(block.fields, path, &rdesc::ShaderMember::path);
        CHECK(member != block.fields.end());
        return *member;
    };
    const auto& basis = find_field(push, "basis");
    CHECK(basis.rows == 4 && basis.columns == 2 && basis.matrix_stride == 16);
    CHECK(basis.matrix_order == rdesc::EShaderMatrixOrder::COLUMN_MAJOR && basis.array_dimensions.empty());
    const auto& ids = find_field(push, "ids[1]");
    CHECK(ids.array_dimensions == (std::vector<rdesc::ShaderArrayDimension>{{2, 16}}));
    const auto& group = find_field(uniform, "entries[1].groups[1]");
    CHECK(group.array_dimensions == (std::vector<rdesc::ShaderArrayDimension>{{2, 32}, {2, 16}}));

    rdesc::ShaderInfo restored;
    CHECK(rdesc::ShaderInfo::deserialize(rdesc::ShaderInfo::serialize(shader.interface), restored));
    CHECK(restored == shader.interface);
    auto legacy = push;
    for (auto& member : legacy.fields)
    {
        member.matrix_order = rdesc::EShaderMatrixOrder::UNSPECIFIED;
        member.array_dimensions.clear();
    }
    rdesc::ShaderBlock decoded;
    CHECK(rdesc::ShaderBlock::deserialize(rdesc::ShaderBlock::serialize(legacy), decoded));
    CHECK(decoded == legacy && decoded.hasStructuredLayout() && !decoded.hasConstantLayoutEvidence());
    auto malformed = rdesc::ShaderBlock::serialize(push);
    malformed.pop_back();
    CHECK(!rdesc::ShaderBlock::deserialize(malformed, decoded) && decoded == legacy);

    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    const auto offset_alignment = session->description().memoryAlignment.constantBufferOffset;
    CHECK(offset_alignment >= 4);
    const auto block_offset = std::uint64_t{offset_alignment};
    const auto matrix_value = std::bit_cast<std::uint32_t>(2.5f);
    const std::uint32_t array_value = 19;
    const auto build = [&](std::uint64_t offset, ConstantBlockContract contract)
    {
        RenderGraphBuilder builder;
        const auto uniform_bytes = take(builder.declareResource({
            "Uniform", GraphBufferDesc{block_offset + sizeof(NestedUniformBlock)}
        }));
        const auto output = take(builder.declareResource({"Result", GraphBufferDesc{16}}));
        const auto readback = take(builder.declareResource({"Readback", GraphBufferDesc{16}}));
        const auto initialized = take(builder.addTransferPass(
            "UBO initialize", TransferWork{{FillBuffer{{{uniform_bytes, 0}}, 0}}}
        ));
        const auto selected = take(builder.addTransferPass(
            "UBO nested field",
            TransferWork{{FillBuffer{{
                written(initialized, uniform_bytes), GraphBufferRange{block_offset + group.offset + 4, 4}
            }, 27}}}
        ));
        ComputeWork work;
        work.interface.parameters.values.resize(push.size);
        auto* parameters = work.interface.parameters.values.data();
        std::memcpy(parameters + basis.offset + basis.matrix_stride + 8, &matrix_value, sizeof(matrix_value));
        std::memcpy(parameters + ids.offset + 8, &array_value, sizeof(array_value));
        work.interface.program = take(builder.addProgram(GraphProgram{shader}));
        work.interface.resources = {
            {"uniforms", {written(selected, uniform_bytes)}},
            {"output_values", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        auto& binding = work.interface.resources.front();
        binding.setUniformConstant<NestedUniformBlock>(offset);
        binding.uniform_constant = contract;
        const auto computed = take(builder.addComputePass("typed ABI readback", std::move(work)));
        const auto copied = take(builder.addTransferPass(
            "readback", TransferWork{{CopyBuffer{{written(computed, output)}, {{readback, 0}}}}}
        ));
        CHECK(builder.exportOutput({written(copied, readback), {}, {}, EGraphUse::TRANSFER_DESTINATION}));
        return std::pair{take(std::move(builder).finish()), readback};
    };
    const auto reject = [&](std::uint64_t offset, ConstantBlockContract contract, auto error)
    {
        const auto candidate = build(offset, contract).first;
        const auto analysis = take(analyzeRenderGraph(candidate));
        auto rejected = runTask(GraphCompiler::compile(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(),
            {candidate, analysis}, {.exercise_async = true}
        ));
        CHECK(!rejected && rejected.error().type == error);
    };
    auto mismatched = kConstantBlockContract<NestedUniformBlock>;
    mismatched.layout = [](rdesc::EShaderTarget target) noexcept
    {
        auto layout = ConstantBlockTraits<NestedUniformBlock>::layout(target);
        // The whole record still has the right size. Changing array coverage must be rejected.
        layout.members = layout.members.first(3);
        return layout;
    };
    reject(block_offset, mismatched, kGraphInvalidDescriptor);
    reject(block_offset - 3, kConstantBlockContract<NestedUniformBlock>, kGraphInvalidRange);

    auto [graph, readback] = build(block_offset, kConstantBlockContract<NestedUniformBlock>);
    const auto analysis = take(analyzeRenderGraph(graph));
    if (backend == EGraphicsBackend::D3D12)
    {
        // The block fits on its own. The CBV and UAV occupy separate NRI descriptor tables.
        static nri::DeviceDesc limited;
        limited = session->description();
        limited.pipelineLayout.rootConstantMaxSize = 64;
        auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
        const auto get_device_desc = core.GetDeviceDesc;
        core.GetDeviceDesc = [](const nri::Device&) -> const nri::DeviceDesc& { return limited; };
        auto rejected = runTask(GraphCompiler::compile(
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(),
            {graph, analysis}, {.exercise_async = true}
        ));
        core.GetDeviceDesc = get_device_desc;
        CHECK(!rejected && rejected.error().type == kDeviceBudgetExceeded);
        CHECK(rejected.error().args[0] == 72 && rejected.error().args[1] == 64);
        CHECK(rejected.error().args[2] == static_cast<std::uint64_t>(EDeviceBudget::ROOT_BYTES));
        std::puts("ROOT RANGE PASS: table DWORDs exhaust the native D3D12 budget before layout creation");
    }
    auto plan = compile(*session, graph, analysis, nullptr);
    FrameHarness frames(*session, 1);
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, 1);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    const std::array instances{instance_job.get()};
    CHECK(take(runTask(frames.render(instances, {}))) > 0);
    CHECK(session->drain());
    const std::array expected{matrix_value, array_value, 27u, 0u};
    CHECK(readBufferWords<4>(*session, *instance_job->resourceInput(readback, 0).buffer) == expected);
    std::puts("CONSTANT LAYOUT PASS: typed UBO binding, incomplete ABI and offset rejection, native values");
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

static void frozenProjectionReceipts(Validation& validation, bool d3d12)
{
    DeviceOptions options;
    if (d3d12) options.backend = EGraphicsBackend::D3D12;
    TestDevice device(options, diagnostics(validation), nativeValidationEnabled());
    auto& core = const_cast<nri::CoreInterface&>(device->nriCore());
    nri::Buffer* native = nullptr;
    const nri::BufferDesc description{16, 0, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE};
    CHECK(core.CreatePlacedBuffer(device->device(), NriDeviceHeap, description, native) == nri::Result::SUCCESS);
    auto old = device->makeFrontiers(), interim = device->makeFrontiers(), next = device->makeFrontiers();
    {
        RenderGraphBuilder builder;
        const auto buffer = take(builder.declareResource({"external", GraphBufferDesc{16}, EGraphOrigin::IMPORTED}));
        CHECK(builder.addTransferPass("zero external", TransferWork{{FillBuffer{{{buffer, 0}}, 0}}}));
        const auto graph = take(std::move(builder).finish());
        auto compiled = compile(*device, graph, take(analyzeRenderGraph(graph)), nullptr);
        JobGpuResources gpu(*device, 1);
        JobExecution instance_job{gpu, compiled.graph, testInstallation(), TargetOutput{}};
        CHECK(instance_job.initialize());
        const auto publish = [&](SubmissionFrontiers& destination)
        {
            instance_job.bindResource(buffer, 0, {.buffer = native, .size = 16, .queue_uses = {nullptr, &destination}});
        };
        publish(old);
        FrameHarness frames(*device, 1);
        const std::array instances{&instance_job};
        RecordingGate::original = core.CmdZeroBuffer;
        RecordingGate::entered = 0;
        RecordingGate::released = false;
        core.CmdZeroBuffer = &RecordingGate::zero;
        std::thread worker([&] { CHECK(take(runTask(frames.render(instances, {}))) > 0); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!RecordingGate::entered.load())
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(instance_job.frozenForSubmission(0));
        publish(interim);
        publish(next);
        RecordingGate::released = true;
        RecordingGate::released.notify_all();
        worker.join();
        core.CmdZeroBuffer = RecordingGate::original;
        CHECK(device->drain());
        const auto peak = [](const SubmissionFrontiers& uses) { return std::ranges::max(uses.serials()); };
        const auto first = peak(old);
        CHECK(first && !peak(interim) && !peak(next));
        CHECK(take(runTask(frames.render(instances, {}))) > 0 && device->drain());
        CHECK(peak(old) == first && !peak(interim) && peak(next) > first);
    }
    CHECK(device->drain());
    core.DestroyBuffer(native);
    CHECK(validation.errors == 0);
    std::puts("FROZEN PROJECTION PASS: two replacements during recording; only the frozen backing received "
              "the first accepted frontier and only the final replacement received the next");
}

static void failedRecordingAllocation(DeviceSession& session, const compiled::RenderGraph& graph)
{
    auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
    static decltype(core.DestroyCommandAllocator) destroy = nullptr;
    static unsigned destroyed = 0;
    destroyed = 0;
    destroy = core.DestroyCommandAllocator;
    const auto create = core.CreateCommandBuffer;
    JobGpuResources gpu(session, 1);
    JobExecution job{gpu, graph, testInstallation(), {}};
    core.DestroyCommandAllocator = [](nri::CommandAllocator* allocator)
    {
        ++destroyed;
        destroy(allocator);
    };
    core.CreateCommandBuffer = [](nri::CommandAllocator&, nri::CommandBuffer*&) { return nri::Result::OUT_OF_MEMORY; };
    const auto failed = job.initialize();
    core.CreateCommandBuffer = create;
    core.DestroyCommandAllocator = destroy;
    CHECK(!failed && failed.error().type == kDeviceFailure);
    CHECK(gpu.frames.front().batches.empty() && destroyed == 1);
    std::puts("BATCH CREATION ROLLBACK PASS: failed CB released its allocator; no native batch published");
}

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
    const auto graph = [&]
    {
        RenderGraphBuilder builder;
        const auto buffer = take(builder.declareResource({"recording", GraphBufferDesc{16}}));
        CHECK(builder.addTransferPass("recording", TransferWork{{FillBuffer{{{buffer, 0}}, 0}}}, {true}));
        const auto source = take(std::move(builder).finish());
        auto compiled = compile(*session, source, take(analyzeRenderGraph(source)), nullptr);
        return std::move(compiled.graph);
    }();
    failedRecordingAllocation(*session, graph);
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    {
        TestDevice other(options, diagnostics(validation), nativeValidationEnabled());
        JobGpuResources other_gpu(*other, 2);
        JobExecution misplaced_job{other_gpu, graph, testInstallation(), TargetOutput{}};
        const auto rejected = misplaced_job.initialize();
        CHECK(!rejected && rejected.error().type == kDeviceArgument);
        CHECK(other->queue().submitted() == 0);
    }
    std::puts("DEVICE GRAPH PASS: foreign device rejected before submission; owning device retained its graph");
    CHECK(take(instance_job->freezeFrame(0)));
    CHECK(take(instance_job->freezeFrame(1)));
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
    std::thread first_worker([&] { first = recordFrame(*instance_job, *session, 0, 0); });
    waitEntered(1);
    const auto duplicate = recordFrame(*instance_job, *session, 0, 0);
    CHECK(!duplicate && duplicate.error().type == kDeviceBusy);
    CHECK(RecordingGate::entered == 1);
    std::thread second_worker([&] { second = recordFrame(*instance_job, *session, 1, 0); });
    waitEntered(2); // Distinct FIF allocators can record concurrently.
    RecordingGate::released.store(true);
    RecordingGate::released.notify_all();
    first_worker.join();
    second_worker.join();
    CHECK(first && second && first->front() != second->front());

    auto frames = std::make_unique<FrameHarness>(*session, 2);
    const std::array instances{instance_job.get()};
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

    {
        JobGpuResources retry_gpu(*session, 1);
        JobExecution retry_job_job{retry_gpu, graph, testInstallation(), TargetOutput{}};
        CHECK(retry_job_job.initialize() && &retry_job_job.graph() == &instance_job->graph());
        FrameHarness retry_frames(*session, 1);
        const std::array retry_jobs{&retry_job_job};
        EndFailure::original = core.EndCommandBuffer;
        EndFailure::fail = true;
        EndFailure::calls = 0;
        core.EndCommandBuffer = &EndFailure::end;
        const auto before = session->queue().submitted();
        const auto failed = runTask(retry_frames.render(retry_jobs, {}));
        CHECK(!failed && failed.error().type == kDeviceFailure && EndFailure::calls == 1);
        CHECK(session->queue().submitted() == before);
        CHECK(take(runTask(retry_frames.render(retry_jobs, {}))) > 0);
        CHECK(EndFailure::calls == 2 && session->queue().submitted() > before);
        core.EndCommandBuffer = EndFailure::original;
        CHECK(session->drain());
    }
    std::puts("FROZEN WORKER RETRY PASS: failed End submitted nothing; reused schedule recorded and submitted on retry");

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
    CHECK(!instance_job->freezeFrame(0));
    CHECK(!recordFrame(*instance_job, *session, 0, 0));
    CHECK(RecordingGate::entered == after_loss);
    core.CmdZeroBuffer = RecordingGate::original;
    (void)session->progressCompletions();
    frames.reset();
    instance_job.reset();
    instance_gpu.reset();
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
        const auto& recording = plan.graph.recordingProgram();
        CHECK(recording.raster_scopes.size() == 2 && recording.passes.size() == 3);
        CHECK(recording.rasterScope(recording.passes[0]) == &recording.raster_scopes[0]);
        CHECK(recording.rasterScope(recording.passes[1]) == &recording.raster_scopes[1]);
        CHECK(!recording.rasterScope(recording.passes[2]) && recording.transferCommands(recording.passes[2]));
        CHECK(plan.graph.passes()[0].rendering_group == 0);
        CHECK(plan.graph.passes()[1].rendering_group == plan.graph.passes()[0].rendering_group);
        CHECK(plan.graph.programs()[0].pipeline != plan.graph.programs()[1].pipeline);
        const auto duplicate = compile(*session, graph, analysis, nullptr);
        CHECK(duplicate.graph.programs()[0].pipeline == plan.graph.programs()[0].pipeline);
        CHECK(duplicate.graph.programs()[1].pipeline == plan.graph.programs()[1].pipeline);
        auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
        auto instance_job = std::make_unique<JobExecution>(
            *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
        );
        CHECK(instance_job->initialize());
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance_job.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1 && session->drain());
        checkPixels(*session, *instance_job->resourceInput(pixels, 0).buffer, 32, 0, {255, 0, 0, 255}, 256);
        instance_job.reset();
        instance_gpu.reset();
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
        auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
        auto instance_job = std::make_unique<JobExecution>(
            *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
        );
        CHECK(instance_job->initialize());
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance_job.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1 && session->drain());
        checkPixels(*session, *instance_job->resourceInput(pixels, 0).buffer, 32, 0, {255, 0, 0, 255}, 256);
        instance_job.reset();
        instance_gpu.reset();
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
            *session, lux::execution::ExecutionRuntime::instance().cpuScheduler(), {graph, analysis}
        ));
        CHECK(rejected && !std::get<0>(*rejected) && std::get<0>(*rejected).error().type == kDeviceArgument);
    }
    {
        RenderGraphBuilder builder;
        const auto source = take(builder.declareResource({"source", GraphBufferDesc{32}}));
        const auto zero_source = take(builder.declareResource({"zero source", GraphBufferDesc{16}}));
        const auto output = take(builder.declareResource({"output", GraphBufferDesc{32}}));
        const auto zero_output = take(builder.declareResource({"zero output", GraphBufferDesc{16}}));
        const auto seed = take(builder.addTransferPass("seed", TransferWork{{
            FillBuffer{{{source, 0}}, 7},
            FillBuffer{{{zero_source, 0}}, 9}
        }}));
        const auto fill = take(builder.addTransferPass("mixed fills", TransferWork{{
            FillBuffer{{written(seed, source), GraphBufferRange{4, 4}}, 0},
            FillBuffer{{written(seed, zero_source), GraphBufferRange{0, 4}}, 0},
            FillBuffer{{written(seed, source), GraphBufferRange{8, 8}}, 11},
            FillBuffer{{written(seed, source), GraphBufferRange{16, 4}}, 0},
            FillBuffer{{written(seed, zero_source), GraphBufferRange{8, 4}}, 0},
            FillBuffer{{written(seed, source), GraphBufferRange{24, 4}}, 19}
        }}));
        const auto copy = take(builder.addTransferPass("copy", TransferWork{{
            CopyBuffer{{written(fill, source)}, {{output, 0}}},
            CopyBuffer{{written(fill, zero_source)}, {{zero_output, 0}}}
        }}));
        CHECK(builder.exportOutput({written(copy, output)}));
        CHECK(builder.exportOutput({written(copy, zero_output)}));
        auto plan = [&]
        {
            const auto graph = take(std::move(builder).finish());
            return compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
        }();
        auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
        auto instance_job = std::make_unique<JobExecution>(
            *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
        );
        CHECK(instance_job->initialize());
        CHECK(take(instance_job->freezeFrame(0)));
        auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
        EndFailure::original = core.EndCommandBuffer;
        EndFailure::fail = true;
        EndFailure::calls = 0;
        core.EndCommandBuffer = &EndFailure::end;
        const auto rejected = recordFrame(*instance_job, *session, 0, 0);
        CHECK(!rejected && session->queue().submitted() == 0 && EndFailure::calls == 1);
        CHECK(recordFrame(*instance_job, *session, 0, 0) && EndFailure::calls == 2);
        CHECK(recordFrame(*instance_job, *session, 0, 0) && EndFailure::calls == 2);
        core.EndCommandBuffer = EndFailure::original;
        auto frames = std::make_unique<FrameHarness>(*session, 2);
        const std::array instances{instance_job.get()};
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto values = readBufferWords<8>(*session, *instance_job->resourceInput(output, 0).buffer);
        const std::array<std::uint32_t, 8> expected{7, 0, 11, 11, 0, 7, 19, 7};
        CHECK(values == expected);
        const auto zeros = readBufferWords<4>(*session, *instance_job->resourceInput(zero_output, 0).buffer);
        const std::array<std::uint32_t, 4> expected_zeros{0, 9, 0, 9};
        CHECK(zeros == expected_zeros);
        instance_job.reset();
        instance_gpu.reset();
        CHECK(session->drain());
    }
    std::printf("TYPED RECORDING PASS backend=%s: detached source, End retry and mixed FillBuffer readback\n",
        d3d12 ? "D3D12" : "Vulkan");
}

struct ComputeSequenceFixture final
{
    RenderGraph graph;
    GraphResourceId intermediate;
    GraphResourceId readback;
};

static ComputeWork incrementWork(std::uint32_t program, ResourceVersion input, GraphResourceId output)
{
    ComputeWork work;
    work.interface.program = program;
    work.interface.resources = {
        {"previous_sample", {input}}, {"current_sample", {{output, 0}, {}, EGraphAccess::WRITE}}
    };
    return work;
}

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
        return incrementWork(program, input, output);
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

static void checkComputeSequence(const compiled::RenderGraph& executable, GraphResourceId intermediate)
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

struct NativeBindingCounts final
{
    unsigned layouts = 0, pipelines = 0, sets = 0, constants = 0;
};

// The single-batch fixtures record on one worker. Capture actual calls and each dispatch interval.
class NativeBindingProbe final
{
public:
    explicit NativeBindingProbe(nri::CoreInterface& core) : core_(core)
    {
        saved_ = core;
        since_dispatch = {};
        samples = {};
        dispatches = 0;
        core.CmdSetPipelineLayout = [](nri::CommandBuffer& cb, nri::BindPoint point, const nri::PipelineLayout& layout)
        {
            ++since_dispatch.layouts;
            saved_.CmdSetPipelineLayout(cb, point, layout);
        };
        core.CmdSetPipeline = [](nri::CommandBuffer& cb, const nri::Pipeline& pipeline)
        {
            ++since_dispatch.pipelines;
            saved_.CmdSetPipeline(cb, pipeline);
        };
        core.CmdSetDescriptorSet = [](nri::CommandBuffer& cb, const nri::SetDescriptorSetDesc& set)
        {
            ++since_dispatch.sets;
            saved_.CmdSetDescriptorSet(cb, set);
        };
        core.CmdSetRootConstants = [](nri::CommandBuffer& cb, const nri::SetRootConstantsDesc& block)
        {
            ++since_dispatch.constants;
            saved_.CmdSetRootConstants(cb, block);
        };
        core.CmdDispatch = [](nri::CommandBuffer& cb, const nri::DispatchDesc& dims)
        {
            CHECK(dispatches < samples.size());
            samples[dispatches++] = std::exchange(since_dispatch, {});
            saved_.CmdDispatch(cb, dims);
        };
    }

    ~NativeBindingProbe()
    {
        core_.CmdSetPipelineLayout = saved_.CmdSetPipelineLayout;
        core_.CmdSetPipeline = saved_.CmdSetPipeline;
        core_.CmdSetDescriptorSet = saved_.CmdSetDescriptorSet;
        core_.CmdDispatch = saved_.CmdDispatch;
        core_.CmdSetRootConstants = saved_.CmdSetRootConstants;
    }

    static inline std::array<NativeBindingCounts, 3> samples;
    static inline unsigned dispatches = 0;
    static inline NativeBindingCounts since_dispatch;

private:
    nri::CoreInterface& core_;
    static inline nri::CoreInterface saved_;
};

static void conditionalBindingJoin(DeviceSession& device)
{
    RenderGraphBuilder builder;
    const auto seed = take(builder.declareResource({"seed", GraphBufferDesc{16}}));
    const auto shared = take(builder.declareResource({"shared", GraphBufferDesc{16}}));
    const auto after = take(builder.declareResource({"after join", GraphBufferDesc{16}}));
    const auto result = take(builder.declareResource({"result", GraphBufferDesc{16}}));
    const auto readback = take(builder.declareResource({"readback", GraphBufferDesc{16}}));
    const auto color = take(builder.declareResource({"stream color", GraphImageDesc{4, 4}}));
    const auto palette_color = take(builder.declareResource({"palette color", GraphImageDesc{4, 4}}));
    const auto pixels = take(builder.declareResource({"stream pixels", GraphBufferDesc{1024}}));
    RasterWork palette;
    palette.fixed().interface.program = take(builder.addProgram(dualTargetRasterProgram()));
    palette.colors = {{{{palette_color, 0}}}};
    const auto fixed_draw = take(builder.addRasterPass("fixed red program", std::move(palette)));
    CHECK(builder.exportOutput({fixed_draw.written.front()}));
    const auto enabled = take(builder.declareCondition("overwrite"));
    const auto overwrite_again = take(builder.declareCondition("overwrite again"));
    const auto program = take(builder.addProgram(GraphProgram{kGraphHistory}));
    const auto initialized = take(builder.addTransferPass("initialize", TransferWork{{FillBuffer{{{seed, 0}}, 11}}}));
    auto before_work = incrementWork(program, initialized.written.front(), shared);
    const auto first = take(builder.addComputePass("before", std::move(before_work)));
    CHECK(builder.dependsOn(fixed_draw.pass, first.pass));
    const auto before = first.written.front();
    const auto branch = take(builder.addTransferPass(
        "conditional overwrite", TransferWork{{FillBuffer{{before, {}, EGraphAccess::WRITE, 0, before}, 20}}},
        {false, PassCondition{EConditionSource::INPUT, enabled}}
    ));
    const auto middle = branch.written.front();
    const auto again = take(builder.addTransferPass(
        "second overwrite", TransferWork{{FillBuffer{{middle, {}, EGraphAccess::WRITE, 0, middle}, 25}}},
        {false, PassCondition{EConditionSource::INPUT, overwrite_again}}
    ));
    const auto joined = take(builder.addComputePass("join", incrementWork(program, again.written.front(), after)));
    RasterWork stream;
    stream.program = RasterPassTemplate{"dynamic stage"};
    stream.colors = {{{{color, 0}}}};
    const auto dynamic = take(builder.addRasterPass("draw stream", std::move(stream)));
    auto after_stream = incrementWork(program, joined.written.front(), result);
    const auto last = take(builder.addComputePass("after stream", std::move(after_stream)));
    CHECK(builder.dependsOn(joined.pass, dynamic.pass) && builder.dependsOn(dynamic.pass, last.pass));
    const auto copy = take(builder.addTransferPass(
        "readback", TransferWork{{
            CopyBuffer{{last.written.front()}, {{readback, 0}}},
            CopyImageBuffer{{dynamic.written.front()}, {{pixels, 0}}, 256, 1024}
        }}
    ));
    CHECK(builder.exportOutput({written(copy, readback)}));
    exportRows(builder, written(copy, pixels), 4, 4, 256);
    const auto source = take(std::move(builder).finish());
    auto plan = compile(device, source, take(analyzeRenderGraph(source)), nullptr);
    const auto& passes = plan.graph.passes();
    const auto& entry = passes[plan.diagnostics.passSlot(first.pass)];
    const auto& join = passes[plan.diagnostics.passSlot(joined.pass)];
    const auto& resume = passes[plan.diagnostics.passSlot(last.pass)];
    CHECK(entry.submission == join.submission && join.submission == resume.submission);
    CHECK(join.binding_join != kGraphInvalidIndex && join.fixed_bindings.layout && join.fixed_bindings.pipeline);
    CHECK(resume.fixed_bindings.layout && resume.fixed_bindings.pipeline && resume.fixed_bindings.private_set);

    JobGpuResources gpu(device, 1);
    JobExecution job{gpu, plan.graph, testInstallation(), {}};
    CHECK(job.initialize());
    const auto& native_program = plan.graph.programs()[plan.diagnostics.passSlot(fixed_draw.pass)];
    auto& stream_draws = rasterDraws(job, passes[plan.diagnostics.passSlot(dynamic.pass)], 0);
    auto& red = stream_draws.packets.emplace_back();
    red.pipeline = native_program.pipeline;
    red.bindings = native_program.descriptors;
    red.draw = nri::DrawDesc{3, 1, 0, 0};
    FrameHarness frames(device, 1);
    const std::array jobs{&job};
    using Conditions = std::array<std::uint8_t, 2>;
    for (const auto values : {Conditions{0, 0}, Conditions{1, 0}, Conditions{1, 1}, Conditions{0, 1}, Conditions{0, 0}})
    {
        CHECK(job.setConditions(values));
        {
            NativeBindingProbe probe(const_cast<nri::CoreInterface&>(device.nriCore()));
            CHECK(take(runTask(frames.render(jobs, {}))) > 0);
            finishSubmissions(device, frames);
            CHECK(probe.dispatches == 3);
            const auto bindings = probe.samples[1];
            if (values[0] || values[1]) CHECK(bindings.layouts > 0 && bindings.pipelines == 1);
            else CHECK(bindings.layouts == 0 && bindings.pipelines == 0 && bindings.sets == 1);
            CHECK(probe.samples[2].layouts == 2 && probe.samples[2].pipelines == 2);
        }
        CHECK(device.drain());
        checkPixels(device, *job.resourceInput(pixels, 0).buffer, 4, 0, {255, 0, 0, 255}, 256);
        const auto words = readBufferWords<4>(device, *job.resourceInput(readback, 0).buffer);
        const auto expected = values[1] ? 27u : (values[0] ? 22u : 14u);
        CHECK(std::ranges::all_of(words, [&](auto word) { return word == expected; }));
    }
    std::puts("FIXED BIND JOIN PASS: skipped regions 0 redundant binds, red stream pixels, GPU words 14/22/27/27/14");
}

static void surfaceBindingBoundary(DeviceSession& device)
{
    RenderGraphBuilder builder;
    const GraphImageDesc image{16, 16};
    const auto scratch = take(builder.declareResource({"head", image}));
    const auto surface = take(builder.declareResource({"surface", image}));
    const auto shader = take(builder.addProgram(dualTargetRasterProgram()));
    const auto draw = [&](GraphResourceId target)
    {
        RasterWork work;
        work.fixed().interface.program = shader;
        work.colors = {{{{target, 0}}}};
        return work;
    };
    const auto head = take(builder.addRasterPass("same pipeline head", draw(scratch)));
    const auto tail = take(builder.addRasterPass("same pipeline surface tail", draw(surface)));
    CHECK(builder.dependsOn(head.pass, tail.pass));
    CHECK(builder.exportOutput({head.written.front()}));
    CHECK(builder.exportOutput({tail.written.front(), {}, GraphOutputRequirements{image.format, 1}}));
    const auto source = take(std::move(builder).finish());
    const TargetDesc target{16, 16, image.format, 1, ETargetKind::SURFACE};
    const auto plan = compile(device, source, take(analyzeRenderGraph(source)), &target);
    const auto h = plan.diagnostics.passSlot(head.pass), t = plan.diagnostics.passSlot(tail.pass);
    const auto& passes = plan.graph.passes();
    CHECK(passes[h].submission == passes[t].submission);
    CHECK(plan.graph.submissions()[passes[t].submission].output_begin == t);
    CHECK(plan.graph.programs()[h].pipeline == plan.graph.programs()[t].pipeline);
    CHECK(passes[h].fixed_bindings.layout && passes[h].fixed_bindings.pipeline);
    CHECK(passes[t].fixed_bindings.layout && passes[t].fixed_bindings.pipeline);
    std::puts("FIXED BIND SURFACE PLAN PASS: identical pipeline and layout emitted at both physical CB starts");
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
    checkComputeSequence(executable.graph, intermediate);
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, 2);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, executable.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    auto frames = std::make_unique<FrameHarness>(*session, 2);
    const std::array instances{instance_job.get()};
    const auto& first = executable.graph.passes()[1].fixed_bindings;
    const auto& next = executable.graph.passes()[2].fixed_bindings;
    CHECK(first.layout && first.pipeline && first.private_set);
    CHECK(!next.layout && !next.pipeline && next.private_set);
    {
        NativeBindingProbe probe(const_cast<nri::CoreInterface&>(session->nriCore()));
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(probe.dispatches == 2);
        const auto counts = probe.samples[1];
        CHECK(counts.layouts == 0 && counts.pipelines == 0 && counts.sets == 1);
    }
    CHECK(session->drain());
    const auto values = readBufferWords<4>(*session, *instance_job->resourceInput(readback, 0).buffer);
    CHECK(std::ranges::all_of(values, [](auto value) { return value == 13; }));
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
    std::puts("FIXED BIND DELTA PASS: second dispatch 0 layout, 0 pipeline, 1 private set; GPU words 13/13/13/13");
    conditionalBindingJoin(*session);
    surfaceBindingBoundary(*session);
}

// This recorder implements the fixture's one known kernel and Buffer operations.
class ComputeSequenceRecorder final
{
public:
    explicit ComputeSequenceRecorder(const compiled::RenderGraph& executable)
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
        const auto& transfers = executable_.recordingProgram().transfers;
        for (const auto operation : transfers.commands(packet))
        {
            if (operation.opcode == ETransferOpcode::ZERO_BUFFER || operation.opcode == ETransferOpcode::FILL_STORAGE)
            {
                const auto& fill = transfers.fills[operation.operand];
                std::ranges::fill(words(fill.destination.slot, fill.destination.range), fill.value);
                continue;
            }
            if (operation.opcode == ETransferOpcode::COPY_BUFFER)
            {
                const auto& copy = transfers.buffer_copies[operation.operand];
                auto source = words(copy.source.slot, copy.source.range);
                auto destination = words(copy.destination.slot, copy.destination.range);
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
        const auto* dispatch = std::get_if<nri::DispatchDesc>(&packet.dispatch);
        CHECK(dispatch && dispatch->workGroupNumX == 1 && dispatch->workGroupNumY == 1 && dispatch->workGroupNumZ == 1);
        const auto uses = executable_.descriptorUses(pass.binding_slot);
        CHECK(uses.size() == 2);
        // The production layout packs private fields by name, placing current_sample before previous_sample.
        const auto output = std::ranges::find(uses, 0u, &DescriptorAccess::binding);
        const auto input = std::ranges::find(uses, 1u, &DescriptorAccess::binding);
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

    const compiled::RenderGraph& executable_;
    std::vector<std::vector<std::optional<Word>>> buffers_;
};

static compiled::RenderGraph
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
        {capabilities, queues}, {graph, take(analyzeRenderGraph(graph))}, ETransientAllocation::INDEPENDENT, policy
    ));
}

static void cpuRasterBindingReplacement()
{
    JobFrameResources native;
    FrameResources frame(native);
    frame.stream_descriptor_rows.resize(3);
    for (auto& stream : frame.stream_descriptor_rows) stream.programs.resize(2);
    std::array<std::uint32_t, 9> tokens{};
    const auto view = [&](std::size_t i) { return reinterpret_cast<nri::Descriptor*>(&tokens[i]); };
    const auto append = [&](RasterBindingKey key, std::size_t token)
    {
        auto& ids = frame.stream_descriptor_rows[key.pass - 7].programs[key.program].records;
        const auto index = static_cast<std::uint32_t>(native.raster_bindings.size());
        auto& record = native.raster_bindings.emplace_back();
        record.program = key;
        record.list = key.pass - 7;
        record.ordinal = static_cast<std::uint32_t>(ids.size());
        record.views.push_back(view(token));
        ids.push_back(index);
    };
    append({7, 0, 1}, 0);
    append({8, 1, 3}, 1);
    append({7, 1, 1}, 2);
    append({9, 0, 0}, 3);
    append({8, 1, 3}, 4);
    append({7, 0, 1}, 5);
    append({8, 0, 3}, 6);
    append({7, 1, 1}, 7);
    std::vector<nri::Descriptor*> retired;
    frame.releaseRasterBindings(0, retired);
    CHECK(retired.size() == 4 && native.raster_bindings.size() == 4);
    for (const auto token : {0, 2, 5, 7}) CHECK(std::ranges::count(retired, view(token)) == 1);
    const auto verify = [&](std::uint32_t pass, std::uint32_t program, std::span<const unsigned> expected)
    {
        const auto& ids = frame.stream_descriptor_rows[pass].programs[program].records;
        CHECK(ids.size() == expected.size());
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto& row = native.raster_bindings[ids[i]];
            CHECK(row.ordinal == i && row.program.pass == 7 + pass && row.program.program == program);
            CHECK(row.views.size() == 1 && row.views.front() == view(expected[i]));
        }
    };
    verify(1, 1, std::array{1u, 4u});
    verify(1, 0, std::array{6u});
    verify(2, 0, std::array{3u});
    frame.stream_descriptor_rows[0].programs.resize(1);
    append({7, 0, 2}, 8);
    frame.releaseRasterBindings(1, retired);
    CHECK(retired.size() == 7 && native.raster_bindings.size() == 2);
    verify(0, 0, std::array{8u});
    verify(2, 0, std::array{3u});
}

static void cpuAliasWaitScopes()
{
    using Stage = nri::StageBits;
    CHECK(combineStages(Stage::NONE, Stage::COPY) == Stage::COPY);
    CHECK(combineStages(Stage::ALL, Stage::COPY) == Stage::ALL);
    CHECK(!coversStages(Stage::NONE, Stage::COPY));
    CHECK(!coversStages(Stage::COPY, Stage::ALL));
    CHECK(!coversStages(Stage::COPY, Stage::COPY | Stage::CLEAR_STORAGE));
    CHECK(coversStages(Stage::ALL, Stage::COPY | Stage::CLEAR_STORAGE));
    AliasUseFrontier previous{}, next{};
    auto& entry = next.queues[0];
    entry.destination = {nri::AccessBits::COPY_DESTINATION, Stage::COPY};
    entry.source = {nri::AccessBits::COPY_DESTINATION | nri::AccessBits::COPY_SOURCE, Stage::COPY};
    auto& compute = previous.queues[1];
    compute.point_count = 1;
    compute.last_submission = 3;
    const auto check_full = [&]
    {
        const auto barrier = aliasHandoff(previous, next, 0);
        CHECK(barrier.before.stages == Stage::ALL && barrier.after.stages == Stage::ALL);
        CHECK(barrier.after.access == entry.source.access);
    };
    check_full();
    entry.waited_through[1] = 2;
    check_full();
    entry.waited_through[1] = 3;
    auto narrow = aliasHandoff(previous, next, 0);
    CHECK(narrow.before.stages == Stage::COPY && narrow.before.access == nri::AccessBits::NONE);
    CHECK(narrow.after.stages == Stage::COPY && narrow.after.access == entry.destination.access);
    previous.queues[2] = compute;
    check_full();
    entry.waited_through[2] = 4;
    previous.queues[0].point_count = 1;
    previous.queues[0].source = {nri::AccessBits::SHADER_RESOURCE, Stage::FRAGMENT_SHADER};
    narrow = aliasHandoff(previous, next, 0);
    CHECK(narrow.before.stages == (Stage::COPY | Stage::FRAGMENT_SHADER));
    CHECK(narrow.before.access == nri::AccessBits::SHADER_RESOURCE);
    // An uncovered initial layout has no qualifying direct wait.
    next.image = true;
    entry.waited_through[1] = kGraphInvalidIndex;
    check_full();
}

static void cpuAliasBufferRanges()
{
    RenderGraphBuilder builder;
    const auto buffer = take(builder.declareResource({"two intervals", GraphBufferDesc{64}}));
    const auto result = take(builder.declareResource({"observed", GraphBufferDesc{8}}));
    constexpr std::array ranges{GraphBufferRange{0, 4}, GraphBufferRange{48, 4}};
    ResourceVersion version{buffer, 0}, output{result, 0};
    for (unsigned i = 0; i < ranges.size(); ++i)
    {
        const auto filled = take(builder.addTransferPass(
            "write" + std::to_string(i), TransferWork{{FillBuffer{{version, ranges[i]}, 7 + i}}}
        ));
        version = filled.written.front();
        const auto copy = take(builder.addTransferPass(
            "observe" + std::to_string(i),
            TransferWork{{CopyBuffer{{version, ranges[i]}, {output, GraphBufferRange{4 * i, 4}}}}}
        ));
        output = copy.written.front();
        CHECK(builder.exportOutput({output, GraphBufferRange{4 * i, 4}}));
    }
    const auto graph = take(std::move(builder).finish());
    const QueueProperties queue;
    nri::DeviceDesc caps{};
    caps.graphicsAPI = nri::GraphicsAPI::VK;
    const auto plan = take(GraphCompiler::compileForRecordingTest(
        {caps, {&queue, &queue, &queue}}, {graph, take(analyzeRenderGraph(graph))}, ETransientAllocation::ALIAS_MEMORY
    ));
    const auto& physical = plan.physicalResources();
    const auto& frontiers = physical.alias_frontiers[physical.resource_slots[buffer.index].index].queues;
    CHECK(frontiers[1].point_count == 0 && frontiers[2].point_count == 0);
    const auto points = std::span{physical.alias_points}.subspan(frontiers[0].first_point, frontiers[0].point_count);
    CHECK(points.size() == 4);
    for (const auto range : ranges)
    {
        CHECK(std::ranges::any_of(points, [&](const AliasUsePoint& p)
        {
            return p.writes && p.needs_entry && std::get<GraphBufferRange>(p.range) == range;
        }));
        CHECK(std::ranges::any_of(points, [&](const AliasUsePoint& p)
        {
            return !p.writes && !p.needs_entry && std::get<GraphBufferRange>(p.range) == range;
        }));
    }
}

static void cpuBuilderTransactions()
{
    RenderGraphBuilder builder;
    CHECK(builder.addProgram(GraphProgram{kGraphFill}));
    CHECK(std::move(builder).finish());
    const auto shader = take(builder.addProgram(GraphProgram{kGraphHistory}));
    const auto input = take(builder.declareResource({"input", GraphBufferDesc{64}}));
    const auto result = take(builder.declareResource({"result", GraphBufferDesc{64}}));
    const auto image = take(builder.declareResource({"color", GraphImageDesc{8, 8}}));
    const auto seed = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{input, 0}}, 1}}}));
    const auto initial = take(builder.nextWriteVersion(result));

    const auto checkUnchanged = [&]
    {
        CHECK(builder.passCount() == 1 && builder.dependencyCount() == 0);
        CHECK(take(builder.nextWriteVersion(result)) == initial);
        CHECK(take(builder.nextWriteVersion(image)).number == 1);
    };
    const auto wrong_kind = builder.addTransferPass("buffer clear", TransferWork{{
        FillBuffer{{{result, 0}}, 3}, ClearImage{{{result, 0}}}
    }});
    CHECK(!wrong_kind && wrong_kind.error().type == kGraphInvalidDescriptor);
    checkUnchanged();
    const auto conflict = builder.addTransferPass("overlap", TransferWork{{
        FillBuffer{{{result, 0}, GraphBufferRange{0, 8}}, 3}, FillBuffer{{{result, 0}, GraphBufferRange{4, 4}}, 2}
    }});
    CHECK(!conflict && conflict.error().type == kGraphConflictingOutput);
    checkUnchanged();

    RasterWork raster;
    raster.fixed().interface.program = shader;
    raster.colors.push_back({{{image, 0}}});
    raster.fixed().draw = DrawIndexedIndirect{
        seed.written.front(), 0, EIndexType::UINT32, seed.written.front(), 0, 2, 16
    };
    const auto bad_draw = builder.addRasterPass("short stride", std::move(raster));
    CHECK(!bad_draw && bad_draw.error().type == kGraphInvalidRange);
    checkUnchanged();

    ComputeWork compute;
    compute.interface.program = shader;
    compute.interface.resources = {
        {"previous_sample", {seed.written.front()}}, {"current_sample", {{result, 0}, {}, EGraphAccess::WRITE}}
    };
    auto invalid = compute;
    invalid.interface.resources.push_back(invalid.interface.resources.back());
    const auto bad_field = builder.addComputeSequence("duplicate field", {compute, invalid});
    CHECK(!bad_field && bad_field.error().type == kGraphInvalidDescriptor);
    checkUnchanged();

    const auto sequence = take(builder.addComputeSequence("retry", {compute, compute}));
    CHECK(sequence.pass.index == 1 && sequence.written.size() == 1 && sequence.written.front().number == 2);
    CHECK(builder.dependencyCount() == 1 && builder.passCount() == 3);
    const auto history = take(builder.declareHistory("history", {GraphBufferDesc{64}}));
    ResourceAccess previous{{history.previous, 0}, {}, EGraphAccess::READ, 0, seed.written.front(), EGraphUse::STORAGE_READ};
    RasterPassTemplate stream;
    stream.purpose = "read alternatives";
    stream.resources = {previous, previous};
    previous.fallback = sequence.written.front();
    stream.resources.push_back(previous);
    previous.range = GraphBufferRange{0, 64};
    stream.resources.push_back(previous);
    RasterWork alternatives;
    alternatives.program = std::move(stream);
    alternatives.colors.push_back({{{image, 0}}});
    const auto streamed = take(builder.addRasterPass("read alternatives", std::move(alternatives)));

    RasterWork padded;
    padded.fixed().interface.program = shader;
    padded.colors.push_back({{streamed.written.front()}});
    padded.fixed().draw = DrawIndexedIndirect{
        seed.written.front(), 0, EIndexType::UINT32, seed.written.front(), 0, 2, 24
    };
    const auto strided = take(builder.addRasterPass("strided commands", std::move(padded)));
    const auto graph = take(std::move(builder).finish());
    const auto& first = graph.passes()[1];
    const auto fields = first.bindings();
    CHECK(fields.shader == &std::get<ComputeWork>(first.command).interface);
    CHECK(fields.shader->resources[0].field != fields.shader->resources[1].field);
    CHECK(first.sequence_first.index == 1 && first.sequence_last.index == 2);
    const auto& uses = graph.passes()[streamed.pass.index].uses;
    CHECK(uses.size() == 4 && uses[0].fallback == seed.written.front() && uses[1].fallback == sequence.written.front());
    CHECK(uses[1].whole_resource && !uses[2].whole_resource);
    const auto& geometry = graph.passes()[strided.pass.index].uses;
    CHECK(geometry.size() == 4 && geometry[2].usage == EGraphUse::INDIRECT && geometry[3].usage == EGraphUse::INDIRECT);
    CHECK((std::get<GraphBufferRange>(geometry[2].range) == GraphBufferRange{0, 20}));
    CHECK((std::get<GraphBufferRange>(geometry[3].range) == GraphBufferRange{24, 20}));
    std::puts("BUILDER TRANSACTION PASS: range/field/stride errors, sequence retry and reflection after Builder reuse");
}

template <class Graph>
concept BorrowsTemporaryCommands = requires(Graph&& graph) { std::move(graph).recordingProgram(); };
static_assert(!BorrowsTemporaryCommands<compiled::RenderGraph>);

template <class Job>
concept BorrowsTemporaryFrame = requires(Job&& job) { std::move(job).frame(0); };
static_assert(!BorrowsTemporaryFrame<JobExecution>);

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
    const auto& program = executable.recordingProgram();
    const auto& transfers = program.transfers;
    const auto native_bytes = transfers.buffer_copies.size() * sizeof(BufferCopyRegion) +
        transfers.image_copies.size() * sizeof(TextureCopyRegion) + transfers.image_buffers.size() * sizeof(FrozenImageBuffer) +
        transfers.fills.size() * sizeof(FrozenBufferFill) + transfers.clears.size() * sizeof(nri::AttachmentDesc);
    std::printf("PACKET TABLES compute=%zu transfer_instructions=%zu opcode_bytes=%zu native_operand_bytes_per_fif=%zu\n",
        program.compute.size(), transfers.instructions.size(), sizeof(TransferInstruction), native_bytes);
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

static void cpuConditionalJoins()
{
    for (const bool independent : {false, true})
    {
        for (const std::uint64_t covered : {12u, 16u})
        {
            RenderGraphBuilder builder;
            const auto branch = take(builder.declareCondition("branch"));
            const auto other = independent ? take(builder.declareCondition("other")) : branch;
            const auto data = take(builder.declareResource({"data", GraphBufferDesc{16}}));
            const auto first = take(builder.addTransferPass(
                "first", TransferWork{{FillBuffer{{{data, 0}}, 7}}},
                {false, PassCondition{EConditionSource::INPUT, branch, true}}
            ));
            const auto second = take(builder.addTransferPass(
                "second", TransferWork{{FillBuffer{{first.written.front(), GraphBufferRange{0, covered}}, 11}}},
                {false, PassCondition{EConditionSource::INPUT, other, false}}
            ));
            CHECK(builder.exportOutput({second.written.front()}));
            const auto graph = take(std::move(builder).finish());
            if (independent || covered < 16)
            {
                const auto result = analyzeRenderGraph(graph);
                CHECK(!result && result.error().type == kGraphUndefinedRead);
                continue;
            }
            const auto executable = compileCpuGraph(graph);
            for (const std::uint8_t selected : {0, 1})
            {
                ComputeSequenceRecorder recorder(executable);
                recorder.execute({&selected, 1});
                const auto expected = selected ? 7u : 11u;
                recorder.verify(data, std::array{expected, expected, expected, expected});
            }
        }
    }
}

static void cpuValueChoice()
{
    RenderGraphBuilder builder;
    const auto flag = take(builder.declareCondition("select input"));
    const PassCondition predicate{EConditionSource::INPUT, flag};
    const auto input = take(builder.declareResource({"input", GraphBufferDesc{16}}));
    const auto alternate = take(builder.declareResource({"alternate", GraphBufferDesc{16}}));
    const auto output = take(builder.declareResource({"output", GraphBufferDesc{16}}));
    const auto seed = take(builder.addTransferPass("initialize", TransferWork{{FillBuffer{{{input, 0}}, 7}}}));
    const auto make_value = [&](RenderGraphBuilder& branch) noexcept -> RenderResult<ResourceVersion>
    {
        auto written = branch.addTransferPass("generate", TransferWork{{FillBuffer{{{alternate, 0}}, 11}}});
        if (!written) return lux::cxx::unexpected(written.error());
        return written->written.front();
    };
    const auto fail = [](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion>
    {
        return lux::cxx::unexpected(RenderError{kGraphInvalidDescriptor});
    };
    const auto rejected = builder.choose(predicate, make_value, fail);
    CHECK(!rejected && builder.passCount() == 1);
    CHECK(take(builder.nextWriteVersion(alternate)).number == 1);
    const auto chosen = take(builder.choose(
        predicate, [&](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion> { return seed.written.front(); },
        make_value
    ));
    CHECK(builder.passCount() == 2 && builder.resourceCount() == 3);
    const auto copied = take(builder.addTransferPass("observe", TransferWork{{CopyBuffer{{chosen}, {{output, 0}}}}}));
    CHECK(copied.written.size() == 1 && copied.written.front().number == 1);
    CHECK(builder.dependsOn(seed.pass, copied.pass));
    CHECK(builder.dependencyCount() == 2);
    CHECK(builder.exportOutput({chosen}));
    CHECK(builder.outputCount() == 1);
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto intermediate = take(builder.declareResource({"sequence intermediate", GraphBufferDesc{16}}));
    const auto final = take(builder.declareResource({"sequence result", GraphBufferDesc{16}}));
    const auto program = take(builder.addProgram(GraphProgram{kGraphHistory}));
    const auto next = take(builder.nextWriteVersion(intermediate));
    const auto sequence = take(builder.addComputeSequence(
        "increment twice", {incrementWork(program, chosen, intermediate), incrementWork(program, next, final)}
    ));
    CHECK(builder.exportOutput({written(sequence, final)}));
    const auto executable = compileCpuGraph(take(std::move(builder).finish()));
    for (const std::uint8_t selected : {0, 1})
    {
        ComputeSequenceRecorder recorder(executable);
        recorder.execute({&selected, 1});
        const auto word = selected ? 7u : 11u;
        recorder.verify(output, std::array{word, word, word, word});
        recorder.verify(final, std::array{word + 2, word + 2, word + 2, word + 2});
    }
}

static void cpuUndefinedChoice()
{
    for (const bool escape : {false, true})
    {
        RenderGraphBuilder builder;
        const auto flag = take(builder.declareCondition("path"));
        const auto first = take(builder.declareResource({"first", GraphBufferDesc{16}}));
        const auto second = take(builder.declareResource({"second", GraphBufferDesc{16}}));
        const auto seed = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{first, 0}}, 3}}}));
        ResourceVersion private_value;
        const auto choice = take(builder.choose(
            {EConditionSource::INPUT, flag},
            [&](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion> { return seed.written.front(); },
            [&](RenderGraphBuilder& arm) noexcept -> RenderResult<ResourceVersion>
            {
                const GraphBufferRange range{0, escape ? 16u : 12u};
                auto write = arm.addTransferPass("private write", TransferWork{{FillBuffer{{{second, 0}, range}, 9}}});
                if (!write) return lux::cxx::unexpected(write.error());
                private_value = write->written.front();
                return private_value;
            }
        ));
        CHECK(builder.exportOutput({escape ? private_value : choice}));
        const auto proof = analyzeRenderGraph(take(std::move(builder).finish()));
        CHECK(!proof && proof.error().type == kGraphUndefinedRead);
    }
}

static void cpuChoiceImageRanges()
{
    const GraphImageRange provided{1, 1, 1, 0, 1};
    const GraphImageRange missing_mip{1, 0, 1, 0, 1};
    const GraphImageRange missing_layer{1, 1, 1, 1, 1};
    for (const auto range : {provided, missing_mip, missing_layer})
    {
        RenderGraphBuilder builder;
        const GraphImageDesc image{8, 8, 2, 2};
        const auto complete = take(builder.declareResource({
            "all", image, EGraphOrigin::IMPORTED, {GraphImageRange{1, 0, 2, 0, 2}}, EGraphUse::SAMPLED
        }));
        const auto partial = take(builder.declareResource({
            "partial", image, EGraphOrigin::IMPORTED, {provided}, EGraphUse::SAMPLED
        }));
        const auto selected = take(builder.choose(
            take(builder.sameBinding(complete, partial)),
            [&](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion> { return ResourceVersion{complete, 0}; },
            [&](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion> { return ResourceVersion{partial, 0}; }
        ));
        CHECK(builder.exportOutput({selected, range}));
        const auto proof = analyzeRenderGraph(take(std::move(builder).finish()));
        if (range == provided) CHECK(proof && proof->execution_order.empty());
        else CHECK(!proof && proof.error().type == kGraphMissingInitial);
    }
}

static void conditionalHistorySamples(DeviceSession& session, std::uint32_t fif, bool hold)
{
    RenderGraphBuilder builder;
    const auto branch = take(builder.declareCondition("branch"));
    const auto publish = take(builder.declareCondition("publish"));
    const auto distance = hold ? 2u : 1u;
    const auto history = take(builder.declareHistory(
        "samples", {GraphBufferDesc{16}, {}, EGraphUse::TRANSFER_SOURCE, distance}
    ));
    const auto readback = take(builder.declareResource({"previous", GraphBufferDesc{16}}));
    const auto write = [&](RenderGraphBuilder& arm, std::string name, std::uint32_t value) -> RenderResult<ResourceVersion>
    {
        auto result = arm.addTransferPass(std::move(name), TransferWork{{FillBuffer{{{history.current, 0}}, value}}});
        if (!result) return lux::cxx::unexpected(result.error());
        return result->written.front();
    };
    const auto sample = take(builder.choose(
        {EConditionSource::INPUT, branch},
        [&](RenderGraphBuilder& arm) noexcept { return write(arm, "first", 7); },
        [&](RenderGraphBuilder& arm) noexcept -> RenderResult<ResourceVersion>
        {
            if (hold) return ResourceVersion{history.previous, 0};
            return write(arm, "second", 11);
        }
    ));
    CHECK(builder.publishHistory(sample, PassCondition{EConditionSource::INPUT, publish}));
    const auto copied = take(builder.addTransferPass(
        "read previous", TransferWork{{CopyBuffer{{{history.previous, 0}}, {{readback, 0}}}}}
    ));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto plan = compile(session, graph, take(analyzeRenderGraph(graph)), nullptr);
    JobGpuResources instance_gpu(session, fif);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
    CHECK(instance_job.initialize());
    FrameHarness frames(session, fif);
    const std::array instances{&instance_job};
    std::vector<std::uint32_t> published;
    constexpr std::array<std::uint8_t, 2> selections[] = {{1, 1}, {0, 1}, {1, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& inputs : selections)
    {
        CHECK(instance_job.setConditions(inputs));
        auto submitted = take(runTask(frames.render(instances, {})));
        submitted += finishSubmissions(session, frames);
        CHECK(submitted == 1);
        CHECK(session.drain());
        const auto slot = instance_job.progress.last_submitted_slot;
        const auto values = readBufferWords<4>(session, *instance_job.resourceInput(readback, slot).buffer);
        const auto previous = published.size() < distance ? 0u : published[published.size() - distance];
        CHECK(std::ranges::all_of(values, [previous](auto word) { return word == previous; }));
        if (inputs[1] && (inputs[0] || !hold)) published.push_back(inputs[0] ? 7u : 11u);
    }
}

static void crossQueueChoice(DeviceSession& session, std::uint32_t fif)
{
    RenderGraphBuilder builder;
    const auto flag = take(builder.declareCondition("use input"));
    const auto input = take(builder.declareResource({"input", GraphBufferDesc{16}}));
    const auto scratch = take(builder.declareResource({"incremented", GraphBufferDesc{16}}));
    const auto result = take(builder.declareResource({"readback", GraphBufferDesc{16}}));
    const auto seed = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{input, 0}}, 7}}}));
    const auto program = take(builder.addProgram(GraphProgram{kGraphHistory}));
    const auto selected = take(builder.choose(
        {EConditionSource::INPUT, flag},
        [&](RenderGraphBuilder&) noexcept -> RenderResult<ResourceVersion> { return seed.written.front(); },
        [&](RenderGraphBuilder& arm) noexcept -> RenderResult<ResourceVersion>
        {
            auto work = incrementWork(program, seed.written.front(), scratch);
            work.allow_async_compute = true;
            auto pass = arm.addComputePass("increment", std::move(work));
            if (!pass) return lux::cxx::unexpected(pass.error());
            return pass->written.front();
        }
    ));
    const auto copied = take(builder.addTransferPass("observe", TransferWork{{CopyBuffer{{selected}, {{result, 0}}}}}));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto plan = compile(session, graph, take(analyzeRenderGraph(graph)), nullptr);
    const bool distinct = &session.queue() != &session.queue(EQueueRole::COMPUTE);
    const auto batches = plan.graph.submissions();
    if (distinct)
    {
        CHECK(std::ranges::any_of(batches, [](const auto& batch) { return batch.queue == EQueueRole::COMPUTE; }));
        CHECK(std::ranges::any_of(batches, [](const auto& batch) { return !batch.dependencies.empty(); }));
    }
    JobGpuResources instance_gpu(session, fif);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
    CHECK(instance_job.initialize());
    FrameHarness frames(session, fif);
    const std::array instances{&instance_job};
    for (const std::uint8_t selected_input : {1, 0, 1, 0})
    {
        CHECK(instance_job.setConditions({&selected_input, 1}));
        auto submitted = take(runTask(frames.render(instances, {})));
        submitted += finishSubmissions(session, frames);
        std::printf("choice FIF=%u input=%u distinct_queues=%u batches=%zu native_submits=%u\n",
            fif, unsigned(selected_input), unsigned(distinct), batches.size(), submitted);
        CHECK(submitted == batches.size());
        CHECK(session.drain());
        const auto slot = instance_job.progress.last_submitted_slot;
        const auto values = readBufferWords<4>(session, *instance_job.resourceInput(result, slot).buffer);
        const auto expected = selected_input ? 7u : 8u;
        CHECK(std::ranges::all_of(values, [expected](auto word) { return word == expected; }));
    }
    std::printf("choice FIF=%u readback=7,8,7,8\n", fif);
}

static void nativeChoices(Validation& validation, EGraphicsBackend backend, std::uint32_t fif)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    conditionalHistorySamples(*session, fif, false);
    conditionalHistorySamples(*session, fif, true);
    crossQueueChoice(*session, fif);
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
        "independent graphics work", TransferWork{{
            FillBuffer{{{graphics_result, 0}, GraphBufferRange{0, 8}}, 7},
            FillBuffer{{{graphics_result, 0}, GraphBufferRange{8, 8}}, 11}
        }},
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
    const auto submissionCount = [](const compiled::RenderGraph& graph) { return graph.submissions().size(); };
    const auto heavy = queueScheduleFixture(backend, 80'000, false);
    const auto serial = queueScheduleFixture(backend, 80'000, true);
    const auto light = queueScheduleFixture(backend, 500, false);
    const auto unknown = queueScheduleFixture(backend, 0, false);
    CHECK(submissionCount(compileCpuGraph(heavy.graph)) == 1);
    CHECK(submissionCount(compileCpuGraph(serial.graph, calibrated)) == 1);
    CHECK(submissionCount(compileCpuGraph(light.graph, calibrated)) == 1);
    CHECK(submissionCount(compileCpuGraph(unknown.graph, calibrated)) == 1);
    CHECK(submissionCount(compileCpuGraph(heavy.graph, calibrated, true)) == 1);
    CHECK(submissionCount(compileCpuGraph(heavy.graph, {{1'000, 1'000, 100'000}})) == 1);

    const auto scheduled = compileCpuGraph(heavy.graph, calibrated);
    const auto batches = scheduled.submissions();
    CHECK(batches.size() == 3);
    CHECK(batches[0].queue == EQueueRole::GRAPHICS && batches[1].queue == EQueueRole::COMPUTE);
    CHECK(batches[2].queue == EQueueRole::GRAPHICS);
    CHECK(batches[1].dependencies.size() == 1 && batches[1].dependencies.front().producer == 0);
    CHECK(batches[2].dependencies.size() == 1 && batches[2].dependencies.front().producer == 0);
}

static void localFrozenInputs(DeviceSession& session, EGraphicsBackend backend, std::uint32_t static_count)
{
    RenderGraphBuilder builder;
    for (std::uint32_t i = 0; i < static_count; ++i)
    {
        const auto key = "static image " + std::to_string(i);
        const auto image = take(builder.declareResource({key, GraphImageDesc{4, 4}}));
        const auto clear = take(builder.addTransferPass(key, TransferWork{{ClearImage{{{image, 0}}}}}));
        CHECK(builder.exportOutput({clear.written.front()}));
    }
    const auto history = take(builder.declareHistory("counter", {GraphBufferDesc{16}}));
    ComputeWork increment;
    increment.interface.program = take(builder.addProgram(program(kGraphHistory, backend)));
    increment.interface.resources = {
        {"previous_sample", {{history.previous, 0}}}, {"current_sample", {{history.current, 0}, {}, EGraphAccess::WRITE}}
    };
    const auto updated = take(builder.addComputePass("increment", std::move(increment)));
    CHECK(builder.publishHistory(updated.written.front()));
    const auto output = take(builder.declareResource({"read counter", GraphBufferDesc{16}}));
    const auto copied = take(builder.addTransferPass("copy counter", TransferWork{{
        CopyBuffer{{updated.written.front()}, {{output, 0}}}
    }}));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto color = take(builder.declareResource({"stream color", GraphImageDesc{4, 4}}));
    RasterWork stream;
    stream.program = RasterPassTemplate{"local freeze probe"};
    stream.colors = {{{{color, 0}}}};
    const auto drawn = take(builder.addRasterPass("stream", std::move(stream)));
    CHECK(builder.exportOutput({drawn.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto plan = compile(session, graph, take(analyzeRenderGraph(graph)), nullptr);
    JobGpuResources instance_gpu(session, 1);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
    CHECK(instance_job.initialize());
    FrameHarness frames(session, 1);
    const std::array instances{&instance_job};
    const auto submit = [&]
    {
        CHECK(take(runTask(frames.render(instances, {}))) > 0);
        finishSubmissions(session, frames);
        CHECK(session.drain());
    };
    using Counts = FrozenPatchCounts;
    CHECK(take(instance_job.freezeFrame(0)));
    CHECK((instance_job.frame(0).last_patches == Counts{plan.graph.resources().size(), plan.graph.passes().size()}));
    auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
    RecordTextureQueries::original = core.GetTextureDesc;
    RecordTextureQueries::count = 0;
    core.GetTextureDesc = RecordTextureQueries::get;
    CHECK(take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_patches == Counts{});
    instance_job.viewport = {0, 0, 0.5f, 1};
    CHECK((take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_patches == Counts{0, 1}));
    const auto stream_slot = plan.diagnostics.passSlot(drawn.pass);
    rasterDraws(instance_job, plan.graph.passes()[stream_slot], 0).parameters.resize(4);
    CHECK((take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_patches == Counts{0, 1}));
    core.GetTextureDesc = RecordTextureQueries::original;
    CHECK(RecordTextureQueries::count == 0);
    submit();
    CHECK(readBufferWords<4>(session, *instance_job.resourceInput(output, 0).buffer)[0] == 1);
    CHECK(take(instance_job.freezeFrame(0)));
    const auto rotation = instance_job.frame(0).last_patches;
    CHECK((rotation == Counts{2, 2}));
    const auto original = instance_job.resourceInput(output, 0);
    auto short_view = original;
    short_view.size = 4;
    instance_job.bindResource(output, 0, short_view);
    CHECK(!instance_job.freezeFrame(0) && !recordFrame(instance_job, session, 0, 0));
    instance_job.bindResource(output, 0, original);
    CHECK((take(instance_job.freezeFrame(0)) && instance_job.frame(0).last_patches == Counts{1, 1}));
    submit();
    CHECK(readBufferWords<4>(session, *instance_job.resourceInput(output, 0).buffer)[0] == 2);
    if (static_count == 4)
    {
        JobGpuResources isolated_gpu(session, 2);
        JobExecution isolated_job{isolated_gpu, plan.graph, testInstallation(), TargetOutput{}};
        CHECK(isolated_job.initialize());
        CHECK(take(isolated_job.freezeFrame(1)));
        isolated_job.viewport = {0, 0, 0.5f, 1};
        CHECK(take(isolated_job.freezeFrame(0)));
        static decltype(core.CmdSetViewports) set_viewports = nullptr;
        static float width = 0;
        set_viewports = core.CmdSetViewports;
        core.CmdSetViewports = [](nri::CommandBuffer& cb, const nri::Viewport* values, std::uint32_t count) noexcept
        {
            if (count == 1) width = values[0].width;
            set_viewports(cb, values, count);
        };
        const auto batch = plan.graph.passes()[stream_slot].submission;
        width = 0;
        CHECK(recordFrame(isolated_job, session, 0, batch) && width == 2);
        width = 0;
        CHECK(recordFrame(isolated_job, session, 1, batch) && width == 4);
        core.CmdSetViewports = set_viewports;
    }
    std::printf(
        "LOCAL FREEZE PASS: static=%u stable=0/0 viewport=0/1 stream=0/1 history=%zu/%zu retry=1/1 "
        "stable texture queries=0 GPU counter=2\n", static_count, rotation.resources, rotation.passes
    );
}

static void sharedTextureRecording(DeviceSession& session, EGraphicsBackend backend)
{
    TargetDesc target;
    target.width = 7;
    target.height = 5;
    target.sampled = true;
    target.format = rdesc::ETextureFormat::RGBA8_UNORM;
    auto physical = take(OffscreenTarget::create(session, target, 1));
    const auto& image = physical.frame(0);
    auto other_target = target;
    other_target.width = 3;
    other_target.height = 2;
    auto other = take(OffscreenTarget::create(session, other_target, 1));
    const auto& second_image = other.frame(0);
    CHECK(submitFixtureCommands(session, session.queue(),
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            nri::TextureBarrierDesc transition{
                image.image, {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE},
                {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER},
                0, 1, 0, 1, nri::PlaneBits::COLOR
            };
            session.nriCore().CmdBarrier(command, {nullptr, 0, nullptr, 0, &transition, 1});
            transition.texture = second_image.image;
            session.nriCore().CmdBarrier(command, {nullptr, 0, nullptr, 0, &transition, 1});
            return {};
        }
    ));
    CHECK(session.drain());
    RenderGraphBuilder builder;
    const auto images = take(builder.declareResource({
        "images", GraphImageDesc{target.width, target.height}, EGraphOrigin::IMPORTED, {{}}, EGraphUse::SAMPLED
    }));
    const auto second_images = take(builder.declareResource({
        "second images", GraphImageDesc{3, 2}, EGraphOrigin::IMPORTED, {{}}, EGraphUse::SAMPLED
    }));
    const auto second_color = take(builder.declareResource({"second dimensions", GraphImageDesc{4, 4}}));
    const auto second_output = take(builder.declareResource({"second readback", GraphBufferDesc{1024}}));
    const auto color = take(builder.declareResource({"dimensions", GraphImageDesc{4, 4}}));
    const auto output = take(builder.declareResource({"readback", GraphBufferDesc{1024}}));
    RasterWork work;
    work.fixed().interface.program = take(builder.addProgram(rasterProgram(kGraphTableSize, backend)));
    work.fixed().interface.resources = {{"images", {{images, 0}}}};
    work.fixed().interface.parameters.setInlineConstant(IndirectCountConstants{image.sampled_index});
    work.colors = {{{{color, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE}};
    const auto drawn = take(builder.addRasterPass("shared image dimensions", work));
    work.fixed().interface.resources = {{"images", {{second_images, 0}}}};
    work.fixed().interface.parameters.setInlineConstant(IndirectCountConstants{second_image.sampled_index});
    work.colors.front().access.version = {second_color, 0};
    const auto second_draw = take(builder.addRasterPass("same table, different inline index", std::move(work)));
    CHECK(builder.dependsOn(drawn.pass, second_draw.pass));
    const auto copied = take(builder.addTransferPass(
        "read dimensions", TransferWork{{
            CopyImageBuffer{{drawn.written.front()}, {{output, 0}}, 256, 1024},
            CopyImageBuffer{{second_draw.written.front()}, {{second_output, 0}}, 256, 1024}
        }}
    ));
    exportRows(builder, written(copied, output), 4, 4, 256);
    exportRows(builder, written(copied, second_output), 4, 4, 256);
    const auto graph = take(std::move(builder).finish());
    const auto plan = compile(session, graph, take(analyzeRenderGraph(graph)), nullptr);
    JobGpuResources instance_gpu(session, 1);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
    const auto& first = plan.graph.passes()[plan.diagnostics.passSlot(drawn.pass)].fixed_bindings;
    const auto& second = plan.graph.passes()[plan.diagnostics.passSlot(second_draw.pass)].fixed_bindings;
    CHECK(first.layout && first.pipeline && first.shared_sets && first.constants);
    CHECK(!second.layout && !second.pipeline && !second.shared_sets && second.constants);
    instance_job.bindImage(images, 0, *image.image);
    instance_job.bindImage(second_images, 0, *second_image.image);
    CHECK(instance_job.initialize());
    FrameHarness frames(session, 1);
    const std::array instances{&instance_job};
    const auto check_dimensions = [&]
    {
        CHECK(take(runTask(frames.render(instances, {}))) > 0);
        finishSubmissions(session, frames);
        CHECK(session.drain());
        checkPixels(session, *instance_job.resourceInput(output, 0).buffer, 4, 0, {7, 5, 0, 255}, 256);
        checkPixels(session, *instance_job.resourceInput(second_output, 0).buffer, 4, 0, {3, 2, 0, 255}, 256);
    };
    {
        NativeBindingProbe probe(const_cast<nri::CoreInterface&>(session.nriCore()));
        check_dimensions();
        CHECK(probe.dispatches == 0 && probe.since_dispatch.sets == 1 && probe.since_dispatch.constants == 2);
    }
    // The imported graph ID declares table access. The actual recorded operand is the shared Set.
    const auto image_input = instance_job.resourceInput(images, 0);
    instance_job.bindResource(images, 0, {});
    instance_job.bindResource(images, 0, image_input);
    CHECK(take(instance_job.freezeFrame(0)));
    auto& core = const_cast<nri::CoreInterface&>(session.nriCore());
    EndFailure::original = core.EndCommandBuffer;
    EndFailure::calls = 0;
    core.EndCommandBuffer = &EndFailure::end;
    CHECK(recordFrame(instance_job, session, 0, 0) && EndFailure::calls == 0);
    core.EndCommandBuffer = EndFailure::original;
    auto& shared = session.descriptors();
    constexpr auto type = nri::DescriptorType::TEXTURE;
    const auto old_generation = shared.nativeSetGeneration(type);
    std::vector<DescriptorTable::Slot> extra;
    while (old_generation == shared.nativeSetGeneration(type)) extra.push_back(take(shared.allocate(type)));
    CHECK(take(instance_job.freezeFrame(0)));
    EndFailure::calls = 0;
    core.EndCommandBuffer = &EndFailure::end;
    CHECK(recordFrame(instance_job, session, 0, 0) && EndFailure::calls == 1);
    CHECK(recordFrame(instance_job, session, 0, 0) && EndFailure::calls == 1);
    core.EndCommandBuffer = EndFailure::original;
    check_dimensions();
    for (auto slot : extra) shared.release(type, slot);
    std::puts("SHARED CONSUMER PASS: one shared set, two root writes, one End on table replacement; GPU read 7x5 and 3x2");
}

static void nativeQueueCandidates(Validation& validation, EGraphicsBackend backend, bool grow_tables = false)
{
    DeviceOptions options;
    options.backend = backend;
    options.multiple_queues = true;
    options.texture_descriptors = grow_tables;
    options.descriptor_initial_counts = {1, 1, 1};
    options.descriptor_layout_limits = {32, 16, 16};
    options.graph_queue_costs = {1'000, 1'000, 1'000}; // Deterministic scheduling input for the fixture.
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    FrameHarness frames(*session, 1);
    const auto fixture = queueScheduleFixture(backend, 80'000, false);
    const auto analysis = take(analyzeRenderGraph(fixture.graph));
    const auto plan = compile(*session, fixture.graph, analysis, nullptr, ETransientAllocation::ALIAS_MEMORY, false);
    const auto bounds = plan.graph.submissions();
    const bool separate = &session->queue() != &session->queue(EQueueRole::COMPUTE);
    CHECK(bounds.size() == (separate ? 3 : 1));
    if (separate)
    {
        CHECK(bounds[1].queue == EQueueRole::COMPUTE);
    }
    JobGpuResources replacement_gpu(*session, 1);
    JobExecution replacement_job{replacement_gpu, plan.graph, testInstallation(), TargetOutput{}};
    CHECK(replacement_job.initialize());
    JobGpuResources instance_gpu(*session, 1);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), TargetOutput{}};
    CHECK(instance_job.initialize());
    const std::array instances{&instance_job};
    CHECK(take(runTask(frames.render(instances, {}))) > 0);
    finishSubmissions(*session, frames);
    CHECK(session->drain());
    const auto computed = readBufferWords<4>(*session, *instance_job.resourceInput(fixture.compute_result, 0).buffer);
    const auto graphics = readBufferWords<4>(*session, *instance_job.resourceInput(fixture.graphics_result, 0).buffer);
    CHECK(computed[0] == 4 && graphics[0] == 7 && graphics[2] == 11);

    const auto original = instance_job.resourceInput(fixture.graphics_result, 0);
    const auto other = replacement_job.resourceInput(fixture.graphics_result, 0);
    CHECK(original.buffer != other.buffer);
    instance_job.bindResource(fixture.graphics_result, 0, other);
    CHECK(take(instance_job.freezeFrame(0)));
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    EndFailure::original = core.EndCommandBuffer;
    EndFailure::calls = 0;
    core.EndCommandBuffer = &EndFailure::end;
    const auto changed = plan.diagnostics.passSlot({static_cast<std::uint32_t>(fixture.graph.passes().size() - 1)});
    const auto expected_batch = plan.graph.passes()[changed].submission;
    for (std::uint32_t batch = 0; batch < bounds.size(); ++batch)
    {
        const auto before = EndFailure::calls;
        CHECK(recordFrame(instance_job, *session, 0, batch));
        CHECK(EndFailure::calls - before == (batch == expected_batch ? 1u : 0u));
        CHECK(recordFrame(instance_job, *session, 0, batch) && EndFailure::calls - before == (batch == expected_batch ? 1u : 0u));
    }
    core.EndCommandBuffer = EndFailure::original;
    CHECK(take(runTask(frames.render(instances, {}))) > 0);
    finishSubmissions(*session, frames);
    CHECK(session->drain());
    const auto rebound = readBufferWords<4>(*session, *other.buffer);
    CHECK(rebound[0] == 7 && rebound[1] == 7 && rebound[2] == 11 && rebound[3] == 11);
    std::printf(
        "LOCAL REBIND PASS: %zu batches, two changed views, %u native End, new buffer [7,7,11,11]\n",
        bounds.size(), EndFailure::calls
    );
    if (grow_tables && session->enabledFeatures().variable_descriptor_count)
    {
        auto& shared = session->descriptors();
        constexpr auto kind = nri::DescriptorType::TEXTURE;
        const auto generation = shared.nativeSetGeneration(kind);
        std::vector<DescriptorTable::Slot> slots;
        while (shared.nativeSetGeneration(kind) == generation) slots.push_back(take(shared.allocate(kind)));
        CHECK(take(instance_job.freezeFrame(0)));
        EndFailure::calls = 0;
        core.EndCommandBuffer = &EndFailure::end;
        for (std::uint32_t batch = 0; batch < bounds.size(); ++batch) CHECK(recordFrame(instance_job, *session, 0, batch));
        core.EndCommandBuffer = EndFailure::original;
        // D3D12 replaced the private pool bound by every CB. These shaders never bind Vulkan's texture table.
        const auto expected = backend == EGraphicsBackend::D3D12 ? bounds.size() : 0;
        CHECK(EndFailure::calls == expected);
        std::printf("POOL REPLACEMENT PASS: %u native End across %zu batches\n", EndFailure::calls, bounds.size());
        for (auto slot : slots) shared.release(kind, slot);
        sharedTextureRecording(*session, backend);
    }
    instance_job.bindResource(fixture.graphics_result, 0, original);
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
    CHECK(std::holds_alternative<RasterCommandSlot>(pass.commands) && recording.raster.size() == 1);
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
    const auto history = take(builder.declareHistory("samples", {GraphBufferDesc{16}, {}, EGraphUse::STORAGE_READ, distance}));
    const auto enabled = take(builder.declareCondition("update"));
    const auto independent = take(builder.declareHistory("independent", {GraphBufferDesc{16}}));
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
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    const std::array instances{instance_job.get()};
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
    CHECK(!plan.graph.physicalResources().resource_slots[history.current.index].valid());
    const std::uint8_t enabled_initial = 1;
    CHECK(instance_job->setConditions({&enabled_initial, 1}));
    CHECK(take(instance_job->freezeFrame(0)));
    const auto rejected = submitFixtureCommands(*session, session->queue(),
        [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void>
        { return lux::cxx::unexpected(RenderError{kDeviceArgument}); }
    );
    CHECK(!rejected && instance_job->progress.count(EExecutionOutcome::EXECUTED) == 0);
    const GraphResourceId invalid_id{UINT32_MAX};
    CHECK(!instance_job->invalidateHistory({&invalid_id, 1}));
    for (std::uint32_t execution = 0; execution < 13; ++execution)
    {
        const bool invalidate = execution == 8;
        if (invalidate)
        {
            CHECK(instance_job->invalidateHistory({}));
            published.clear();
        }
        const std::uint8_t enabled_value = execution != 3 && execution != 4 && execution != 10;
        CHECK(instance_job->setConditions({&enabled_value, 1}));
        const auto previous = newestInput();
        if (enabled_value)
        {
            published.push_back(evaluate(previous));
        }
        const auto submitted = take(runTask(frames->render(instances, {})));
        CHECK(submitted == 1);
        const auto slot = instance_job->progress.last_submitted_slot;
        CHECK(session->drain());
        const auto values = readBufferWords<13>(*session, *instance_job->resourceInput(output, slot).buffer);
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
    CHECK(instance_job->setConditions({&enabled_value, 1}));
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
        readBufferWords(*session, *instance_job->resourceInput(output, instance_job->progress.last_submitted_slot).buffer);
    CHECK(values[0] == published.back());
    if (fif == 1 && use_fallback && distance == 1)
    {
        JobGpuResources other_gpu(*session, fif);
        JobExecution other{other_gpu, plan.graph, testInstallation(), {}};
        CHECK(other.initialize() && &other.graph() == &instance_job->graph());
        CHECK(other.setConditions({&enabled_value, 1}));
        const std::array pair{instance_job.get(), &other};
        CHECK(take(runTask(frames->render(pair, {}))) > 0);
        finishSubmissions(*session, *frames);
        CHECK(session->drain());
        const auto mature = readBufferWords<13>(*session, *instance_job->resourceInput(output, 0).buffer);
        const auto fresh = readBufferWords<13>(*session, *other.resourceInput(output, 0).buffer);
        CHECK(mature[0] == evaluate(newestInput()) && mature[8] == 1);
        CHECK(fresh[0] == 18 && fresh[4] == 29 && fresh[8] == 0 && fresh[9] == 1);
        std::puts("JOB HISTORY PASS: one device graph, mature Job advanced; new Job read its own first-frame fallbacks");
    }
    instance_job.reset();
    instance_gpu.reset();
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
    const auto history = take(builder.declareHistory("hzb", {image, {{1, 1, 1, 1}}, EGraphUse::SAMPLED}));
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
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), physical->output()
    );
    CHECK(instance_job->initialize());
    for (std::uint32_t execution = 0; execution < 6; ++execution)
    {
        if (execution == 2)
        {
            // Retire an in-flight old instance while preparing a larger candidate from the same immutable plan.
            const std::array old{instance_job.get()};
            CHECK(take(runTask(frames->render(old, {}))) > 0 && !frames->submitting());
            target.width = 17;
            target.height = 11;
            auto replacement = std::optional{take(OffscreenTarget::create(*session, target, fif))};
            auto candidate_gpu = std::make_unique<JobGpuResources>(*session, fif);
            auto candidate_job = std::make_unique<JobExecution>(
                *candidate_gpu, plan.graph, testInstallation(), replacement->output()
            );
            CHECK(candidate_job->initialize());
            instance_job = std::move(candidate_job);
            instance_gpu = std::move(candidate_gpu);
            physical.emplace(std::move(*replacement));
            CHECK(session->drain());
        }
        if (execution == 4)
        {
            const auto& bank = instance_gpu->histories.front();
            const auto old_image = bank.slots.front().image;
            target.width = 9;
            target.height = 7;
            auto replacement = take(OffscreenTarget::create(*session, target, fif));
            auto changed = take(ResizeResources::prepare(
                *instance_job, replacement.output()
            ));
            CHECK(changed.prepareAdoption(*instance_job));
            std::move(changed).commit(*instance_job);
            CHECK(bank.slots.front().image != old_image);
            for (const auto& slot : bank.slots) CHECK(slot.last_use.belongsTo(&*session, session->queueCount()));
            physical.emplace(std::move(replacement));
        }
        const std::array instances{instance_job.get()};
        CHECK(take(runTask(frames->render(instances, {}))) > 0 && !frames->submitting());
        CHECK(session->drain());
        const auto values = readBufferWords<128>(
            *session,
            *instance_job->resourceInput(output, instance_job->progress.last_submitted_slot).buffer
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
    instance_job.reset();
    instance_gpu.reset();
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
        const GraphBufferRange range{value == 6 ? 0u : 8u, 8};
        current = take(builder.addTransferPass(
                           "conditional " + std::to_string(value),
                           TransferWork{{FillBuffer{{current, range, EGraphAccess::WRITE, 0, current}, value}}},
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
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    const std::array instances{instance_job.get()};
    for (const std::uint8_t enabled : {0, 1, 0, 1})
    {
        CHECK(instance_job->setConditions({&enabled, 1}));
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto values = readBufferWords<4>(
            *session,
            *instance_job->resourceInput(output, instance_job->progress.last_submitted_slot).buffer
        );
        const auto expected = enabled ? std::array{6u, 6u, 9u, 9u} : std::array{3u, 3u, 3u, 3u};
        CHECK(std::ranges::equal(values, expected));
    }
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
}

static void sharedImportedBuffer(Validation& validation, EGraphicsBackend backend, bool ordered, bool separate_ranges = false)
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
    const auto copied = take(builder.addTransferPass("copy result", std::move(copies)));
    CHECK(builder.exportOutput({copied.written.front()}));
    const auto graph = take(std::move(builder).finish());
    auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), nullptr);
    auto first_gpu = std::make_unique<JobGpuResources>(*session, 2);
    auto first_job = std::make_unique<JobExecution>(
        *first_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    auto second_gpu = std::make_unique<JobGpuResources>(*session, 2);
    auto second_job = std::make_unique<JobExecution>(
        *second_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    for (auto* job : {first_job.get(), second_job.get()})
    {
        const std::uint64_t offset = separate_ranges && job == second_job.get() ? 16 : 0;
        job->bindResource(input, UINT32_MAX, {shared, offset, 16});
        CHECK(job->initialize());
    }
    const std::array instances{first_job.get(), second_job.get()};
    const auto before = session->queue(EQueueRole::COMPUTE).submitted();
    const std::array explicit_order{SubmissionOrder{1, 0}};
    const auto selected_order = std::span(explicit_order).first(ordered ? 1 : 0);
    CHECK(take(runTask(frames->render(instances, {}, selected_order))) > 0);
    finishSubmissions(*session, *frames);
    const auto submitted = session->queue(EQueueRole::COMPUTE).submitted() - before;
    if (&session->queue() != &session->queue(EQueueRole::COMPUTE))
    {
        CHECK(submitted == (ordered ? 2 : 1));
    }
    CHECK(session->drain());
    const auto a =
        readBufferWords<4>(*session, *first_job->resourceInput(output, first_job->progress.last_submitted_slot).buffer);
    const auto b =
        readBufferWords<4>(*session, *second_job->resourceInput(output, second_job->progress.last_submitted_slot).buffer);
    CHECK(a[0] == 1 && b[0] == 1);
    std::printf("SHARED READ ORDER backend=%s explicit=%u separate_ranges=%u compute_submits=%llu results=1,1\n",
        backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan", unsigned(ordered), unsigned(separate_ranges),
        static_cast<unsigned long long>(submitted));
    first_job.reset();
    first_gpu.reset();
    second_job.reset();
    second_gpu.reset();
    CHECK(session->drain());
    core.DestroyBuffer(shared);
}

static void multiQueueHistory(Validation& validation, DeviceOptions options, std::uint32_t fif)
{
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto history = take(builder.declareHistory("samples", {GraphBufferDesc{16}}));
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
    CHECK(plan.graph.submissions().size() == (separate ? 7 : 1));
    if (separate)
    {
        const auto& copy_dependencies = plan.graph.submissions()[2].dependencies;
        const auto copy = std::ranges::find(copy_dependencies, 1u, &SubmissionDependency::producer);
        CHECK(copy != copy_dependencies.end() && copy->stages == nri::StageBits::COPY);
        const auto& compute_dependencies = plan.graph.submissions()[3].dependencies;
        const auto compute = std::ranges::find(compute_dependencies, 2u, &SubmissionDependency::producer);
        // The latest Graphics producer also covers the shared preparation edge (ALL).
        CHECK(compute != compute_dependencies.end() && compute->stages == nri::StageBits::ALL);
        CHECK(
            std::ranges::find(compute_dependencies, 0u, &SubmissionDependency::producer) == compute_dependencies.end()
        );
    }

    auto first_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto first_job = std::make_unique<JobExecution>(
        *first_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    auto second_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto second_job = std::make_unique<JobExecution>(
        *second_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(first_job->initialize() && second_job->initialize());
    const std::array instances{first_job.get(), second_job.get()};
    for (unsigned frame = 0; frame < 2; ++frame)
    {
        const auto before_graphics = session->queue().submitted();
        const auto before_compute = session->queue(EQueueRole::COMPUTE).submitted();
        CHECK(take(runTask(frames->render(instances, {}))) != 0);
        if (separate)
        {
            // Every graph batch owns a FIF command buffer: seven batches must no longer
            // stop at the temporary pool's three slots.
            CHECK(!frames->submitting());
            CHECK(first_job->progress.count(EExecutionOutcome::EXECUTED) == frame + 1);
            if (frame == 0)
            {
                CHECK(first_job->invalidateHistory({}));
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
            readBufferWords<4>(*session, *first_job->resourceInput(output, first_job->progress.last_submitted_slot).buffer);
        const auto b =
            readBufferWords<4>(*session, *second_job->resourceInput(output, second_job->progress.last_submitted_slot).buffer);
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
        CHECK(first_job->progress.count(EExecutionOutcome::EXECUTED) == 2);
        CHECK(!frames->submitting() && !first_job->freezeFrame(0));
    }
    first_job.reset();
    first_gpu.reset();
    second_job.reset();
    second_gpu.reset();
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

enum class EAllocationFault { SECOND_ALLOCATION, PARTIAL_TEXTURE_BIND };

class PhysicalAllocationFault final
{
public:
    PhysicalAllocationFault(DeviceSession& session, EAllocationFault fault)
        : core_(const_cast<nri::CoreInterface&>(session.nriCore())), original_(core_), fault_(fault)
    {
        current_ = this;
        core_.AllocateMemory = allocate;
        core_.FreeMemory = release;
        core_.BindTextureMemory = bindTextures;
    }

    ~PhysicalAllocationFault()
    {
        core_.BindTextureMemory = original_.BindTextureMemory;
        core_.FreeMemory = original_.FreeMemory;
        core_.AllocateMemory = original_.AllocateMemory;
        current_ = nullptr;
        CHECK(injected_ && allocated_ > 0 && outstanding_.empty());
        std::printf("Physical allocation rollback: %s, released %u candidate allocations\n",
            fault_ == EAllocationFault::SECOND_ALLOCATION ? "second allocation" : "partial texture bind", allocated_
        );
    }

private:
    static nri::Result NRI_CALL allocate(nri::Device& device, const nri::AllocateMemoryDesc& desc, nri::Memory*& memory)
    {
        auto& self = *current_;
        if (self.fault_ == EAllocationFault::SECOND_ALLOCATION && self.allocated_ == 1)
        {
            self.injected_ = true;
            return nri::Result::OUT_OF_MEMORY;
        }
        const auto result = self.original_.AllocateMemory(device, desc, memory);
        if (result == nri::Result::SUCCESS)
        {
            ++self.allocated_;
            self.outstanding_.push_back(memory);
        }
        return result;
    }

    static void NRI_CALL release(nri::Memory* memory)
    {
        auto& self = *current_;
        const auto removed = std::erase(self.outstanding_, memory);
        CHECK(removed == 1);
        self.original_.FreeMemory(memory);
    }

    static nri::Result NRI_CALL bindTextures(const nri::BindTextureMemoryDesc* bindings, std::uint32_t count)
    {
        auto& self = *current_;
        CHECK(count > 1 && self.fault_ == EAllocationFault::PARTIAL_TEXTURE_BIND);
        CHECK(self.original_.BindTextureMemory(bindings, 1) == nri::Result::SUCCESS);
        self.injected_ = true;
        return nri::Result::OUT_OF_MEMORY;
    }

    inline static PhysicalAllocationFault* current_ = nullptr;
    nri::CoreInterface& core_;
    nri::CoreInterface original_;
    EAllocationFault fault_;
    std::vector<nri::Memory*> outstanding_;
    std::uint32_t allocated_ = 0;
    bool injected_ = false;
};

struct DedicatedResizeRequirement final
{
    inline static decltype(nri::CoreInterface::GetTextureMemoryDesc) original = nullptr;
    inline static decltype(nri::CoreInterface::GetTextureDesc) describe = nullptr;
    inline static unsigned selected = 0, closure_objects = 0;
    inline static const nri::Texture* live_fixed = nullptr;
    inline static bool require_fixed_dedicated = false;

    static void NRI_CALL query(const nri::Texture& image, nri::MemoryLocation location, nri::MemoryDesc& memory)
    {
        original(image, location, memory);
        if (describe(image).width == 64)
        {
            ++selected;
            memory.mustBeDedicated = true;
        }
        else if (describe(image).width == 16 && (require_fixed_dedicated || &image != live_fixed))
        {
            ++closure_objects;
            memory.mustBeDedicated = true;
        }
    }
};

static void layeredAliasHandoff(Validation& validation, EGraphicsBackend backend)
{
    DeviceOptions options;
    options.backend = backend;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    RenderGraphBuilder builder;
    constexpr std::array widths{16u, 32u};
    constexpr std::array ranges{GraphImageRange{1, 0, 1, 0, 1}, GraphImageRange{1, 1, 1, 1, 1}};
    constexpr std::array colors{
        std::array{1.f, 0.f, 0.f, 1.f}, std::array{0.f, 1.f, 0.f, 1.f},
        std::array{0.f, 0.f, 1.f, 1.f}, std::array{1.f, 1.f, 1.f, 1.f}
    };
    std::array<GraphResourceId, 2> images;
    std::array<GraphResourceId, 4> buffers;
    std::array<GraphPassId, 2> last_reads;
    for (unsigned i = 0; i < widths.size(); ++i)
    {
        GraphImageDesc desc{widths[i], widths[i], 2, 2};
        if (i) desc.extent.source = EImageExtentSource::TARGET;
        images[i] = take(builder.declareResource({"layers" + std::to_string(i), desc}));
        ResourceVersion version{images[i], 0};
        const auto preceding = last_reads;
        for (unsigned part = 0; part < ranges.size(); ++part)
        {
            const auto index = 2 * i + part;
            const auto extent = widths[i] >> part;
            const auto filled = take(builder.addTransferPass(
                "clear subresource" + std::to_string(index), TransferWork{{ClearImage{{version, ranges[part]}, colors[index]}}}
            ));
            version = filled.written.front();
            if (i)
            {
                for (const auto prior : preceding) CHECK(builder.dependsOn(prior, filled.pass));
            }
            buffers[index] = take(builder.declareResource({"readback" + std::to_string(index), GraphBufferDesc{extent * 256}}));
            const auto copy = take(builder.addTransferPass(
                "observe subresource" + std::to_string(index),
                TransferWork{{CopyImageBuffer{
                    {version, ranges[part]}, {{buffers[index], 0}}, 256, extent * 256, ImageCopyRegion{0, 0, extent, extent}
                }}}
            ));
            last_reads[part] = copy.pass;
            exportRows(builder, copy.written.front(), extent, extent, 256);
        }
    }

    const auto graph = take(std::move(builder).finish());
    const TargetDesc target{32, 32, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    const auto plan = compile(*session, graph, take(analyzeRenderGraph(graph)), &target);
    const auto& physical = plan.graph.physicalResources();
    const auto write = nri::StageBits::COLOR_ATTACHMENT;
    const auto all_uses = write | nri::StageBits::COPY;
    const auto entry = backend == EGraphicsBackend::D3D12 ? all_uses : write;
    for (const auto resource : images)
    {
        const auto& frontier = physical.alias_frontiers[physical.resource_slots[resource.index].index];
        const auto& queue = frontier.queues[0];
        CHECK(queue.point_count == 4 && queue.source.stages == all_uses && queue.destination.stages == entry);
        const auto points = std::span{physical.alias_points}.subspan(queue.first_point, queue.point_count);
        for (const auto range : ranges)
        {
            CHECK(std::ranges::count_if(points, [&](const auto& point)
            {
                return std::get<GraphImageRange>(point.range) == range && point.writes && point.needs_entry;
            }) == 1);
            CHECK(std::ranges::count_if(points, [&](const auto& point)
            {
                return std::get<GraphImageRange>(point.range) == range && !point.writes && !point.needs_entry;
            }) == 1);
        }
    }
    auto original = take(OffscreenTarget::create(*session, target, 1));
    auto large_desc = target;
    large_desc.width = large_desc.height = 64;
    auto larger = take(OffscreenTarget::create(*session, large_desc, 1));
    FrameHarness frames(*session, 1);
    for (const auto fault : {EAllocationFault::SECOND_ALLOCATION, EAllocationFault::PARTIAL_TEXTURE_BIND})
    {
        JobGpuResources rejected_gpu(*session, 1);
        JobExecution rejected{rejected_gpu, plan.graph, testInstallation(), original.output()};
        PhysicalAllocationFault injection(*session, fault);
        const auto result = rejected.initialize();
        CHECK(!result && isNativeFailure(result.error(), nri::Result::OUT_OF_MEMORY));
        const auto& native = rejected_gpu.frames.front();
        CHECK(native.buffers.empty() && native.images.empty() && native.memories.empty());
    }
    JobGpuResources instance_gpu(*session, 1);
    JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), original.output()};
    CHECK(instance_job.initialize());
    const auto check = [&](bool split)
    {
        std::string dump;
        CHECK(instance_job.dumpResources({&dump, [](void* p, std::string_view part) noexcept
        {
            static_cast<std::string*>(p)->append(part);
            return true;
        }}));
        if (split)
        {
            CHECK(dump.find("\"explicit_memory_count\":6") != std::string::npos);
            CHECK(dump.find("\"alias_barriers\":[]") != std::string::npos);
            CHECK(dump.find("\"reason\":\"dedicated\"") != std::string::npos);
        }
        else
        {
            CHECK(dump.find("\"explicit_memory_count\":5") != std::string::npos);
            CHECK(dump.find("\"before_stages\":" + std::to_string(static_cast<unsigned>(all_uses))) != std::string::npos);
            CHECK(dump.find("\"after_stages\":" + std::to_string(static_cast<unsigned>(entry))) != std::string::npos);
        }
        const std::array instances{&instance_job};
        CHECK(take(runTask(frames.render(instances, {}))) == 1);
        CHECK(session->drain());
        for (unsigned i = 0; i < buffers.size(); ++i)
        {
            std::array<unsigned, 4> expected;
            std::ranges::transform(colors[i], expected.begin(), [](float v) { return unsigned(v * 255); });
            checkPixels(*session, *instance_job.resourceInput(buffers[i], 0).buffer, widths[i / 2] >> (i % 2), 0, expected, 256);
        }
        std::printf("Alias mip/layer readback passed. %s\n", dump.c_str());
    };
    check(false);
    auto& core = const_cast<nri::CoreInterface&>(session->nriCore());
    {
        PhysicalAllocationFault injection(*session, EAllocationFault::PARTIAL_TEXTURE_BIND);
        const auto rejected = ResizeResources::prepare(instance_job, larger.output());
        CHECK(!rejected && isNativeFailure(rejected.error(), nri::Result::OUT_OF_MEMORY));
    }
    check(false); // Same native outputs still render after the partially bound candidate was destroyed.
    // The target extent stays fixed while the native group contract first splits and then merges.
    // Both old/new group members must migrate, including the member whose own requirement is unchanged.
    DedicatedResizeRequirement::describe = core.GetTextureDesc;
    DedicatedResizeRequirement::original = core.GetTextureMemoryDesc;
    {
        DedicatedResizeRequirement::require_fixed_dedicated = true;
        core.GetTextureMemoryDesc = &DedicatedResizeRequirement::query;
        auto regroup = ResizeResources::prepare(instance_job, original.output());
        core.GetTextureMemoryDesc = DedicatedResizeRequirement::original;
        DedicatedResizeRequirement::require_fixed_dedicated = false;
        CHECK(regroup && regroup->prepareAdoption(instance_job));
        std::move(*regroup).commit(instance_job);
    }
    check(true);
    {
        auto restore = ResizeResources::prepare(instance_job, original.output());
        CHECK(restore && restore->prepareAdoption(instance_job));
        std::move(*restore).commit(instance_job);
    }
    check(false);
    DedicatedResizeRequirement::live_fixed = instance_job.resourceInput(images[0], 0).image;
    DedicatedResizeRequirement::closure_objects = 0;
    DedicatedResizeRequirement::describe = core.GetTextureDesc;
    DedicatedResizeRequirement::original = std::exchange(core.GetTextureMemoryDesc, &DedicatedResizeRequirement::query);
    DedicatedResizeRequirement::selected = 0;
    auto split = ResizeResources::prepare(instance_job, larger.output());
    core.GetTextureMemoryDesc = DedicatedResizeRequirement::original;
    CHECK(split && DedicatedResizeRequirement::selected == 1 && DedicatedResizeRequirement::closure_objects == 1);
    const auto& placement = split->frames[0].changes.placements;
    CHECK(placement[physical.resource_slots[images[0].index].index].reason == EMemoryPlacement::DEDICATED);
    CHECK(placement[physical.resource_slots[images[1].index].index].reason == EMemoryPlacement::DEDICATED);
    CHECK(split->prepareAdoption(instance_job));
    std::move(*split).commit(instance_job);
    check(true);
    auto joined = take(ResizeResources::prepare(instance_job, original.output()));
    CHECK(joined.prepareAdoption(instance_job));
    std::move(joined).commit(instance_job);
    check(false);
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
        (plan.graph.physicalResources().resource_slots[a.index] == plan.graph.physicalResources().resource_slots[b.index]) ==
        !alias_memory
    );
    CHECK(plan.graph.physicalResources().slots.size() == (alias_memory ? 4 : 3));
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    if (alias_memory)
    {
        std::string dump;
        CHECK(instance_job->dumpResources(
            {&dump,
             [](void* p, std::string_view text) noexcept
             {
                 static_cast<std::string*>(p)->append(text);
                 return true;
             }}
        ));
        CHECK(dump.find("\"explicit_memory_count\":3") != std::string::npos);
    }
    const std::array instances{instance_job.get()};
    for (std::uint32_t i = 0; i < fif + 1; ++i)
    {
        CHECK(take(runTask(frames->render(instances, {}))) == 1);
        CHECK(session->drain());
        const auto slot = instance_job->progress.last_submitted_slot;
        checkPixels(*session, *instance_job->resourceInput(red, slot).buffer, 16, 0, {255, 0, 0, 255}, 256);
        checkPixels(*session, *instance_job->resourceInput(green, slot).buffer, extent, 0, {0, 255, 0, 255}, 256);
    }
    instance_job.reset();
    instance_gpu.reset();
    CHECK(session->drain());
}

static void crossQueueImageHandoff(DeviceSession& device, EGraphicsBackend backend, bool direct_wait)
{
    const GraphImageRange base_mip{}, shader_mip{1, 1, 1};
    RenderGraphBuilder builder;
    const auto shader = take(builder.addProgram(program(kGraphFill, backend)));
    std::array<GraphResourceId, 2> images;
    std::array<GraphResourceId, 4> readbacks;
    PassResult prior_compute{}, prior_copy{};
    const auto save = [&](ResourceVersion version, unsigned index)
    {
        auto& buffer = readbacks[index];
        buffer = take(builder.declareResource({"pixel" + std::to_string(index), GraphBufferDesc{256}}));
        const auto copy = take(builder.addTransferPass("save" + std::to_string(index), TransferWork{{CopyImageBuffer{
            {version, index % 2 ? shader_mip : base_mip}, {{buffer, 0}}, 256, 256, ImageCopyRegion{0, 0, 1, 1}
        }}}));
        CHECK(builder.exportOutput({copy.written.front(), GraphBufferRange{0, 4}, {}, EGraphUse::TRANSFER_DESTINATION}));
        return copy;
    };
    for (unsigned i = 0; i < images.size(); ++i)
    {
        const unsigned width = 16u << i;
        const auto suffix = std::to_string(i);
        images[i] = take(builder.declareResource({"alias" + suffix, GraphImageDesc{width, width, 2}}));
        const auto color = std::array{i ? 0.f : 1.f, i ? 1.f : 0.f, 0.f, 1.f};
        const auto clear = take(builder.addTransferPass(
            "activate" + suffix, TransferWork{{ClearImage{{{images[i], 0}, base_mip}, color}}}
        ));
        if (i)
        {
            if (direct_wait) CHECK(builder.dependsOn(prior_compute.pass, clear.pass));
            CHECK(builder.dependsOn(prior_copy.pass, clear.pass));
        }
        const auto observed = save(clear.written.front(), i * 2);
        ComputeWork work;
        work.allow_async_compute = true;
        work.interface.program = shader;
        work.interface.resources = {{"generated", {clear.written.front(), shader_mip, EGraphAccess::WRITE}}};
        work.dispatch = DispatchArguments{{width / 8, width / 8, 1}};
        prior_compute = take(builder.addComputePass("write" + suffix, std::move(work)));
        CHECK(builder.dependsOn(observed.pass, prior_compute.pass));
        prior_copy = save(prior_compute.written.front(), i * 2 + 1);
    }

    const auto graph = take(std::move(builder).finish());
    auto ready = compile(device, graph, take(analyzeRenderGraph(graph)), nullptr);
    const auto& physical = ready.graph.physicalResources();
    const auto a = physical.resource_slots[images[0].index].index;
    const auto b = physical.resource_slots[images[1].index].index;
    CHECK(a != b);
    FrameHarness frames(device, 1);
    JobGpuResources gpu(device, 1);
    JobExecution job{gpu, ready.graph, testInstallation(), TargetOutput{}};
    CHECK(job.initialize());
    const auto& source = physical.alias_frontiers[a];
    const auto& entry = physical.alias_frontiers[b].queues[0];
    const bool narrow = direct_wait && (backend != EGraphicsBackend::D3D12 || device.description().features.enhancedBarriers);
    CHECK(source.queues[1].point_count && entry.point_count);
    CHECK((entry.waited_through[1] != kGraphInvalidIndex) == narrow);
    nri::GlobalBarrierDesc expected{source.queues[0].source, entry.destination};
    if (narrow)
    {
        CHECK(entry.waited_through[1] >= source.queues[1].last_submission);
        expected.before.stages = combineStages(expected.before.stages, entry.destination.stages);
        CHECK(expected.before.stages != nri::StageBits::ALL && expected.after.stages != nri::StageBits::ALL);
    }
    else
    {
        expected.before.stages = expected.after.stages = nri::StageBits::ALL;
        expected.after.access = entry.source.access;
    }
    AliasHandoffProbe probe(device, expected, direct_wait ? nri::StageBits::ALL : nri::StageBits::COPY);
    probe.expectImage(*job.resourceInput(images[1], 0).image);
    const std::array jobs{&job};
    CHECK(take(runTask(frames.render(jobs, {}))) > 0);
    finishSubmissions(device, frames);
    CHECK(device.drain() && probe.observed());
    for (unsigned i = 0; i < readbacks.size(); ++i)
    {
        const auto word = readBufferWords<1>(device, *job.resourceInput(readbacks[i], 0).buffer)[0];
        if (i % 2) CHECK(word == 0xffbf8040u || word == 0xffbf7f40u);
        else CHECK(word == (i == 0 ? 0xff0000ffu : 0xff00ff00u));
    }
    std::printf("IMAGE ALIAS ENTRY direct_wait=%u narrow=%u pixels=red,shader,green,shader\n", direct_wait, narrow);
}

enum class EReuseOrder
{
    UNORDERED,
    EXPLICIT,
    SHADER_READER
};

struct PhysicalReuseCase final
{
    EReuseOrder order;
    bool alias_memory = false;
};

static void physicalReuse(Validation& validation, DeviceOptions options, std::uint32_t fif, PhysicalReuseCase scenario)
{
    const auto [order, alias_memory] = scenario;
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    auto frames = std::make_unique<FrameHarness>(*session, fif);
    RenderGraphBuilder builder;
    const auto a = take(builder.declareResource({"first temporary", GraphBufferDesc{16}}));
    const auto b = take(builder.declareResource({"second temporary", GraphBufferDesc{alias_memory ? 32u : 16u}}));
    const auto output_a = take(builder.declareResource({"first result", GraphBufferDesc{16}}));
    const auto output_b = take(builder.declareResource({"second result", GraphBufferDesc{16}}));
    const auto shader = take(builder.addProgram(program(kGraphHistory, options.backend)));
    const auto seed_a = take(builder.addTransferPass("seed first", TransferWork{{FillBuffer{{{a, 0}}, 3}}}));
    const auto increment = [&](std::string name, ResourceVersion input, GraphResourceId output, bool async = true)
    {
        ComputeWork work;
        work.allow_async_compute = async;
        work.interface.program = shader;
        work.interface.resources = {
            {"previous_sample", {input}},
            {"current_sample", {{output, 0}, {}, EGraphAccess::WRITE}}
        };
        return take(builder.addComputePass(std::move(name), std::move(work)));
    };
    const auto first = increment("observe first", seed_a.written.front(), output_a);
    std::optional<GraphResourceId> witness;
    if (order == EReuseOrder::SHADER_READER)
    {
        // This graphics batch waits for the old Compute use at COMPUTE_SHADER. Its later Fill
        // aliases that memory at a different stage. The alias barrier must bridge the narrow wait.
        witness = take(builder.declareResource({"graphics shader result", GraphBufferDesc{16}}));
        const auto reader = increment("read first on graphics", first.written.front(), *witness, false);
        CHECK(builder.exportOutput({reader.written.front(), {}, {}, EGraphUse::TRANSFER_DESTINATION}));
    }
    const auto seed_b = take(builder.addTransferPass("seed second", TransferWork{{FillBuffer{{{b, 0}}, 7}}}));
    if (order == EReuseOrder::EXPLICIT)
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
    CHECK(plan.graph.submissions().size() == control.graph.submissions().size());
    for (std::size_t i = 0; i < plan.graph.submissions().size(); ++i)
    {
        const auto& compiled = plan.graph.submissions()[i];
        const auto& independent = control.graph.submissions()[i];
        CHECK(compiled.queue == independent.queue && compiled.dependencies.size() == independent.dependencies.size());
        for (std::size_t d = 0; d < compiled.dependencies.size(); ++d)
        {
            CHECK(compiled.dependencies[d].producer == independent.dependencies[d].producer);
            CHECK(compiled.dependencies[d].stages == independent.dependencies[d].stages);
        }
    }
    const bool reuse = order != EReuseOrder::UNORDERED || &session->queue() == &session->queue(EQueueRole::COMPUTE);
    CHECK(
        (plan.graph.physicalResources().resource_slots[a.index] == plan.graph.physicalResources().resource_slots[b.index]) ==
        (reuse && !alias_memory)
    );
    CHECK(plan.diagnostics.resource_reuse[output_a.index] == EResourceReuse::EXPORTED);
    const auto independent_slots = witness ? 5u : 4u;
    CHECK(control.graph.physicalResources().slots.size() == independent_slots);
    CHECK(plan.graph.physicalResources().slots.size() == independent_slots - (reuse && !alias_memory));
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    std::optional<AliasHandoffProbe> handoff_probe;
    if (alias_memory)
    {
        std::string dump;
        CHECK(instance_job->dumpResources(
            {&dump,
             [](void* p, std::string_view text) noexcept
             {
                 static_cast<std::string*>(p)->append(text);
                 return true;
             }}
        ));
        CHECK(dump.find("\"explicit_memory_count\":" + std::to_string(independent_slots - reuse)) != std::string::npos);
        CHECK(dump.find(reuse ? "\"reason\":\"aliased\"" : "\"reason\":\"unordered\"") != std::string::npos);
        const auto& physical = plan.graph.physicalResources();
        const auto& source = physical.alias_frontiers[physical.resource_slots[a.index].index].queues;
        if (reuse && source[1].point_count)
        {
            const auto& target = physical.alias_frontiers[physical.resource_slots[b.index].index].queues[0];
            CHECK(target.destination.stages != nri::StageBits::ALL && target.destination.stages != nri::StageBits::NONE);
            nri::GlobalBarrierDesc expected{source[0].source, target.destination};
            auto wait = nri::StageBits::ALL;
            if (order == EReuseOrder::SHADER_READER)
            {
                wait = nri::StageBits::COMPUTE_SHADER;
                CHECK(!coversStages(wait, target.destination.stages));
                CHECK(target.waited_through[1] == kGraphInvalidIndex);
                expected.before.stages = expected.after.stages = nri::StageBits::ALL;
                expected.after.access = target.source.access;
            }
            else
            {
                const auto waited = target.waited_through[1];
                CHECK(waited != kGraphInvalidIndex && waited >= source[1].last_submission);
                expected.before.stages = combineStages(source[0].source.stages, target.destination.stages);
            }
            handoff_probe.emplace(*session, expected, wait);
        }
    }
    for (std::uint32_t slot = 0; slot < fif; ++slot)
    {
        CHECK(
            (instance_job->resourceInput(a, slot).buffer == instance_job->resourceInput(b, slot).buffer) == (reuse && !alias_memory)
        );
        for (std::uint32_t previous = 0; previous < slot; ++previous)
        {
            CHECK(instance_job->resourceInput(a, slot).buffer != instance_job->resourceInput(a, previous).buffer);
        }
    }
    const std::array instances{instance_job.get()};
    for (std::uint32_t frame = 0; frame < fif + 2; ++frame)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (instance_job->progress.count(EExecutionOutcome::EXECUTED) <= frame &&
               std::chrono::steady_clock::now() < deadline)
        {
            take(runTask(frames->render(instances, {})));
            CHECK(session->progressCompletions());
            if (instance_job->progress.count(EExecutionOutcome::EXECUTED) <= frame)
            {
                take(frames->waitForProgress(1'000'000));
            }
        }
        CHECK(instance_job->progress.count(EExecutionOutcome::EXECUTED) == frame + 1);
        if (handoff_probe)
        {
            CHECK(handoff_probe->observed());
            handoff_probe.reset();
        }
    }
    CHECK(session->drain());
    const auto slot = instance_job->progress.last_submitted_slot;
    const auto left = readBufferWords<4>(*session, *instance_job->resourceInput(output_a, slot).buffer);
    const auto right = readBufferWords<4>(*session, *instance_job->resourceInput(output_b, slot).buffer);
    CHECK(std::ranges::all_of(left, [](auto value) { return value == 4; }));
    CHECK(std::ranges::all_of(right, [](auto value) { return value == 8; }));
    if (witness)
    {
        const auto bytes = readBufferWords<4>(*session, *instance_job->resourceInput(*witness, slot).buffer);
        CHECK(std::ranges::all_of(bytes, [](auto value) { return value == 5; }));
    }
    take(runTask(frames->render(instances, {})));
    finishSubmissions(*session, *frames);
    instance_job.reset(); // Accepted GPU work retains the unique physical collection through cleanup_scope_.
    instance_gpu.reset();
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
    const auto& transitions = plan.graph.passes().front().before.images;
    CHECK(transitions.size() == 1);
    const auto& transition = transitions.front();
    CHECK(transition.resource == image.index && transition.previous_slot == kGraphInvalidIndex);
    CHECK(transition.native.mipOffset == 0 && transition.native.mipNum == 4);
    CHECK(transition.native.layerOffset == 0 && transition.native.layerNum == 3);
    CHECK(transition.native.before.layout == nri::Layout::UNDEFINED);
    CHECK(transition.native.after.layout == nri::Layout::COLOR_ATTACHMENT);
    auto instance_gpu = std::make_unique<JobGpuResources>(*session, fif);
    auto instance_job = std::make_unique<JobExecution>(
        *instance_gpu, plan.graph, testInstallation(), TargetOutput{}
    );
    CHECK(instance_job->initialize());
    const std::array instances{instance_job.get()};
    for (std::uint32_t frame = 0; frame < fif; ++frame)
    {
        CHECK(take(runTask(frames->render(instances, {}))) != 0);
        finishSubmissions(*session, *frames);
        CHECK(session->drain());
        checkPixels(*session, *instance_job->resourceInput(output, frame).buffer, 32, 0, {64, 128, 191, 255}, 256);
    }
    instance_job.reset();
    instance_gpu.reset();
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
        cpuBuilderTransactions();
        cpuRecordingTables();
        cpuConditionalRanges();
        cpuConditionalJoins();
        cpuValueChoice();
        cpuUndefinedChoice();
        cpuChoiceImageRanges();
        cpuImportedBoundaries();
        cpuRasterPackets();
        cpuQueueBoundaries();
        cpuQueueCandidates();
        cpuAliasBufferRanges();
        cpuAliasWaitScopes();
        cpuRasterBindingReplacement();
        detachedDirtyRanges();
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
        const bool d3d12 = argc > 2 && std::string_view(argv[2]) == "d3d12";
        frozenProjectionReceipts(validation, d3d12);
        recordingOwnership(validation, d3d12);
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
        constantLayoutEvidence(validation, d3d12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN);
        CHECK(validation.errors == 0);
        return 0;
    }
    const auto backend = argc > 2 && std::string_view(argv[2]) == "d3d12"
        ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    if (argc > 1 && std::string_view(argv[1]) == "alias-frontiers")
    {
        layeredAliasHandoff(validation, backend);
        DeviceOptions options;
        options.backend = backend;
        options.multiple_queues = true;
        options.prefer_dedicated_queues = true;
        {
            TestDevice device(options, diagnostics(validation), nativeValidationEnabled());
            if (&device->queue() != &device->queue(EQueueRole::COMPUTE))
            {
                crossQueueImageHandoff(*device, backend, true);
                crossQueueImageHandoff(*device, backend, false);
            }
        }
        physicalReuse(validation, options, 2, {EReuseOrder::EXPLICIT, true});
        physicalReuse(validation, options, 2, {EReuseOrder::UNORDERED, true});
        physicalReuse(validation, options, 2, {EReuseOrder::SHADER_READER, true});
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "choices")
    {
        for (auto fif : {1u, 2u, 3u}) nativeChoices(validation, backend, fif);
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "local-freeze")
    {
        DeviceOptions options;
        options.backend = backend;
        TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
        localFrozenInputs(*session, backend, 4);
        localFrozenInputs(*session, backend, 32);
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "batch-reuse")
    {
        nativeQueueCandidates(validation, backend, true);
        CHECK(validation.errors == 0);
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "copies")
    {
        frameUploadSnapshot(validation, backend);
        resolveAndCopy(validation, backend);
        compressedCopies(validation, backend);
        indirectCounts(validation, backend);
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
        nativeChoices(validation, backend, fif);
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
            physicalReuse(validation, options, fif, {EReuseOrder::EXPLICIT});
            physicalReuse(validation, options, fif, {EReuseOrder::UNORDERED});
            physicalReuse(validation, options, fif, {EReuseOrder::EXPLICIT, true});
            physicalReuse(validation, options, fif, {EReuseOrder::UNORDERED, true});
        }
    }
    nativeQueueCandidates(validation, backend);
    frameUploadSnapshot(validation, backend);
    indirectCounts(validation, backend);
    resolveAndCopy(validation, backend);
    compressedCopies(validation, backend);
    sharedImportedBuffer(validation, backend, false);
    sharedImportedBuffer(validation, backend, true);
    sharedImportedBuffer(validation, backend, false, true);
    CHECK(validation.errors == 0);
    std::printf(
        "Graph %s: Compute -> mip -> MRT/Depth -> sampled postprocess; FIF=1/2/3; GPU IndirectCount=0/1/3; errors=%u "
        "warnings=%u\n",
        backend == EGraphicsBackend::D3D12 ? "D3D12" : "Vulkan",
        validation.errors.load(),
        validation.warnings.load()
    );
}
