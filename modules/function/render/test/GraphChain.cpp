#include "Support.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/OffscreenTarget.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/GraphCompiler.hpp>
#include <lux/engine/render/graph/PlanDiagnostics.hpp>
#include <lux/engine/render/graph/RenderGraphBuilder.hpp>
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

static rdesc::SpirvArtifact program(
    rdesc::EShaderType type,
    std::span<const std::uint32_t> words,
    std::span<const std::byte> reflection
)
{
    rdesc::SpirvArtifact result;
    result.stages.push_back({type, "main", {words.begin(), words.end()}});
    CHECK(rdesc::ShaderInfo::deserialize(reflection, result.interface));
    return result;
}

static rdesc::SpirvArtifact rasterProgram(
    std::span<const std::uint32_t> fragment,
    std::span<const std::byte> reflection
)
{
    auto result = program(rdesc::EShaderType::FRAGMENT, fragment, reflection);
    auto vertex = program(rdesc::EShaderType::VERTEX, kGraphTriangle, kGraphTriangleReflection);
    result.stages.insert(result.stages.begin(), std::move(vertex.stages.front()));
    result.interface.entry_points.insert(
        result.interface.entry_points.begin(),
        vertex.interface.entry_points.begin(),
        vertex.interface.entry_points.end()
    );
    return result;
}

static RenderPlan compile(
    DeviceSession& session,
    const RenderGraph& graph,
    const RenderGraphAnalysis& analysis,
    const TargetDesc* target
)
{
    return take(GraphCompiler::compile(
        session.device(),
        session.nriCore(),
        session.enabledFeatures(),
        session.formatSupport(),
        {session.vertexStrideLimit(), session.vertexOffsetLimit()},
        {&session.queue(EQueueRole::GRAPHICS).properties(),
         &session.queue(EQueueRole::COMPUTE).properties(),
         &session.queue(EQueueRole::TRANSFER).properties()},
        0,
        0,
        graph,
        analysis,
        target
    ));
}

static ResourceVersion written(const PassResult& pass, GraphResourceId resource)
{
    const auto found = std::ranges::find(pass.written, resource, &ResourceVersion::resource);
    CHECK(found != pass.written.end());
    return *found;
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
    CHECK(session.queue().recordAndSubmit(
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

static void imageChain(Validation& validation, std::uint32_t fif, bool async_compute = false)
{
    DeviceOptions options;
    options.multiple_queues = async_compute;
    auto session = take(DeviceSession::create(options, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
    RenderGraphBuilder builder;
    GraphImageDesc image{64, 64};
    image.extent.target_relative = true;
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
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphFill, kGraphFillReflection)));
    compute.interface.resources = {{"generated", {{seed, 0}, GraphImageRange{1, 1, 1}, EGraphAccess::WRITE}}};
    compute.dispatch = DispatchArguments{{8, 8, 1}};
    const auto generated = take(builder.addComputePass("generate mip 1", std::move(compute)));
    RasterWork mrt;
    mrt.interface.program = take(builder.addProgram(rasterProgram(kGraphMrt, kGraphMrtReflection)));
    mrt.interface.resources = {
        {"source_image", {generated.written.front(), GraphImageRange{1, 1, 1}}},
        {"tint", {tint_pass.written.front()}}
    };
    mrt.interface.samplers = {{"source_sampler", {0}}};
    mrt.colors = {{{{first, 0}}}, {{{second, 0}}}};
    mrt.depth = RasterAttachment{{{depth, 0}, GraphImageRange{2}}};
    mrt.graphics.depth_test = mrt.graphics.depth_write = true;
    const auto raster = take(builder.addRasterPass("MRT and depth", std::move(mrt)));
    RasterWork present;
    present.interface.program = take(builder.addProgram(rasterProgram(kGraphPresent, kGraphPresentReflection)));
    present.interface.resources = {
        {"colors", {written(raster, first)}, {{written(raster, second)}}},
        {"depth_image", {written(raster, depth), GraphImageRange{2}}}
    };
    present.interface.samplers = {{"source_sampler", {0}}};
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
    auto physical = take(OffscreenTarget::create(*session, target, fif));
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan, physical.get());
    CHECK(instance->bindSampler(0, *sampler));
    CHECK(instance->initialize());
    CHECK(instance->dumpResources(sink));
    CHECK(dump.find("\"allocation_requirement_bytes\":") != std::string::npos);
    if (const auto* path = std::getenv("LUX_GRAPH_DUMP"))
    {
        std::ofstream output(path, std::ios::app);
        output << dump << '\n';
        CHECK(output.good());
    }
    auto larger_target = target;
    larger_target.width = larger_target.height = 128;
    CHECK(plan.compatible(larger_target));
    auto larger = take(OffscreenTarget::create(*session, larger_target, fif));
    auto second_instance = std::make_unique<RenderGraphInstance>(*session, fif, plan, larger.get());
    CHECK(second_instance->bindSampler(0, *sampler));
    CHECK(second_instance->initialize());
    const std::array instances{instance.get(), second_instance.get()};
    const auto submit = [&](std::span<RenderGraphInstance* const> selected)
    {
        auto submitted = take(frames->render(selected, {}));
        for (unsigned attempts = 0; frames->submitting() && attempts < 10000; ++attempts)
        {
            CHECK(session->pollQueues());
            session->dispatchQueueCompletions();
            submitted += take(frames->render({}, {}));
            std::this_thread::yield();
        }
        CHECK(!frames->submitting());
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
        CHECK(take(frames->render(instances, {})) == 0);
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

static void indirectCounts(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 3));
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
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphCount, kGraphCountReflection)));
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
    draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, kGraphRedReflection)));
    draw.colors = {{{{color, 0}}, EAttachmentLoad::CLEAR, EAttachmentStore::STORE, {0, 0, 0, 1}}};
    draw.draw = DrawIndexedIndirect{
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
    auto instance = std::make_unique<RenderGraphInstance>(*session, 3, plan);
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
    for (std::uint32_t requested : {0u, 1u, 3u})
    {
        requested_value = requested;
        CHECK(take(frames->render(instances, {})) == 1);
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

static void resolveAndCopy(Validation& validation)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, 2));
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
    draw.interface.program = take(builder.addProgram(rasterProgram(kGraphRed, kGraphRedReflection)));
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
    auto instance = std::make_unique<RenderGraphInstance>(*session, 2, plan);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    CHECK(take(frames->render(instances, {})) == 1);
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
    CHECK(session.queue().recordAndSubmit(
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

static void historySequence(Validation& validation, std::uint32_t fif, bool use_fallback)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
    RenderGraphBuilder builder;
    const auto history = take(builder.declareHistory("samples", GraphBufferDesc{16}));
    const auto enabled = take(builder.declareCondition("update"));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{36}}));
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
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphHistory, kGraphHistoryReflection)));
    work.interface.resources = {
        {"previous_sample", {{history.previous, 0}, {}, EGraphAccess::READ, 0, shader_fallback}},
        {"current_sample", {cleared.written.front(), {}, EGraphAccess::WRITE, 0, cleared.written.front()}}
    };
    const auto incremented = take(
        builder.addComputePass("increment", std::move(work), {false, PassCondition{EConditionSource::INPUT, enabled}})
    );
    CHECK(builder.publishHistory(incremented.written.front()));
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
             CopyBuffer{{valid_marker.written.front()}, {{output, 0}, GraphBufferRange{32, 4}}}}
        }
    ));
    CHECK(builder.exportOutput({observed.written.front()}));
    const auto graph = take(std::move(builder).finish());
    const auto analysis = take(analyzeRenderGraph(graph));
    auto plan = compile(*session, graph, analysis, nullptr);
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    std::uint32_t sequence = 0;
    const std::uint8_t enabled_initial = 1;
    CHECK(instance->setConditions({&enabled_initial, 1}));
    CHECK(take(instance->prepareFrame(0)));
    const auto rejected = session->queue().recordAndSubmit(
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
            sequence = 0;
        }
        const std::uint8_t enabled_value = execution != 3 && execution != 4 && execution != 10;
        CHECK(instance->setConditions({&enabled_value, 1}));
        const auto previous = sequence;
        if (enabled_value)
        {
            if (!sequence && use_fallback)
            {
                sequence = 17;
            }
            ++sequence;
        }
        const auto submitted = take(frames->render(instances, {}));
        CHECK(submitted == 1);
        const auto slot = instance->progress().last_submitted_slot;
        CHECK(session->drain());
        const auto values = readHistoryResult(*session, *instance->nativeBuffer(output.index, slot));
        for (unsigned i = 0; i < 4; ++i)
        {
            CHECK(values[i] == (enabled_value ? sequence : 0));
            CHECK(values[i + 4] == (!previous && use_fallback ? 29 : previous));
        }
        CHECK(values[8] == unsigned(previous != 0));
    }
    // Several accepted frames coexist; history selection follows accepted samples, not slot parity.
    const std::uint8_t enabled_value = 1;
    CHECK(instance->setConditions({&enabled_value, 1}));
    for (std::uint32_t i = 0; i < fif; ++i)
    {
        if (!take(frames->render(instances, {})))
        {
            CHECK(session->drain());
            CHECK(take(frames->render(instances, {})) == 1);
        }
        ++sequence;
    }
    CHECK(session->drain());
    const auto values =
        readHistoryResult(*session, *instance->nativeBuffer(output.index, instance->progress().last_submitted_slot));
    CHECK(values[0] == sequence);
    instance.reset();
    CHECK(session->drain());
}

static void hzbHistory(Validation& validation, std::uint32_t fif)
{
    DeviceOptions options;
    options.multiple_queues = true;
    auto session = take(DeviceSession::create(options, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
    RenderGraphBuilder builder;
    GraphImageDesc image{9, 7, 1, 1, rdesc::ETextureFormat::R32_SFLOAT};
    image.extent.target_relative = true;
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
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphDepthField, kGraphDepthFieldReflection)));
    seed.interface.resources = {{"generated", {{source, 0}, {}, EGraphAccess::WRITE}}};
    seed.dispatch = ImageDispatchArguments{source};
    const auto generated = take(builder.addComputePass("generate depth field", std::move(seed)));
    const auto reduce =
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kHzbDownsample, kHzbDownsampleReflection)));
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
    auto physical = take(OffscreenTarget::create(*session, target, fif));
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan, physical.get());
    CHECK(instance->initialize());
    for (std::uint32_t execution = 0; execution < 4; ++execution)
    {
        if (execution == 2)
        {
            // Retire an in-flight old instance while preparing a larger candidate from the same immutable plan.
            const std::array old{instance.get()};
            CHECK(take(frames->render(old, {})) > 0 && !frames->submitting());
            target.width = 17;
            target.height = 11;
            auto replacement = take(OffscreenTarget::create(*session, target, fif));
            auto candidate = std::make_unique<RenderGraphInstance>(*session, fif, plan, replacement.get());
            CHECK(candidate->initialize());
            instance = std::move(candidate);
            physical = std::move(replacement);
            CHECK(session->drain());
        }
        const std::array instances{instance.get()};
        CHECK(take(frames->render(instances, {})) > 0 && !frames->submitting());
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

static void conditionalRegion(Validation& validation, std::uint32_t fif)
{
    auto session = take(DeviceSession::create({}, diagnostics(validation), true, 3));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
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
    auto instance = std::make_unique<RenderGraphInstance>(*session, fif, plan);
    CHECK(instance->initialize());
    const std::array instances{instance.get()};
    for (const std::uint8_t enabled : {0, 1, 0, 1})
    {
        CHECK(instance->setConditions({&enabled, 1}));
        CHECK(take(frames->render(instances, {})) == 1);
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

static void sharedImportedBuffer(Validation& validation, bool write_back)
{
    DeviceOptions options;
    options.multiple_queues = true;
    auto session = take(DeviceSession::create(options, diagnostics(validation), true, 1));
    auto frames = take(FrameExecution::create(*session, nullptr, 2));
    const auto& core = session->nriCore();
    nri::Buffer* shared = nullptr;
    CHECK(
        core.CreatePlacedBuffer(
            session->device(),
            NriDeviceHeap,
            {16, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE},
            shared
        ) == nri::Result::SUCCESS
    );
    CHECK(session->queue().recordAndSubmit(
        [&](nri::CommandBuffer& command, std::uint32_t) noexcept -> RenderResult<void>
        {
            core.CmdZeroBuffer(command, *shared, 0, 16);
            const nri::BufferBarrierDesc barrier{
                shared,
                {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY | nri::StageBits::CLEAR_STORAGE},
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
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphHistory, kGraphHistoryReflection)));
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
    auto first = std::make_unique<RenderGraphInstance>(*session, 2, plan);
    auto second = std::make_unique<RenderGraphInstance>(*session, 2, plan);
    for (auto* instance : {first.get(), second.get()})
    {
        instance->setResource(input.index, UINT32_MAX, {shared, 0, 16});
        CHECK(instance->initialize());
    }
    const std::array instances{first.get(), second.get()};
    const auto before = session->queue(EQueueRole::COMPUTE).submitted();
    CHECK(take(frames->render(instances, {})) > 0);
    for (unsigned attempt = 0; frames->submitting() && attempt < 10000; ++attempt)
    {
        CHECK(session->pollQueues());
        session->dispatchQueueCompletions();
        take(frames->render({}, {}));
        std::this_thread::yield();
    }
    CHECK(!frames->submitting());
    if (&session->queue() != &session->queue(EQueueRole::COMPUTE))
    {
        CHECK(session->queue(EQueueRole::COMPUTE).submitted() - before == (write_back ? 2 : 1));
    }
    CHECK(session->drain());
    const auto a =
        readHistoryResult<4>(*session, *first->nativeBuffer(output.index, first->progress().last_submitted_slot));
    const auto b =
        readHistoryResult<4>(*session, *second->nativeBuffer(output.index, second->progress().last_submitted_slot));
    CHECK(a[0] == 1 && b[0] == (write_back ? 2 : 1));
    first.reset();
    second.reset();
    CHECK(session->drain());
    core.DestroyBuffer(shared);
}

static void multiQueueHistory(Validation& validation, DeviceOptions options, std::uint32_t fif)
{
    auto session = take(DeviceSession::create(options, diagnostics(validation), true, 1));
    auto frames = take(FrameExecution::create(*session, nullptr, fif));
    RenderGraphBuilder builder;
    const auto history = take(builder.declareHistory("samples", GraphBufferDesc{16}));
    const auto temporary = take(builder.declareResource({"temporary", GraphBufferDesc{16}}));
    const auto initial = take(builder.declareResource({"initial", GraphBufferDesc{16}}));
    const auto output = take(builder.declareResource({"observed", GraphBufferDesc{16}}));
    const auto seeded = take(builder.addTransferPass("seed", TransferWork{{FillBuffer{{{initial, 0}}, 1}}}));
    const auto shader =
        take(builder.addProgram(program(rdesc::EShaderType::COMPUTE, kGraphHistory, kGraphHistoryReflection)));
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
    CHECK(plan.submissions().size() == (separate ? 7 : 1));
    auto first = std::make_unique<RenderGraphInstance>(*session, fif, plan);
    auto second = std::make_unique<RenderGraphInstance>(*session, fif, plan);
    CHECK(first->initialize() && second->initialize());
    const std::array instances{first.get(), second.get()};
    for (unsigned frame = 0; frame < 2; ++frame)
    {
        const auto before_graphics = session->queue().submitted();
        const auto before_compute = session->queue(EQueueRole::COMPUTE).submitted();
        CHECK(take(frames->render(instances, {})) != 0);
        if (separate)
        {
            CHECK(frames->submitting(*first) && frames->submitting(*second));
            CHECK(first->progress().count(EExecutionOutcome::EXECUTED) == frame);
            if (frame == 0)
            {
                CHECK(first->invalidateHistory());
            }
            for (unsigned progress = 0; frames->submitting() && progress < 10000; ++progress)
            {
                CHECK(session->pollQueues());
                session->dispatchQueueCompletions();
                take(frames->render({}, {})); // Continue the frozen window, not a fresh preparation.
                std::this_thread::yield();
            }
            CHECK(!frames->submitting());
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
    }
    if (separate)
    {
        CHECK(take(frames->render(instances, {})) != 0);
        CHECK(frames->submitting());
        frames->cancelSubmissions(RenderError{kDeviceArgument});
        CHECK(first->progress().count(EExecutionOutcome::EXECUTED) == 2);
        CHECK(!frames->submitting() && !first->prepareFrame(0));
    }
    first.reset();
    second.reset();
    CHECK(session->drain());
    const auto old_serial = session->queue().submitted();
    CHECK(session->queue().recordAndSubmit(
        [](nri::CommandBuffer&, std::uint32_t) noexcept -> RenderResult<void> { return {}; }
    ));
    const auto dependency = take(session->queue().dependency(old_serial, nri::StageBits::ALL));
    CHECK(dependency.value == old_serial && dependency.value != session->queue().submitted());
    CHECK(!session->queue().dependency(session->queue().submitted() + 1, nri::StageBits::ALL));
    CHECK(session->drain());
}

int main()
{
    Validation validation;
    for (std::uint32_t fif : {1u, 2u, 3u})
    {
        imageChain(validation, fif);
        imageChain(validation, fif, true);
        historySequence(validation, fif, false);
        historySequence(validation, fif, true);
        conditionalRegion(validation, fif);
        hzbHistory(validation, fif);
        for (unsigned topology = 0; topology < 3; ++topology)
        {
            DeviceOptions options;
            options.multiple_queues = topology != 0;
            options.prefer_dedicated_queues = topology == 2;
            multiQueueHistory(validation, options, fif);
        }
    }
    indirectCounts(validation);
    resolveAndCopy(validation);
    sharedImportedBuffer(validation, false);
    sharedImportedBuffer(validation, true);
    CHECK(validation.errors == 0);
    std::printf(
        "Graph: Compute -> mip -> MRT/Depth -> sampled postprocess; FIF=1/2/3; GPU IndirectCount=0/1/3; errors=%u "
        "warnings=%u\n",
        validation.errors.load(),
        validation.warnings.load()
    );
}
